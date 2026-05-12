/* wows-stitch: stitch WoWS ship geometry parts into a single GLB.
 *
 * Uses libpython (via Python C API) exclusively for GameParams.data
 * (byte-reversed zlib-compressed pickle); everything else is C/C++.
 *
 * Pipeline:
 *   1. Python C API  → embedded game_params.py → hull model + mounts
 *   2. assets_bin.cpp → HP_ world-space transforms + BlendBone corrections
 *   3. libwows-geometry → per-part geometry parsed directly into tinygltf::Model
 *   4. tinygltf → merge all parts, write output
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <tiny_gltf.h>

extern "C" {
#include "wows-geometry.h"
#include "assets_bin.h"
}

#include <argp.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize2.h>

/* ── embedded game_params.py ─────────────────────────────────────── */

static const char GAME_PARAMS_PY[] = R"PYTHON(
import io, pickle, zlib, sys
from typing import Any

COMPONENT_TYPES = [
    "hull", "artillery", "atba", "airDefense",
    "directors", "finders", "radars", "torpedoes",
]

class _Stub:
    def __init__(self, *args, **kwargs): pass
    def get(self, key, default=None): return self.__dict__.get(key, default)
    def __getitem__(self, key): return self.__dict__[key]
    def __setitem__(self, key, val): self.__dict__[key] = val
    def __contains__(self, key): return key in self.__dict__
    def __iter__(self): return iter(self.__dict__)
    def items(self): return self.__dict__.items()
    def keys(self): return self.__dict__.keys()
    def values(self): return self.__dict__.values()

def _to_plain(obj, _memo=None):
    if _memo is None: _memo = {}
    oid = id(obj)
    if oid in _memo: return _memo[oid]
    if isinstance(obj, _Stub):
        result = {}
        _memo[oid] = result
        result.update({str(k): _to_plain(v, _memo) for k, v in obj.__dict__.items()})
        return result
    if isinstance(obj, dict):
        result = {}
        _memo[oid] = result
        result.update({str(k) if not isinstance(k, str) else k: _to_plain(v, _memo)
                       for k, v in obj.items()})
        return result
    if isinstance(obj, list):
        result_list = []
        _memo[oid] = result_list
        result_list.extend(_to_plain(v, _memo) for v in obj)
        return result_list
    if isinstance(obj, tuple):
        return tuple(_to_plain(v, _memo) for v in obj)
    return obj

class _PermissiveUnpickler(pickle.Unpickler):
    def find_class(self, module, name):
        try: return super().find_class(module, name)
        except Exception: return _Stub

def load_game_params(path):
    with open(path, "rb") as f: data = f.read()
    data = data[::-1]
    data = zlib.decompress(data)
    raw = _PermissiveUnpickler(io.BytesIO(data)).load()
    return _to_plain(raw)

def get_params_root(game_params):
    if isinstance(game_params, dict):
        if "" in game_params:
            inner = game_params[""]
            if isinstance(inner, dict): return inner
        return game_params
    if isinstance(game_params, (list, tuple)):
        first = game_params[0]
        if isinstance(first, dict): return first
    return {}

def _as_list(val):
    if val is None: return []
    if isinstance(val, str): return [val]
    if isinstance(val, (list, tuple)): return list(val)
    return []

def extract_ship(ship_name, ship_data):
    result = {"name": ship_name, "hull_upgrades": {}}
    upgrade_info = ship_data.get("ShipUpgradeInfo", {})
    if not isinstance(upgrade_info, dict): return result
    for upgrade_name, upgrade_data in upgrade_info.items():
        if not isinstance(upgrade_data, dict): continue
        if upgrade_data.get("ucType") != "_Hull": continue
        components = upgrade_data.get("components", {})
        if not isinstance(components, dict): continue
        hull_comp_names = _as_list(components.get("hull"))
        if not hull_comp_names: continue
        hull_comp_name = hull_comp_names[0]
        hull_data = ship_data.get(hull_comp_name, {})
        if not isinstance(hull_data, dict): continue
        hull_model = hull_data.get("model", "")
        mounts = {}
        for ct in COMPONENT_TYPES:
            comp_names = _as_list(components.get(ct))
            for comp_name in comp_names:
                comp_data = ship_data.get(comp_name, {})
                if not isinstance(comp_data, dict): continue
                for key, val in comp_data.items():
                    if not key.startswith("HP_"): continue
                    if not isinstance(val, dict): continue
                    model_path = val.get("model", "")
                    if not model_path: continue
                    mounts[key] = {"model": model_path, "component": comp_name,
                                   "component_type": ct}
        if hull_model or mounts:
            result["hull_upgrades"][upgrade_name] = {
                "hull_component": hull_comp_name,
                "hull_model": hull_model,
                "mounts": mounts,
            }
    return result
)PYTHON";

/* ── string helpers ───────────────────────────────────────────────── */

static std::string path_basename(const std::string &p) {
    auto sl = p.rfind('/');
    return (sl == std::string::npos) ? p : p.substr(sl + 1);
}

static std::string path_dirname(const std::string &p) {
    auto sl = p.rfind('/');
    return (sl == std::string::npos) ? "." : p.substr(0, sl);
}

static std::string stem(const std::string &filename) {
    auto dot = filename.rfind('.');
    return (dot == std::string::npos) ? filename : filename.substr(0, dot);
}

static std::string normalize_slashes(std::string s) {
    for (auto &c : s) if (c == '\\') c = '/';
    return s;
}

static bool file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/* ── model path helpers ───────────────────────────────────────────── */

/* .model → .geometry, prepend game_dir */
static std::string model_to_geom_path(const std::string &model, const std::string &game_dir) {
    std::string rel = normalize_slashes(model);
    auto pos = rel.rfind(".model");
    if (pos != std::string::npos) rel.replace(pos, 6, ".geometry");
    return normalize_slashes(game_dir + "/" + rel);
}

/* .geometry path → "Dir/File.visual" suffix for assets.bin lookup */
static std::string geom_to_visual_suffix(const std::string &geom_path) {
    std::string v = normalize_slashes(geom_path);
    auto pos = v.rfind(".geometry");
    if (pos != std::string::npos) v.replace(pos, 9, ".visual");
    auto sl1 = v.rfind('/');
    if (sl1 != std::string::npos && sl1 > 0) {
        auto sl2 = v.rfind('/', sl1 - 1);
        if (sl2 != std::string::npos) return v.substr(sl2 + 1);
    }
    return v;
}

/* ── geometry file discovery ──────────────────────────────────────── */

static std::vector<std::string> find_hull_geoms(const std::string &hull_model,
                                                const std::string &game_dir) {
    std::string geom_path = model_to_geom_path(hull_model, game_dir);
    if (!file_exists(geom_path)) return {};

    std::string ship_dir   = path_dirname(geom_path);
    std::string base_name  = stem(path_basename(geom_path));

    DIR *dir = opendir(ship_dir.c_str());
    if (!dir) return {};

    std::vector<std::string> results;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        std::string fname = ent->d_name;
        if (fname.size() <= 9) continue;
        if (fname.compare(fname.size() - 9, 9, ".geometry") != 0) continue;
        if (fname.compare(0, base_name.size(), base_name) != 0) continue;
        /* skip exact base (low-poly placeholder) */
        if (stem(fname) == base_name) continue;
        results.push_back(ship_dir + "/" + fname);
    }
    closedir(dir);
    std::sort(results.begin(), results.end());
    return results;
}

/* ── Python C API: load ship info from GameParams ─────────────────── */

struct MountEntry {
    std::string hp_name;
    std::string model_path;
};

struct HullInfo {
    std::string hull_model;
    std::vector<MountEntry> mounts;
};

/* Safe PyUnicode_AsUTF8 wrapper: returns "" on null/non-string */
static std::string py_str(PyObject *o) {
    if (!o || !PyUnicode_Check(o)) return "";
    const char *s = PyUnicode_AsUTF8(o);
    return s ? s : "";
}

static bool load_hull_info(const char *gameparams_path,
                           const char *ship_name,
                           const char *hull_sel, /* nullptr = first */
                           HullInfo &out) {
    /* compile and execute the embedded game_params.py into a fresh module */
    PyObject *mod = PyImport_AddModule("game_params");
    if (!mod) { PyErr_Print(); return false; }
    PyObject *ns = PyModule_GetDict(mod); /* borrowed ref */
    {
        PyObject *code = Py_CompileString(GAME_PARAMS_PY, "game_params.py", Py_file_input);
        if (!code) { PyErr_Print(); return false; }
        PyObject *r = PyEval_EvalCode(code, ns, ns);
        Py_DECREF(code);
        if (!r) { PyErr_Print(); return false; }
        Py_DECREF(r);
    }

    /* load_game_params(path) — mod is a borrowed ref, do not Py_DECREF it */
    PyObject *gp = PyObject_CallMethod(mod, "load_game_params", "s", gameparams_path);
    if (!gp) { PyErr_Print(); return false; }

    /* get_params_root(gp) */
    PyObject *root = PyObject_CallMethod(mod, "get_params_root", "O", gp);
    Py_DECREF(gp);
    if (!root) { PyErr_Print(); return false; }

    /* find ship: exact match first, then case-insensitive substring */
    std::string found_name = ship_name;
    PyObject *ship_data = PyDict_GetItemString(root, ship_name);
    if (!ship_data) {
        std::string needle = ship_name;
        for (auto &c : needle) c = (char)tolower((unsigned char)c);
        PyObject *pk, *pv;
        Py_ssize_t pos = 0;
        while (PyDict_Next(root, &pos, &pk, &pv)) {
            std::string k = py_str(pk);
            std::string kl = k;
            for (auto &c : kl) c = (char)tolower((unsigned char)c);
            if (kl.find(needle) != std::string::npos) {
                ship_data = pv;
                found_name = k;
                break;
            }
        }
    }
    if (!ship_data) {
        fprintf(stderr, "Ship '%s' not found in GameParams.\n", ship_name);
        Py_DECREF(root); Py_DECREF(mod);
        return false;
    }

    /* extract_ship(name, data) */
    PyObject *info = PyObject_CallMethod(mod, "extract_ship", "sO",
                                         found_name.c_str(), ship_data);
    Py_DECREF(root);
    if (!info) { PyErr_Print(); return false; }

    PyObject *upgrades = PyDict_GetItemString(info, "hull_upgrades");
    if (!upgrades || !PyDict_Check(upgrades)) {
        fprintf(stderr, "No hull_upgrades found.\n");
        Py_DECREF(info); return false;
    }

    /* select upgrade */
    PyObject *upg_data = nullptr;
    if (hull_sel) {
        std::string sel = hull_sel;
        for (auto &c : sel) c = (char)tolower((unsigned char)c);
        PyObject *pk, *pv;
        Py_ssize_t pos = 0;
        while (PyDict_Next(upgrades, &pos, &pk, &pv)) {
            std::string k = py_str(pk);
            std::string kl = k;
            for (auto &c : kl) c = (char)tolower((unsigned char)c);
            if (kl.find(sel) != std::string::npos) { upg_data = pv; break; }
        }
        if (!upg_data) {
            fprintf(stderr, "Hull upgrade '%s' not found.\n", hull_sel);
            Py_DECREF(info); return false;
        }
    } else {
        /* first upgrade in sorted order */
        PyObject *keys = PyDict_Keys(upgrades);
        PyList_Sort(keys);
        if (PyList_Size(keys) > 0) {
            PyObject *k = PyList_GetItem(keys, 0);
            upg_data = PyDict_GetItem(upgrades, k);
        }
        Py_DECREF(keys);
        if (!upg_data) {
            fprintf(stderr, "No hull upgrades available.\n");
            Py_DECREF(info); return false;
        }
    }

    out.hull_model = py_str(PyDict_GetItemString(upg_data, "hull_model"));

    PyObject *mounts = PyDict_GetItemString(upg_data, "mounts");
    if (mounts && PyDict_Check(mounts)) {
        PyObject *pk, *pv;
        Py_ssize_t pos = 0;
        while (PyDict_Next(mounts, &pos, &pk, &pv)) {
            std::string hp   = py_str(pk);
            std::string mdl  = py_str(PyDict_GetItemString(pv, "model"));
            if (!mdl.empty())
                out.mounts.push_back({hp, mdl});
        }
    }

    Py_DECREF(info);
    return true;
}

/* ── HP transform lookup helpers ──────────────────────────────────── */

using Mat16d = std::vector<double>;

static Mat16d mat4_mul_d(const Mat16d &a, const Mat16d &b) {
    Mat16d out(16, 0.0);
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                out[col*4+row] += a[k*4+row] * b[col*4+k];
    return out;
}

static Mat16d float_to_double_mat(const float m[16]) {
    Mat16d r(16);
    for (int i = 0; i < 16; ++i) r[i] = (double)m[i];
    return r;
}

/* ── GLB merge using tinygltf ─────────────────────────────────────── */

struct GlbPart {
    tinygltf::Model model;
    std::string     mesh_name;
    std::string     geom_path; /* source .geometry file */
    Mat16d          matrix;    /* empty → identity */
};

static tinygltf::Model merge_parts(std::vector<GlbPart> &parts) {
    tinygltf::Model merged;
    merged.asset.version   = "2.0";
    merged.asset.generator = "wows-geometry wows-stitch";
    merged.scenes.push_back({});
    merged.defaultScene = 0;

    std::vector<unsigned char> buf;

    for (auto &part : parts) {
        tinygltf::Model &m = part.model;

        /* 4-byte align merged buffer */
        while (buf.size() & 3) buf.push_back(0);
        size_t byte_off = buf.size();

        if (!m.buffers.empty())
            buf.insert(buf.end(), m.buffers[0].data.begin(),
                                  m.buffers[0].data.end());

        size_t bv_start  = merged.bufferViews.size();
        size_t acc_start = merged.accessors.size();

        for (const auto &bv : m.bufferViews) {
            tinygltf::BufferView nbv = bv;
            nbv.buffer     = 0;
            nbv.byteOffset += (int)byte_off;
            merged.bufferViews.push_back(nbv);
        }

        for (const auto &acc : m.accessors) {
            tinygltf::Accessor nacc = acc;
            if (nacc.bufferView >= 0) nacc.bufferView += (int)bv_start;
            merged.accessors.push_back(nacc);
        }

        for (const auto &mesh : m.meshes) {
            tinygltf::Mesh nmesh;
            nmesh.name = part.mesh_name;
            for (const auto &prim : mesh.primitives) {
                tinygltf::Primitive np = prim;
                for (auto &kv : np.attributes) kv.second += (int)acc_start;
                if (np.indices >= 0) np.indices += (int)acc_start;
                nmesh.primitives.push_back(np);
            }
            int mesh_idx = (int)merged.meshes.size();
            merged.meshes.push_back(nmesh);

            tinygltf::Node node;
            node.name = part.mesh_name;
            node.mesh = mesh_idx;
            if (!part.matrix.empty()) node.matrix = part.matrix;
            int node_idx = (int)merged.nodes.size();
            merged.nodes.push_back(node);
            merged.scenes[0].nodes.push_back(node_idx);
        }
    }

    tinygltf::Buffer b;
    b.data = std::move(buf);
    merged.buffers.push_back(std::move(b));
    return merged;
}

/* ── geometry → tinygltf::Model (direct, no temp file) ───────────── */

/* Inlined from lib/utils.c to avoid depending on internal.h */
static float stitch_f16_to_f32(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31;
    uint32_t e = (h >> 10) & 0x1fu;
    uint32_t m = h & 0x3ffu;
    uint32_t f;
    if (e == 0) {
        if (m == 0) { f = s; }
        else {
            e = 1;
            while (!(m & 0x400u)) { m <<= 1; e--; }
            m &= 0x3ffu;
            f = s | ((e + 112u) << 23) | (m << 13);
        }
    } else if (e == 31) {
        f = s | 0x7f800000u | (m << 13);
    } else {
        f = s | ((e + 112u) << 23) | (m << 13);
    }
    float r; memcpy(&r, &f, 4); return r;
}

static void stitch_unpack_normal(uint32_t packed, float *nx, float *ny, float *nz) {
    int8_t b[4]; memcpy(b, &packed, 4);
    *nx = b[0] / 127.0f; *ny = b[1] / 127.0f; *nz = b[2] / 127.0f;
}

static void stitch_unpack_uv(uint32_t packed, float *u, float *v) {
    uint16_t ub, vb;
    memcpy(&ub, (const uint8_t *)&packed + 0, 2);
    memcpy(&vb, (const uint8_t *)&packed + 2, 2);
    *u = stitch_f16_to_f32(ub) + 0.5f;
    *v = stitch_f16_to_f32(vb) + 0.5f;
}

/* Find the vertex-buffer base offset for a given index-bloc, mirroring
 * exporter.cpp find_vertex_base() logic exactly. */
static uint32_t find_vbase(const wows_geometry *g, uint32_t ibloc, uint16_t *vtype_out) {
    const wows_geometry_info *s2t = &g->section_2[ibloc];
    uint16_t ptd  = s2t->packed_texel_density;
    uint32_t icnt = s2t->items_count;
    uint32_t ioff = s2t->items_offset;
    uint32_t nv   = g->header->n_vertex_bloc;
    uint32_t ni   = g->header->n_index_bloc;
    *vtype_out = 0;

    uint32_t rank = 0;
    for (uint32_t i = 0; i < ni; ++i) {
        const wows_geometry_info *s2 = &g->section_2[i];
        if (s2->packed_texel_density != ptd) continue;
        if (s2->items_count > icnt || (s2->items_count == icnt && s2->items_offset < ioff))
            rank++;
    }

    uint32_t prev_cnt = UINT32_MAX, prev_off = UINT32_MAX;
    bool have_prev = false;
    for (uint32_t pass = 0; pass <= rank; ++pass) {
        uint32_t cur_cnt = 0, cur_off = UINT32_MAX; uint16_t cur_vt = 0;
        for (uint32_t j = 0; j < nv; ++j) {
            const wows_geometry_info *s1 = &g->section_1[j];
            if (s1->packed_texel_density != ptd) continue;
            uint32_t c = s1->items_count, o = s1->items_offset;
            if (have_prev && (c > prev_cnt || (c == prev_cnt && o <= prev_off))) continue;
            if (c > cur_cnt || (c == cur_cnt && o < cur_off)) {
                cur_cnt = c; cur_off = o; cur_vt = s1->merged_buffer_index;
            }
        }
        if (cur_off == UINT32_MAX) return 0;
        if (pass == rank) { *vtype_out = cur_vt; return cur_off; }
        prev_cnt = cur_cnt; prev_off = cur_off; have_prev = true;
    }
    return 0;
}

static bool geom_to_model(const std::string &geom_path, tinygltf::Model &model_out) {
    wows_geometry *geom = nullptr;
    std::vector<char> gp_buf(geom_path.begin(), geom_path.end());
    gp_buf.push_back('\0');
    if (wows_parse_geometry(gp_buf.data(), &geom) != 0) {
        fprintf(stderr, "  Warning: failed to parse %s\n", geom_path.c_str());
        return false;
    }

    uint32_t n_vt    = geom->header->n_vertex_type;
    uint32_t n_ibloc = geom->header->n_index_bloc;
    uint32_t n_it    = geom->header->n_index_type;

    std::vector<uint8_t> blob;
    blob.reserve(1 << 21);
    auto pad4   = [&]() { while (blob.size() & 3) blob.push_back(0); };
    auto append = [&](const void *src, size_t n) {
        const auto *p = static_cast<const uint8_t *>(src);
        blob.insert(blob.end(), p, p + n);
    };

    /* Phase 1: vertex buffers */
    struct VInfo { size_t pos_bv, norm_bv, uv_bv; uint32_t count;
                   float mn[3], mx[3]; };
    std::vector<VInfo> vinfo(n_vt);

    for (uint32_t k = 0; k < n_vt; ++k) {
        vinfo[k].count = 0;
        if (!geom->vertexes || !geom->vertexes[k] || !geom->vertexes[k]->raw_data) continue;
        uint32_t total  = geom->vertexes[k]->vertex_count;
        uint16_t stride = geom->vertex_meta_sections[k].s_vertex_size;
        const uint8_t *raw = geom->vertexes[k]->raw_data;

        std::vector<float> pos(total*3), norm(total*3), uv(total*2);
        float mn[3]={1e30f,1e30f,1e30f}, mx[3]={-1e30f,-1e30f,-1e30f};
        for (uint32_t j = 0; j < total; ++j) {
            const uint8_t *v = raw + (size_t)j * stride;
            float x, y, z; memcpy(&x, v, 4); memcpy(&y, v+4, 4); memcpy(&z, v+8, 4);
            pos[j*3]=x; pos[j*3+1]=y; pos[j*3+2]=z;
            for (int i=0;i<3;i++){float c=(i==0?x:i==1?y:z);if(c<mn[i])mn[i]=c;if(c>mx[i])mx[i]=c;}
            uint32_t pn, puv; memcpy(&pn, v+12, 4); memcpy(&puv, v+16, 4);
            stitch_unpack_normal(pn, &norm[j*3], &norm[j*3+1], &norm[j*3+2]);
            stitch_unpack_uv(puv, &uv[j*2], &uv[j*2+1]);
        }
        pad4(); vinfo[k].pos_bv = blob.size(); append(pos.data(), total*12);
        pad4(); vinfo[k].norm_bv = blob.size(); append(norm.data(), total*12);
        pad4(); vinfo[k].uv_bv  = blob.size(); append(uv.data(), total*8);
        vinfo[k].count = total;
        memcpy(vinfo[k].mn, mn, 12); memcpy(vinfo[k].mx, mx, 12);
    }

    /* Phase 2: index slices */
    struct IInfo { size_t bv_off; uint32_t count; uint16_t idx_size, vtype; };
    std::vector<IInfo> iinfo(n_ibloc);
    for (uint32_t i = 0; i < n_ibloc; ++i) {
        iinfo[i].count = 0;
        uint16_t ibuf = geom->section_2[i].merged_buffer_index;
        uint32_t ioff = geom->section_2[i].items_offset;
        uint32_t icnt = geom->section_2[i].items_count;
        if (ibuf >= n_it || !geom->indexes || !geom->indexes[ibuf] ||
            !geom->indexes[ibuf]->raw_data || ioff+icnt > geom->indexes[ibuf]->index_count)
            continue;
        uint16_t is = geom->indexes[ibuf]->index_size;
        const uint8_t *raw = geom->indexes[ibuf]->raw_data + (size_t)ioff * is;
        uint16_t vt; uint32_t vbase = find_vbase(geom, i, &vt);
        if (vt >= n_vt || !vinfo[vt].count) continue;
        iinfo[i].vtype = vt; iinfo[i].count = icnt;

        uint16_t out_is = is;
        if (is == 2) {
            uint32_t raw_max = 0;
            for (uint32_t j = 0; j < icnt; ++j) {
                uint16_t tv; memcpy(&tv, raw+j*2, 2); if((uint32_t)tv>raw_max) raw_max=tv;
            }
            if (raw_max + vbase > 65535u) out_is = 4;
        }
        iinfo[i].idx_size = out_is;
        pad4(); iinfo[i].bv_off = blob.size();
        for (uint32_t j = 0; j < icnt; ++j) {
            uint32_t v = 0;
            if (is==2){uint16_t t;memcpy(&t,raw+j*2,2);v=t;} else memcpy(&v,raw+j*4,4);
            uint32_t vo = v + vbase;
            if (out_is==4) append(&vo,4); else {uint16_t vo16=(uint16_t)vo;append(&vo16,2);}
        }
    }
    pad4();
    wows_geometry_free(geom);

    /* Phase 3: populate tinygltf model */
    tinygltf::Buffer tbuf; tbuf.data = std::move(blob);
    model_out.buffers.push_back(std::move(tbuf));
    model_out.asset.version = "2.0";
    model_out.asset.generator = "wows-geometry";

    auto add_bv = [&](size_t off, size_t len, int tgt) -> int {
        tinygltf::BufferView bv; bv.buffer=0; bv.byteOffset=off; bv.byteLength=len; bv.target=tgt;
        model_out.bufferViews.push_back(bv); return (int)model_out.bufferViews.size()-1;
    };
    auto add_acc = [&](int bv, int comp, int type, int cnt) -> int {
        tinygltf::Accessor ac; ac.bufferView=bv; ac.byteOffset=0;
        ac.componentType=comp; ac.type=type; ac.count=cnt;
        model_out.accessors.push_back(ac); return (int)model_out.accessors.size()-1;
    };

    std::vector<int> pos_acc(n_vt,-1), norm_acc(n_vt,-1), uv_acc(n_vt,-1);
    for (uint32_t k = 0; k < n_vt; ++k) {
        if (!vinfo[k].count) continue;
        uint32_t vc = vinfo[k].count;
        int bvp  = add_bv(vinfo[k].pos_bv,  vc*12, TINYGLTF_TARGET_ARRAY_BUFFER);
        int bvn  = add_bv(vinfo[k].norm_bv,  vc*12, TINYGLTF_TARGET_ARRAY_BUFFER);
        int bvu  = add_bv(vinfo[k].uv_bv,   vc*8,  TINYGLTF_TARGET_ARRAY_BUFFER);
        pos_acc[k]  = add_acc(bvp, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vc);
        norm_acc[k] = add_acc(bvn, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vc);
        uv_acc[k]   = add_acc(bvu, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, vc);
        auto &pa = model_out.accessors[pos_acc[k]];
        pa.minValues = {vinfo[k].mn[0], vinfo[k].mn[1], vinfo[k].mn[2]};
        pa.maxValues = {vinfo[k].mx[0], vinfo[k].mx[1], vinfo[k].mx[2]};
    }

    tinygltf::Mesh mesh;
    for (uint32_t i = 0; i < n_ibloc; ++i) {
        if (!iinfo[i].count) continue;
        uint16_t k = iinfo[i].vtype;
        if (k >= n_vt || pos_acc[k] < 0) continue;
        int bvi = add_bv(iinfo[i].bv_off, (size_t)iinfo[i].count * iinfo[i].idx_size,
                         TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        int comp = (iinfo[i].idx_size == 2) ? TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT
                                             : TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
        int ia = add_acc(bvi, comp, TINYGLTF_TYPE_SCALAR, iinfo[i].count);
        tinygltf::Primitive prim;
        prim.attributes["POSITION"]   = pos_acc[k];
        prim.attributes["NORMAL"]     = norm_acc[k];
        prim.attributes["TEXCOORD_0"] = uv_acc[k];
        prim.indices = ia;
        prim.mode    = TINYGLTF_MODE_TRIANGLES;
        mesh.primitives.push_back(prim);
    }
    model_out.meshes.push_back(mesh);
    tinygltf::Node node; node.mesh = 0;
    model_out.nodes.push_back(node);
    tinygltf::Scene scene; scene.nodes.push_back(0);
    model_out.scenes.push_back(scene);
    model_out.defaultScene = 0;
    return true;
}

/* ── auto-detect game files from game_dir ─────────────────────────── */

/* Collect all files named `target` within `max_depth` directory levels. */
static void find_recursive(const std::string &dir, const std::string &target,
                           int depth, std::vector<std::string> &out) {
    if (depth < 0) return;
    DIR *d = opendir(dir.c_str());
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dir + "/" + name;
        if (name == target) {
            if (file_exists(full)) out.push_back(full);
        } else if (depth > 0) {
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                find_recursive(full, target, depth - 1, out);
        }
    }
    closedir(d);
}

/* Return the single best match for `filename` under game_dir (3 levels).
 * When multiple candidates exist, the lexicographically largest path is
 * returned — for versioned bin/<number>/res/ layouts this picks the newest. */
static std::string find_game_file(const std::string &game_dir,
                                  const std::string &filename) {
    std::vector<std::string> hits;
    find_recursive(game_dir, filename, 3, hits);
    if (hits.empty()) return "";
    std::sort(hits.begin(), hits.end());
    return hits.back(); /* highest = newest version */
}

/* ── DDS block-compressed decoder (BC1/BC2/BC3/BC4/BC5) ──────────── */

static void rgb565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t rv=(c>>11)&0x1f; *r=(rv<<3)|(rv>>2);
    uint8_t gv=(c>> 5)&0x3f; *g=(gv<<2)|(gv>>4);
    uint8_t bv= c     &0x1f; *b=(bv<<3)|(bv>>2);
}

static void bc1_block(const uint8_t *src, uint8_t tmp[64]) {
    uint16_t c0,c1; memcpy(&c0,src,2); memcpy(&c1,src+2,2);
    uint32_t ix;    memcpy(&ix,src+4,4);
    uint8_t r[4],g[4],b[4]; r[3]=g[3]=b[3]=0;
    rgb565(c0,&r[0],&g[0],&b[0]); rgb565(c1,&r[1],&g[1],&b[1]);
    if (c0>c1) {
        r[2]=(2*r[0]+r[1]+1)/3; g[2]=(2*g[0]+g[1]+1)/3; b[2]=(2*b[0]+b[1]+1)/3;
        r[3]=(r[0]+2*r[1]+1)/3; g[3]=(g[0]+2*g[1]+1)/3; b[3]=(b[0]+2*b[1]+1)/3;
    } else {
        r[2]=(r[0]+r[1])/2; g[2]=(g[0]+g[1])/2; b[2]=(b[0]+b[1])/2;
    }
    for (int i=0;i<16;++i) {
        int k=(ix>>(i*2))&3;
        tmp[i*4]=r[k]; tmp[i*4+1]=g[k]; tmp[i*4+2]=b[k]; tmp[i*4+3]=255;
    }
}

static void bc4_block(const uint8_t *src, uint8_t av[16]) {
    uint8_t a0=src[0],a1=src[1]; uint8_t t[8]; t[0]=a0; t[1]=a1;
    if(a0>a1){t[2]=(6*a0+a1+3)/7;t[3]=(5*a0+2*a1+3)/7;t[4]=(4*a0+3*a1+3)/7;
              t[5]=(3*a0+4*a1+3)/7;t[6]=(2*a0+5*a1+3)/7;t[7]=(a0+6*a1+3)/7;}
    else{t[2]=(4*a0+a1+2)/5;t[3]=(3*a0+2*a1+2)/5;t[4]=(2*a0+3*a1+2)/5;
         t[5]=(a0+4*a1+2)/5;t[6]=0;t[7]=255;}
    uint64_t bits=0; memcpy(&bits,src+2,6);
    for(int i=0;i<16;++i) av[i]=t[(bits>>(i*3))&7];
}

enum DdsFmt { DDS_NONE,DDS_BC1,DDS_BC2,DDS_BC3,DDS_BC4,DDS_BC5 };

static std::vector<uint8_t> decode_dds(const uint8_t *d,size_t sz,int *W,int *H) {
    if(sz<128||memcmp(d,"DDS ",4)!=0) return {};
    uint32_t h,w,fcc;
    memcpy(&h,d+12,4); memcpy(&w,d+16,4); memcpy(&fcc,d+84,4);
    size_t off=128; DdsFmt fmt=DDS_NONE;
    static const uint32_t DXT1=0x31545844,DXT3=0x33545844,DXT5=0x35545844,
                          DXT2=0x32545844,DXT4=0x34545844,
                          ATI1=0x31495441,BC4U=0x55344342,
                          ATI2=0x32495441,BC5U=0x55354342,DX10=0x30315844;
    if(fcc==DX10){
        if(sz<148) return {};
        uint32_t dxgi; memcpy(&dxgi,d+128,4); off=148;
        switch(dxgi){case 71:case 72:fmt=DDS_BC1;break;case 74:fmt=DDS_BC2;break;
                     case 77:case 78:fmt=DDS_BC3;break;case 80:case 81:fmt=DDS_BC4;break;
                     case 83:case 84:fmt=DDS_BC5;break;default:return {};}
    } else {
        if(fcc==DXT1)            fmt=DDS_BC1;
        else if(fcc==DXT2||fcc==DXT3) fmt=DDS_BC2;
        else if(fcc==DXT4||fcc==DXT5) fmt=DDS_BC3;
        else if(fcc==ATI1||fcc==BC4U) fmt=DDS_BC4;
        else if(fcc==ATI2||fcc==BC5U) fmt=DDS_BC5;
        else return {};
    }
    int bw=(w+3)/4,bh=(h+3)/4;
    int bs=(fmt==DDS_BC1||fmt==DDS_BC4)?8:16;
    if(sz<off+(size_t)bw*bh*bs) return {};
    *W=(int)w; *H=(int)h;
    std::vector<uint8_t> rgba(w*h*4,255);
    const uint8_t *src=d+off;
    for(int by=0;by<bh;++by) for(int bx=0;bx<bw;++bx) {
        int px=bx*4,py=by*4;
        int cx=std::min(4,(int)w-px),cy=std::min(4,(int)h-py);
        uint8_t tmp[64]={};
        switch(fmt){
        case DDS_BC1: bc1_block(src,tmp); break;
        case DDS_BC2:{uint8_t cb[16];bc1_block(src+8,tmp);
            for(int i=0;i<8;++i){cb[i*2]=(src[i]&0xF)*17;cb[i*2+1]=(src[i]>>4)*17;}
            for(int i=0;i<16;++i)tmp[i*4+3]=255; break;}
        case DDS_BC3:{uint8_t ab[16];bc4_block(src,ab);bc1_block(src+8,tmp);
            for(int i=0;i<16;++i)tmp[i*4+3]=255; break;}
        case DDS_BC4:{uint8_t ab[16];bc4_block(src,ab);
            for(int i=0;i<16;++i){tmp[i*4]=tmp[i*4+1]=tmp[i*4+2]=ab[i];tmp[i*4+3]=255;}break;}
        case DDS_BC5:{uint8_t rb[16],gb[16];bc4_block(src,rb);bc4_block(src+8,gb);
            for(int i=0;i<16;++i){tmp[i*4]=rb[i];tmp[i*4+1]=gb[i];tmp[i*4+2]=0;tmp[i*4+3]=255;}break;}
        default:break;}
        for(int ty=0;ty<cy;++ty)
            memcpy(rgba.data()+(py+ty)*(int)w*4+px*4,tmp+ty*16,cx*4);
        src+=bs;
    }
    return rgba;
}

static std::vector<uint8_t> dds_to_png(const std::string &path, int max_sz) {
    FILE *f=fopen(path.c_str(),"rb"); if(!f) return {};
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0){fclose(f);return {};}
    std::vector<uint8_t> raw((size_t)sz);
    bool ok=fread(raw.data(),1,(size_t)sz,f)==(size_t)sz; fclose(f);
    if(!ok) return {};
    int w,h;
    std::vector<uint8_t> rgba=decode_dds(raw.data(),raw.size(),&w,&h);
    if(rgba.empty()) return {};
    if(max_sz>0&&(w>max_sz||h>max_sz)){
        float s=(float)max_sz/std::max(w,h);
        int nw=std::max(1,(int)(w*s)),nh=std::max(1,(int)(h*s));
        std::vector<uint8_t> rs((size_t)nw*nh*4);
        stbir_resize_uint8_srgb(rgba.data(),w,h,0,rs.data(),nw,nh,0,STBIR_RGBA);
        rgba=std::move(rs); w=nw; h=nh;
    }
    /* Strip alpha channel when fully opaque → ~25% smaller PNG */
    bool has_alpha = false;
    for (int i = 3, n = w*h*4; i < n; i += 4)
        if (rgba[i] != 255) { has_alpha = true; break; }
    std::vector<uint8_t> rgb3;
    const uint8_t *pix = rgba.data();
    int channels = 4, stride = w*4;
    if (!has_alpha) {
        rgb3.resize((size_t)w*h*3);
        for (int i = 0; i < w*h; i++) {
            rgb3[i*3+0]=rgba[i*4+0]; rgb3[i*3+1]=rgba[i*4+1]; rgb3[i*3+2]=rgba[i*4+2];
        }
        pix=rgb3.data(); channels=3; stride=w*3;
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func([](void*ctx,void*data,int n){
        auto*v=(std::vector<uint8_t>*)ctx;
        const auto*p=(const uint8_t*)data; v->insert(v->end(),p,p+n);
    },&png,w,h,channels,pix,stride);
    return png;
}

/* ── geometry mapping_id helpers ─────────────────────────────────── */

static std::vector<uint32_t> read_geom_mapping_ids(const std::string &path) {
    FILE *f=fopen(path.c_str(),"rb"); if(!f) return {};
    uint8_t hdr[72]; if(fread(hdr,1,72,f)<72){fclose(f);return {};}
    uint32_t n; memcpy(&n,hdr+12,4);
    uint64_t off; memcpy(&off,hdr+32,8);
    fseek(f,(long)off,SEEK_SET);
    std::vector<uint32_t> ids;
    for(uint32_t i=0;i<n;++i){
        uint8_t e[16]; if(fread(e,1,16,f)<16) break;
        uint32_t mid; memcpy(&mid,e,4); ids.push_back(mid);
    }
    fclose(f); return ids;
}

static std::map<uint32_t,uint32_t> read_geom_tri_counts(const std::string &path) {
    FILE *f=fopen(path.c_str(),"rb"); if(!f) return {};
    uint8_t hdr[72]; if(fread(hdr,1,72,f)<72){fclose(f);return {};}
    uint32_t n; memcpy(&n,hdr+12,4);
    uint64_t off; memcpy(&off,hdr+32,8);
    fseek(f,(long)off,SEEK_SET);
    std::map<uint32_t,uint32_t> r;
    for(uint32_t i=0;i<n;++i){
        uint8_t e[16]; if(fread(e,1,16,f)<16) break;
        uint32_t mid,cnt; memcpy(&mid,e,4); memcpy(&cnt,e+12,4);
        r[mid]=cnt/3;
    }
    fclose(f); return r;
}

/* ── texture file finder ─────────────────────────────────────────── */

static const char *MFM_STRIP[]={"_skinned","_alpha","_ep",nullptr};

static std::string find_texture(const std::string &dir,
                                 const std::string &stem,
                                 const char *channel) {
    std::vector<std::string> cands={stem};
    for(const char**s=MFM_STRIP;*s;++s){
        size_t sl=strlen(*s);
        if(stem.size()>sl&&stem.compare(stem.size()-sl,sl,*s)==0){
            cands.push_back(stem.substr(0,stem.size()-sl)); break;
        }
    }
    for(auto&cs:cands)
        for(auto ext:{".dd0",".dd1",".dds"}){
            std::string p=dir+"/"+cs+channel+ext;
            if(file_exists(p)) return p;
        }
    return "";
}

/* ── LOD selection ───────────────────────────────────────────────── */

static int best_lod(const assets_bin_visual_info_t *vi,
                    const std::set<uint32_t> &dmg,
                    const std::map<uint32_t,uint32_t> &tc) {
    int best=0,btris=-1;
    for(size_t i=0;i<vi->lod_count;++i){
        int tris=0;
        for(size_t j=0;j<vi->lods[i].count;++j){
            uint32_t mid=vi->lods[i].mapping_ids[j];
            if(dmg.count(mid)) continue;
            auto it=tc.find(mid); if(it!=tc.end()) tris+=(int)it->second;
        }
        if(tris>btris){btris=tris;best=(int)i;}
    }
    return best;
}

/* ── texture application ─────────────────────────────────────────── */

static void apply_textures(tinygltf::Model &model,
                            const std::vector<std::string> &geom_order,
                            assets_bin_pdb_t *pdb,
                            const std::string &game_dir,
                            int lod_level, bool excl_damage, int max_tex) {
    if(!pdb||geom_order.empty()) return;

    if(model.samplers.empty()){
        tinygltf::Sampler samp;
        samp.magFilter=9729; samp.minFilter=9987;
        samp.wrapS=samp.wrapT=10497;
        model.samplers.push_back(samp);
    }

    std::map<std::string,int> stem_to_mat;

    auto embed_png=[&](const std::vector<uint8_t>&png)->int{
        auto&buf=model.buffers[0].data;
        while(buf.size()&3) buf.push_back(0);
        size_t boff=buf.size();
        buf.insert(buf.end(),png.begin(),png.end());
        tinygltf::BufferView bv; bv.buffer=0; bv.byteOffset=boff; bv.byteLength=png.size();
        int bvi=(int)model.bufferViews.size(); model.bufferViews.push_back(bv);
        tinygltf::Image img; img.bufferView=bvi; img.mimeType="image/png";
        int ii=(int)model.images.size(); model.images.push_back(img);
        tinygltf::Texture tex; tex.sampler=0; tex.source=ii;
        int ti=(int)model.textures.size(); model.textures.push_back(tex);
        return ti;
    };

    auto vflip_tex=[](int ti)->tinygltf::TextureInfo{
        tinygltf::TextureInfo t; t.index=ti; t.texCoord=0;
        tinygltf::Value::Object khr;
        tinygltf::Value::Array sc,of;
        sc.push_back(tinygltf::Value(1.0)); sc.push_back(tinygltf::Value(-1.0));
        of.push_back(tinygltf::Value(0.0)); of.push_back(tinygltf::Value( 1.0));
        khr["scale"]=tinygltf::Value(sc); khr["offset"]=tinygltf::Value(of);
        t.extensions["KHR_texture_transform"]=tinygltf::Value(khr);
        return t;
    };

    auto ensure_mat=[&](const std::string&name,const std::vector<uint8_t>&png)->int{
        auto it=stem_to_mat.find(name);
        if(it!=stem_to_mat.end()) return it->second;
        int ti=embed_png(png);
        tinygltf::Material mat; mat.name=name; mat.doubleSided=true;
        mat.pbrMetallicRoughness.baseColorTexture=vflip_tex(ti);
        mat.pbrMetallicRoughness.metallicFactor=0.0;
        mat.pbrMetallicRoughness.roughnessFactor=0.8;
        int mi=(int)model.materials.size(); model.materials.push_back(mat);
        stem_to_mat[name]=mi; return mi;
    };

    struct GtData {
        std::vector<uint32_t> mids;
        std::set<uint32_t>    allowed;
        std::map<uint32_t,int> mid_mat;
    };
    std::map<std::string,GtData> cache;

    for(size_t mi=0;mi<model.meshes.size();++mi){
        if(mi>=geom_order.size()) break;
        const std::string &gp=geom_order[mi];
        if(gp.empty()) continue;

        if(!cache.count(gp)){
            GtData &gt=cache[gp];
            std::string suf=geom_to_visual_suffix(gp);
            assets_bin_visual_info_t *vi=assets_bin_get_visual_info(pdb,suf.c_str());
            if(!vi){ cache[gp]={};  continue; }

            gt.mids=read_geom_mapping_ids(gp);

            std::set<uint32_t> dmg;
            for(size_t ri=0;ri<vi->rs_count;++ri)
                if(vi->render_sets[ri].is_damage)
                    dmg.insert(vi->render_sets[ri].indices_mapping_id);

            int clod;
            if(lod_level<0&&vi->lod_count>0){
                auto tc=read_geom_tri_counts(gp);
                clod=best_lod(vi,dmg,tc);
            } else { clod=std::max(0,lod_level); }

            fprintf(stderr,"  Visual %s: LOD%d/%zu, %zu render sets\n",
                    path_basename(gp).c_str(),clod,vi->lod_count,vi->rs_count);

            if(vi->lod_count>0&&(size_t)clod<vi->lod_count)
                for(size_t j=0;j<vi->lods[clod].count;++j)
                    gt.allowed.insert(vi->lods[clod].mapping_ids[j]);
            else
                for(size_t ri=0;ri<vi->rs_count;++ri)
                    gt.allowed.insert(vi->render_sets[ri].indices_mapping_id);

            if(excl_damage) for(uint32_t m:dmg) gt.allowed.erase(m);

            for(size_t ri=0;ri<vi->rs_count;++ri){
                const assets_bin_rs_t &rs=vi->render_sets[ri];
                uint32_t mid=rs.indices_mapping_id;
                if(!gt.allowed.count(mid)||!rs.mfm_path[0]) continue;

                std::string mfm=normalize_slashes(rs.mfm_path);
                auto sl=mfm.rfind('/');
                std::string mdir=(sl!=std::string::npos)?mfm.substr(0,sl):".";
                std::string mfile=(sl!=std::string::npos)?mfm.substr(sl+1):mfm;
                std::string tstem=mfile.size()>4&&mfile.compare(mfile.size()-4,4,".mfm")==0
                                   ?mfile.substr(0,mfile.size()-4):mfile;
                std::string tdir=game_dir+"/"+mdir;

                std::string dds=find_texture(tdir,tstem,"_a");
                if(dds.empty()){
                    fprintf(stderr,"    no albedo: %s\n",tstem.c_str()); continue;
                }
                std::vector<uint8_t> png=dds_to_png(dds,max_tex);
                if(png.empty()){
                    fprintf(stderr,"    decode fail: %s\n",path_basename(dds).c_str()); continue;
                }
                int mat=ensure_mat(tstem,png);
                gt.mid_mat[mid]=mat;
                fprintf(stderr,"    %s → mat%d (%zuKB)\n",
                        path_basename(dds).c_str(),mat,png.size()/1024);
            }
            assets_bin_visual_info_free(vi);
        }

        GtData &gt=cache.at(gp);
        std::vector<tinygltf::Primitive> kept;
        for(size_t pi=0;pi<model.meshes[mi].primitives.size();++pi){
            tinygltf::Primitive &prim=model.meshes[mi].primitives[pi];
            if(pi<gt.mids.size()){
                uint32_t mid=gt.mids[pi];
                if(!gt.allowed.empty()&&!gt.allowed.count(mid)) continue;
                auto it=gt.mid_mat.find(mid);
                if(it!=gt.mid_mat.end()) prim.material=it->second;
            }
            kept.push_back(prim);
        }
        model.meshes[mi].primitives=std::move(kept);
    }

    if(!stem_to_mat.empty()){
        auto&eu=model.extensionsUsed;
        if(std::find(eu.begin(),eu.end(),"KHR_texture_transform")==eu.end())
            eu.push_back("KHR_texture_transform");
    }
}

/* ── argp ─────────────────────────────────────────────────────────── */

const char *argp_program_version     = "wows-stitch " BFD_VERSION;
const char *argp_program_bug_address = "https://github.com/kakwa/wows-depack/issues";

static char doc[] =
    "\nStitch WoWS ship geometry parts into a single GLB.\n"
    "\n"
    "GameParams.data and assets.bin are auto-detected from -d if not given\n"
    "(recursive scan up to 3 directory levels; newest version wins).";

static struct argp_option options[] = {
    {"gameparams",   'g', "FILE",  0, "GameParams.data (auto-detected from -d if omitted)"},
    {"game-dir",     'd', "DIR",   0, "Root game directory"},
    {"ship",         's', "NAME",  0, "Ship name or substring (e.g. PJSB007, Kongo)"},
    {"output",       'o', "FILE",  0, "Output .glb file"},
    {"hull",         'H', "UPG",   0, "Hull upgrade name substring (default: first)"},
    {"with-turrets", 't', nullptr, 0, "Include turret / mounted-component models"},
    {"assets-bin",   'a', "FILE",  0, "assets.bin (auto-detected from -d if omitted)"},
    {"textures",     'T', nullptr, 0, "Apply DDS textures (requires assets.bin)"},
    {"texture-size", 'Z', "N",     0, "Max texture dimension in pixels (default: 2048)"},
    {"lod",          'L', "N",     0, "LOD level to export (-1=auto, default: -1)"},
    {"damage",       'D', nullptr, 0, "Include damage/crack geometry (default: excluded)"},
    {0}
};

struct Args {
    char *gameparams   = nullptr;
    char *game_dir     = nullptr;
    char *ship         = nullptr;
    char *output       = nullptr;
    char *hull         = nullptr;
    char *assets_bin   = nullptr;
    bool  with_turrets = false;
    bool  textures     = false;
    int   tex_size     = 2048;
    int   lod          = -1;
    bool  damage       = false;
};

static error_t parse_opt(int key, char *arg, struct argp_state *state) {
    Args *a = static_cast<Args *>(state->input);
    switch (key) {
    case 'g': a->gameparams   = arg; break;
    case 'd': a->game_dir     = arg; break;
    case 's': a->ship         = arg; break;
    case 'o': a->output       = arg; break;
    case 'H': a->hull         = arg; break;
    case 'a': a->assets_bin   = arg; break;
    case 't': a->with_turrets = true; break;
    case 'T': a->textures     = true; break;
    case 'Z': a->tex_size     = atoi(arg); break;
    case 'L': a->lod          = atoi(arg); break;
    case 'D': a->damage       = true; break;
    default:  return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, nullptr, doc};

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    Args args;
    argp_parse(&argp, argc, argv, 0, nullptr, &args);

    if (!args.game_dir || !args.ship || !args.output) {
        fprintf(stderr, "Error: -d, -s, and -o are required.\n");
        return 1;
    }

    std::string game_dir = normalize_slashes(args.game_dir);

    /* ── auto-detect GameParams.data ─── */
    std::string gameparams_path = args.gameparams ? args.gameparams : "";
    if (gameparams_path.empty()) {
        gameparams_path = find_game_file(game_dir, "GameParams.data");
        if (gameparams_path.empty()) {
            fprintf(stderr,
                    "Error: GameParams.data not found under %s.\n"
                    "       Supply -g <path> to specify it explicitly.\n",
                    game_dir.c_str());
            return 1;
        }
        fprintf(stderr, "Auto-detected GameParams: %s\n", gameparams_path.c_str());
    }

    /* ── auto-detect assets.bin ─── */
    std::string assets_bin_path = args.assets_bin ? args.assets_bin : "";
    if (assets_bin_path.empty()) {
        assets_bin_path = find_game_file(game_dir, "assets.bin");
        if (!assets_bin_path.empty())
            fprintf(stderr, "Auto-detected assets.bin: %s\n", assets_bin_path.c_str());
    }

    fprintf(stderr, "Loading GameParams …\n");
    Py_Initialize();

    HullInfo hull;
    if (!load_hull_info(gameparams_path.c_str(), args.ship, args.hull, hull)) {
        Py_Finalize();
        return 1;
    }

    Py_Finalize();

    fprintf(stderr, "Hull model: %s\n", hull.hull_model.c_str());
    fprintf(stderr, "Mounts:     %zu\n", hull.mounts.size());

    /* ── hull geometry files ─── */
    std::vector<std::string> hull_geoms = find_hull_geoms(hull.hull_model, game_dir);
    if (hull_geoms.empty()) {
        fprintf(stderr, "Error: no hull geometry files found for %s\n",
                hull.hull_model.c_str());
        return 1;
    }
    fprintf(stderr, "Hull parts: %zu\n", hull_geoms.size());

    /* ── HP transforms and BlendBone corrections ─── */
    std::map<std::string, Mat16d> hp_transforms;
    std::map<std::string, Mat16d> bb_corrections;

    if (args.with_turrets && !assets_bin_path.empty()) {
        fprintf(stderr, "Loading HP transforms from assets.bin …\n");

        for (const auto &gp : hull_geoms) {
            std::string suffix = geom_to_visual_suffix(gp);
            assets_bin_hp_list_t *hp = assets_bin_get_hp_transforms(
                assets_bin_path.c_str(), suffix.c_str());
            if (!hp) continue;
            for (size_t i = 0; i < hp->count; ++i) {
                hp_transforms[hp->entries[i].name] =
                    float_to_double_mat(hp->entries[i].mat);
            }
            assets_bin_hp_list_free(hp);
        }
        fprintf(stderr, "  %zu HP_ transforms.\n", hp_transforms.size());

        /* unique turret model paths */
        std::vector<std::string> unique_models;
        std::set<std::string>    seen;
        for (const auto &m : hull.mounts)
            if (!m.model_path.empty() && seen.insert(m.model_path).second)
                unique_models.push_back(m.model_path);

        if (!unique_models.empty()) {
            std::vector<const char *> ptrs;
            for (const auto &s : unique_models) ptrs.push_back(s.c_str());
            assets_bin_bb_list_t *bb = assets_bin_get_blendbone_corrections(
                assets_bin_path.c_str(), ptrs.data(), ptrs.size());
            if (bb) {
                for (size_t i = 0; i < bb->count; ++i)
                    bb_corrections[bb->entries[i].model_path] =
                        float_to_double_mat(bb->entries[i].correction);
                assets_bin_bb_list_free(bb);
            }
        }
    }

    /* ── convert + collect GLB parts ─── */
    std::vector<GlbPart> parts;

    /* hull parts — no transform */
    for (const auto &gp : hull_geoms) {
        fprintf(stderr, "  Hull part: %s …\n", path_basename(gp).c_str());
        GlbPart part;
        part.mesh_name = stem(path_basename(gp));
        part.geom_path = gp;
        if (geom_to_model(gp, part.model))
            parts.push_back(std::move(part));
    }

    if (parts.empty()) {
        fprintf(stderr, "Error: no hull parts could be converted.\n");
        return 1;
    }

    /* turret parts */
    if (args.with_turrets) {
        /* deduplicate geom path per (model_path → geom) */
        std::map<std::string, std::string> model_to_geom_map;
        for (const auto &m : hull.mounts) {
            if (model_to_geom_map.count(m.model_path)) continue;
            std::string gp = model_to_geom_path(m.model_path, game_dir);
            model_to_geom_map[m.model_path] = file_exists(gp) ? gp : "";
        }

        /* iterate mounts (sorted by hp_name for determinism) */
        std::vector<MountEntry> sorted_mounts = hull.mounts;
        std::sort(sorted_mounts.begin(), sorted_mounts.end(),
                  [](const MountEntry &a, const MountEntry &b) {
                      return a.hp_name < b.hp_name;
                  });

        for (const auto &m : sorted_mounts) {
            const std::string &geom_path = model_to_geom_map[m.model_path];
            if (geom_path.empty()) continue;

            /* compute transform: hp_mat * blendbone_correction */
            Mat16d transform;
            auto hp_it = hp_transforms.find(m.hp_name);
            if (hp_it != hp_transforms.end()) {
                transform = hp_it->second;
                auto bb_it = bb_corrections.find(m.model_path);
                if (bb_it != bb_corrections.end())
                    transform = mat4_mul_d(transform, bb_it->second);
            }

            std::string label = stem(path_basename(geom_path)) + " (" + m.hp_name + ")";
            fprintf(stderr, "  Turret: %s …\n", label.c_str());

            GlbPart part;
            part.mesh_name = label;
            part.geom_path = geom_path;
            part.matrix    = transform;
            if (geom_to_model(geom_path, part.model))
                parts.push_back(std::move(part));
        }
    }

    /* ── merge ─── */
    fprintf(stderr, "Merging %zu part(s) …\n", parts.size());
    tinygltf::Model merged = merge_parts(parts);

    /* ── textures ─── */
    if (args.textures && !assets_bin_path.empty()) {
        fprintf(stderr, "Applying textures (size=%d, lod=%d, damage=%s) …\n",
                args.tex_size, args.lod, args.damage ? "yes" : "no");
        assets_bin_pdb_t *pdb = assets_bin_pdb_open(assets_bin_path.c_str());
        if (!pdb) {
            fprintf(stderr, "Warning: failed to open assets.bin for textures.\n");
        } else {
            std::vector<std::string> geom_order;
            for (const auto &p : parts) geom_order.push_back(p.geom_path);
            apply_textures(merged, geom_order, pdb, game_dir,
                           args.lod, !args.damage, args.tex_size);
            assets_bin_pdb_free(pdb);
        }
    } else if (args.textures) {
        fprintf(stderr, "Warning: --textures requires assets.bin (use -a or -d).\n");
    }

    /* ── write output ─── */
    tinygltf::TinyGLTF writer;
    std::string err, warn;
    bool ok = writer.WriteGltfSceneToFile(
        &merged, args.output,
        /*embedImages*/ true,
        /*embedBuffers*/ true,
        /*prettyPrint*/ false,
        /*writeBinary*/ true);

    if (!ok) {
        fprintf(stderr, "Error: failed to write GLB to %s\n", args.output);
        return 1;
    }

    struct stat st;
    stat(args.output, &st);
    fprintf(stderr, "Written: %s (%.1f KB)\n", args.output, st.st_size / 1024.0);
    fprintf(stderr, "  Meshes:      %zu\n", merged.meshes.size());
    fprintf(stderr, "  Accessors:   %zu\n", merged.accessors.size());
    fprintf(stderr, "  BufferViews: %zu\n", merged.bufferViews.size());

    return 0;
}
