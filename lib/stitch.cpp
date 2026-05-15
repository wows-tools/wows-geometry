#include "wows-model-exporter.h"
extern "C" {
#include "wows-geometry.h"
}

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

bool wows_stitch_verbose = false;
/* mirror verbosity into assets_bin */
void wows_stitch_set_verbose(bool v) {
    wows_stitch_verbose = v;
    wows_assets_bin_verbose = v;
}
#define vlog(tag, fmt, ...)                                                                                            \
    do {                                                                                                               \
        if (wows_stitch_verbose)                                                                                       \
            fprintf(stderr, "[%s] " fmt, tag, ##__VA_ARGS__);                                                          \
    } while (0)

/* ── path / file utilities ───────────────────────────────────────── */

std::string wows_stitch_path_basename(const std::string &p) {
    auto sl = p.rfind('/');
    return (sl == std::string::npos) ? p : p.substr(sl + 1);
}

std::string wows_stitch_path_dirname(const std::string &p) {
    auto sl = p.rfind('/');
    return (sl == std::string::npos) ? "." : p.substr(0, sl);
}

std::string wows_stitch_stem(const std::string &filename) {
    auto dot = filename.rfind('.');
    return (dot == std::string::npos) ? filename : filename.substr(0, dot);
}

std::string wows_stitch_normalize_slashes(std::string s) {
    for (auto &c : s)
        if (c == '\\')
            c = '/';
    return s;
}

bool wows_stitch_file_exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/* ── model path helpers ─────────────────────────────────────────── */

std::string wows_stitch_model_to_geom_path(const std::string &model, const std::string &game_dir) {
    std::string rel = wows_stitch_normalize_slashes(model);
    auto pos = rel.rfind(".model");
    if (pos != std::string::npos)
        rel.replace(pos, 6, ".geometry");
    return wows_stitch_normalize_slashes(game_dir + "/" + rel);
}

std::string wows_stitch_geom_to_visual_suffix(const std::string &geom_path) {
    std::string v = wows_stitch_normalize_slashes(geom_path);
    auto pos = v.rfind(".geometry");
    if (pos != std::string::npos)
        v.replace(pos, 9, ".visual");
    auto sl1 = v.rfind('/');
    if (sl1 != std::string::npos && sl1 > 0) {
        auto sl2 = v.rfind('/', sl1 - 1);
        if (sl2 != std::string::npos)
            return v.substr(sl2 + 1);
    }
    return v;
}

std::vector<std::string> wows_stitch_find_hull_geoms(const std::string &hull_model, const std::string &game_dir) {
    std::string geom_path = wows_stitch_model_to_geom_path(hull_model, game_dir);
    if (!wows_stitch_file_exists(geom_path))
        return {};

    std::string ship_dir = wows_stitch_path_dirname(geom_path);
    std::string base_name = wows_stitch_stem(wows_stitch_path_basename(geom_path));

    DIR *dir = opendir(ship_dir.c_str());
    if (!dir)
        return {};

    std::vector<std::string> results;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        std::string fname = ent->d_name;
        if (fname.size() <= 9)
            continue;
        if (fname.compare(fname.size() - 9, 9, ".geometry") != 0)
            continue;
        if (fname.compare(0, base_name.size(), base_name) != 0)
            continue;
        if (wows_stitch_stem(fname) == base_name)
            continue;
        results.push_back(ship_dir + "/" + fname);
    }
    closedir(dir);
    std::sort(results.begin(), results.end());
    return results;
}

std::vector<std::string> wows_stitch_find_propeller_geoms(const std::string &hull_model, const std::string &game_dir) {
    std::string geom_path = wows_stitch_model_to_geom_path(hull_model, game_dir);
    std::string ship_dir = wows_stitch_path_dirname(geom_path);

    DIR *dir = opendir(ship_dir.c_str());
    if (!dir)
        return {};

    std::vector<std::string> results;
    struct dirent *ent;
    while ((ent = readdir(dir))) {
        std::string fname = ent->d_name;
        if (fname.size() <= 9)
            continue;
        if (fname.compare(fname.size() - 9, 9, ".geometry") != 0)
            continue;
        std::string flower = fname;
        std::transform(flower.begin(), flower.end(), flower.begin(), ::tolower);
        if (flower.find("prop") != std::string::npos || flower.find("screw") != std::string::npos)
            results.push_back(ship_dir + "/" + fname);
    }
    closedir(dir);
    std::sort(results.begin(), results.end());
    return results;
}

/* ── game file discovery ────────────────────────────────────────── */

static void find_recursive(const std::string &dir, const std::string &target, int depth,
                           std::vector<std::string> &out) {
    if (depth < 0)
        return;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        std::string name = ent->d_name;
        if (name == "." || name == "..")
            continue;
        std::string full = dir + "/" + name;
        if (name == target) {
            if (wows_stitch_file_exists(full))
                out.push_back(full);
        } else if (depth > 0) {
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                find_recursive(full, target, depth - 1, out);
        }
    }
    closedir(d);
}

std::string wows_stitch_find_game_file(const std::string &game_dir, const std::string &filename) {
    std::vector<std::string> hits;
    find_recursive(game_dir, filename, 3, hits);
    if (hits.empty())
        return "";
    std::sort(hits.begin(), hits.end());
    return hits.back();
}

/* ── matrix math ────────────────────────────────────────────────── */

wows_mat16d wows_stitch_mat4_mul_d(const wows_mat16d &a, const wows_mat16d &b) {
    wows_mat16d out(16, 0.0);
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                out[col * 4 + row] += a[k * 4 + row] * b[col * 4 + k];
    return out;
}

wows_mat16d wows_stitch_float_to_double_mat(const float m[16]) {
    wows_mat16d r(16);
    for (int i = 0; i < 16; ++i)
        r[i] = (double)m[i];
    return r;
}

/* ── geometry → tinygltf::Model ─────────────────────────────────── */

static float f16_to_f32(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31;
    uint32_t e = (h >> 10) & 0x1fu;
    uint32_t m = h & 0x3ffu;
    uint32_t f;
    if (e == 0) {
        if (m == 0) {
            f = s;
        } else {
            e = 1;
            while (!(m & (1u << 10))) {
                m <<= 1;
                --e;
            }
            m &= ~(1u << 10);
            f = s | (e << 23) | (m << 13);
        }
    } else if (e == 31) {
        f = s | 0x7f800000u | (m << 13);
    } else {
        f = s | ((e + 112) << 23) | (m << 13);
    }
    float r;
    memcpy(&r, &f, 4);
    return r;
}

static void unpack_normal(uint32_t packed, float *nx, float *ny, float *nz) {
    int8_t bytes[4];
    memcpy(bytes, &packed, 4);
    *nx = bytes[0] / 127.0f;
    *ny = bytes[1] / 127.0f;
    *nz = bytes[2] / 127.0f;
}

static void unpack_uv(uint32_t packed, float *u, float *v) {
    uint16_t hu, hv;
    memcpy(&hu, (uint8_t *)&packed + 0, 2);
    memcpy(&hv, (uint8_t *)&packed + 2, 2);
    /* UV is stored as actual_uv - 0.5 in half-float */
    *u = f16_to_f32(hu) + 0.5f;
    *v = f16_to_f32(hv) + 0.5f;
}

static uint32_t find_vbase(const wows_geometry *g, uint32_t ibloc, uint16_t *vtype_out) {
    const wows_geometry_info *idx_tgt = &g->index_bloc_map[ibloc];
    uint16_t ptd = idx_tgt->packed_texel_density;
    uint32_t icnt = idx_tgt->items_count;
    uint32_t ioff = idx_tgt->items_offset;
    uint32_t nv = g->header->n_vertex_bloc;
    uint32_t ni = g->header->n_index_bloc;
    *vtype_out = 0;

    uint32_t rank = 0;
    for (uint32_t i = 0; i < ni; i++) {
        const wows_geometry_info *idx_cand = &g->index_bloc_map[i];
        if (idx_cand->packed_texel_density != ptd)
            continue;
        uint32_t oc = idx_cand->items_count, oo = idx_cand->items_offset;
        if (oc > icnt || (oc == icnt && oo < ioff))
            rank++;
    }

    uint32_t prev_cnt = UINT32_MAX, prev_off = UINT32_MAX;
    bool have_prev = false;
    for (uint32_t pass = 0; pass <= rank; pass++) {
        uint32_t cur_cnt = 0, cur_off = UINT32_MAX;
        uint16_t cur_vt = 0;
        for (uint32_t j = 0; j < nv; j++) {
            const wows_geometry_info *vtx_cand = &g->vertex_bloc_map[j];
            if (vtx_cand->packed_texel_density != ptd)
                continue;
            uint32_t c = vtx_cand->items_count, o = vtx_cand->items_offset;
            if (have_prev) {
                if (c > prev_cnt)
                    continue;
                if (c == prev_cnt && o <= prev_off)
                    continue;
            }
            if (c > cur_cnt || (c == cur_cnt && o < cur_off)) {
                cur_cnt = c;
                cur_off = o;
                cur_vt = vtx_cand->merged_buffer_index;
            }
        }
        if (cur_off == UINT32_MAX)
            return 0;
        if (pass == rank) {
            *vtype_out = cur_vt;
            return cur_off;
        }
        prev_cnt = cur_cnt;
        prev_off = cur_off;
        have_prev = true;
    }
    return 0;
}

/* shared implementation: converts a parsed wows_geometry → tinygltf::Model */
static bool geom_to_model_impl(wows_geometry *geom, const std::string &tag, tinygltf::Model &model_out) {
    uint32_t n_vt = geom->header->n_vertex_type;
    uint32_t n_ibloc = geom->header->n_index_bloc;
    uint32_t n_it = geom->header->n_index_type;

    std::vector<uint8_t> blob;
    blob.reserve(1 << 21);
    auto pad4 = [&]() {
        while (blob.size() & 3)
            blob.push_back(0);
    };
    auto append = [&](const void *src, size_t n) {
        const auto *p = static_cast<const uint8_t *>(src);
        blob.insert(blob.end(), p, p + n);
    };

    struct VInfo {
        size_t pos_bv, norm_bv, uv_bv;
        uint32_t count;
        float mn[3], mx[3];
    };
    std::vector<VInfo> vinfo(n_vt);

    for (uint32_t k = 0; k < n_vt; ++k) {
        vinfo[k].count = 0;
        if (!geom->vertexes || !geom->vertexes[k] || !geom->vertexes[k]->raw_data)
            continue;
        uint32_t total = geom->vertexes[k]->vertex_count;
        uint16_t stride = geom->vertex_meta_sections[k].s_vertex_size;
        const uint8_t *raw = geom->vertexes[k]->raw_data;

        std::vector<float> pos(total * 3), norm(total * 3), uv(total * 2);
        float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t j = 0; j < total; ++j) {
            const uint8_t *v = raw + (size_t)j * stride;
            float x, y, z;
            memcpy(&x, v, 4);
            memcpy(&y, v + 4, 4);
            memcpy(&z, v + 8, 4);
            pos[j * 3] = x;
            pos[j * 3 + 1] = y;
            pos[j * 3 + 2] = z;
            for (int i = 0; i < 3; i++) {
                float c = (i == 0 ? x : i == 1 ? y : z);
                if (c < mn[i])
                    mn[i] = c;
                if (c > mx[i])
                    mx[i] = c;
            }
            uint32_t pn, puv;
            memcpy(&pn, v + 12, 4);
            memcpy(&puv, v + 16, 4);
            unpack_normal(pn, &norm[j * 3], &norm[j * 3 + 1], &norm[j * 3 + 2]);
            unpack_uv(puv, &uv[j * 2], &uv[j * 2 + 1]);
        }
        pad4();
        vinfo[k].pos_bv = blob.size();
        append(pos.data(), total * 12);
        pad4();
        vinfo[k].norm_bv = blob.size();
        append(norm.data(), total * 12);
        pad4();
        vinfo[k].uv_bv = blob.size();
        append(uv.data(), total * 8);
        vinfo[k].count = total;
        memcpy(vinfo[k].mn, mn, 12);
        memcpy(vinfo[k].mx, mx, 12);
    }

    struct IInfo {
        size_t bv_off;
        uint32_t count;
        uint16_t idx_size, vtype;
    };
    std::vector<IInfo> iinfo(n_ibloc);
    for (uint32_t i = 0; i < n_ibloc; ++i) {
        iinfo[i].count = 0;
        uint16_t ibuf = geom->index_bloc_map[i].merged_buffer_index;
        uint32_t ioff = geom->index_bloc_map[i].items_offset;
        uint32_t icnt = geom->index_bloc_map[i].items_count;
        if (ibuf >= n_it || !geom->indexes || !geom->indexes[ibuf] || !geom->indexes[ibuf]->raw_data ||
            ioff + icnt > geom->indexes[ibuf]->index_count)
            continue;
        uint16_t is = geom->indexes[ibuf]->index_size;
        const uint8_t *raw = geom->indexes[ibuf]->raw_data + (size_t)ioff * is;
        uint16_t vt;
        uint32_t vbase = find_vbase(geom, i, &vt);
        if (vt >= n_vt || !vinfo[vt].count)
            continue;
        iinfo[i].vtype = vt;
        iinfo[i].count = icnt;

        uint16_t out_is = is;
        if (is == 2) {
            uint32_t raw_max = 0;
            for (uint32_t j = 0; j < icnt; ++j) {
                uint16_t tv;
                memcpy(&tv, raw + j * 2, 2);
                if ((uint32_t)tv > raw_max)
                    raw_max = tv;
            }
            if (raw_max + vbase > 65535u)
                out_is = 4;
        }
        iinfo[i].idx_size = out_is;
        pad4();
        iinfo[i].bv_off = blob.size();
        for (uint32_t j = 0; j < icnt; ++j) {
            uint32_t v = 0;
            if (is == 2) {
                uint16_t t;
                memcpy(&t, raw + j * 2, 2);
                v = t;
            } else
                memcpy(&v, raw + j * 4, 4);
            uint32_t vo = v + vbase;
            if (out_is == 4)
                append(&vo, 4);
            else {
                uint16_t vo16 = (uint16_t)vo;
                append(&vo16, 2);
            }
        }
    }
    pad4();

    tinygltf::Buffer tbuf;
    tbuf.data = std::move(blob);
    model_out.buffers.push_back(std::move(tbuf));
    model_out.asset.version = "2.0";
    model_out.asset.generator = "wows-geometry";

    auto add_bv = [&](size_t off, size_t len, int tgt) -> int {
        tinygltf::BufferView bv;
        bv.buffer = 0;
        bv.byteOffset = off;
        bv.byteLength = len;
        bv.target = tgt;
        model_out.bufferViews.push_back(bv);
        return (int)model_out.bufferViews.size() - 1;
    };
    auto add_acc = [&](int bv, int comp, int type, int cnt) -> int {
        tinygltf::Accessor ac;
        ac.bufferView = bv;
        ac.byteOffset = 0;
        ac.componentType = comp;
        ac.type = type;
        ac.count = cnt;
        model_out.accessors.push_back(ac);
        return (int)model_out.accessors.size() - 1;
    };

    std::vector<int> pos_acc(n_vt, -1), norm_acc(n_vt, -1), uv_acc(n_vt, -1);
    for (uint32_t k = 0; k < n_vt; ++k) {
        if (!vinfo[k].count)
            continue;
        uint32_t vc = vinfo[k].count;
        int bvp = add_bv(vinfo[k].pos_bv, vc * 12, TINYGLTF_TARGET_ARRAY_BUFFER);
        int bvn = add_bv(vinfo[k].norm_bv, vc * 12, TINYGLTF_TARGET_ARRAY_BUFFER);
        int bvu = add_bv(vinfo[k].uv_bv, vc * 8, TINYGLTF_TARGET_ARRAY_BUFFER);
        pos_acc[k] = add_acc(bvp, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vc);
        norm_acc[k] = add_acc(bvn, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vc);
        uv_acc[k] = add_acc(bvu, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, vc);
        auto &pa = model_out.accessors[pos_acc[k]];
        pa.minValues = {vinfo[k].mn[0], vinfo[k].mn[1], vinfo[k].mn[2]};
        pa.maxValues = {vinfo[k].mx[0], vinfo[k].mx[1], vinfo[k].mx[2]};
    }

    tinygltf::Mesh mesh;
    for (uint32_t i = 0; i < n_ibloc; ++i) {
        if (!iinfo[i].count)
            continue;
        uint16_t k = iinfo[i].vtype;
        if (k >= n_vt || pos_acc[k] < 0)
            continue;
        int bvi =
            add_bv(iinfo[i].bv_off, (size_t)iinfo[i].count * iinfo[i].idx_size, TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        int comp =
            (iinfo[i].idx_size == 2) ? TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT : TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
        int ia = add_acc(bvi, comp, TINYGLTF_TYPE_SCALAR, iinfo[i].count);
        tinygltf::Primitive prim;
        prim.attributes["POSITION"] = pos_acc[k];
        prim.attributes["NORMAL"] = norm_acc[k];
        prim.attributes["TEXCOORD_0"] = uv_acc[k];
        prim.indices = ia;
        prim.mode = TINYGLTF_MODE_TRIANGLES;
        mesh.primitives.push_back(prim);
    }
    model_out.meshes.push_back(mesh);
    tinygltf::Node node;
    node.mesh = 0;
    model_out.nodes.push_back(node);
    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model_out.scenes.push_back(scene);
    model_out.defaultScene = 0;
    return true;
}

bool wows_stitch_geom_to_model(const std::string &geom_path, tinygltf::Model &model_out) {
    wows_geometry *geom = nullptr;
    std::vector<char> gp_buf(geom_path.begin(), geom_path.end());
    gp_buf.push_back('\0');
    if (wows_parse_geometry(gp_buf.data(), &geom) != 0) {
        vlog(wows_stitch_path_basename(geom_path).c_str(), "Warning: failed to parse geometry\n");
        return false;
    }
    bool ok = geom_to_model_impl(geom, wows_stitch_path_basename(geom_path), model_out);
    wows_geometry_free(geom);
    return ok;
}

bool wows_stitch_geom_to_model_from_memory(const uint8_t *data, size_t size, tinygltf::Model &model_out) {
    if (!data || size == 0)
        return false;
    wows_geometry *geom = nullptr;
    FILE *fp = fmemopen(const_cast<void *>(static_cast<const void *>(data)), size, "rb");
    if (!fp)
        return false;
    int rc = wows_parse_geometry_fp(fp, &geom);
    fclose(fp);
    if (rc != 0 || !geom)
        return false;
    bool ok = geom_to_model_impl(geom, "memory", model_out);
    wows_geometry_free(geom);
    return ok;
}

/* ── GLB merge ──────────────────────────────────────────────────── */

tinygltf::Model wows_stitch_merge_parts(std::vector<wows_glb_part> &parts) {
    tinygltf::Model merged;
    merged.asset.version = "2.0";
    merged.asset.generator = "wows-geometry wows-gltf-exporter";
    merged.scenes.push_back({});
    merged.defaultScene = 0;

    std::vector<unsigned char> buf;

    for (auto &part : parts) {
        tinygltf::Model &m = part.model;

        while (buf.size() & 3)
            buf.push_back(0);
        size_t byte_off = buf.size();

        if (!m.buffers.empty())
            buf.insert(buf.end(), m.buffers[0].data.begin(), m.buffers[0].data.end());

        size_t bv_start = merged.bufferViews.size();
        size_t acc_start = merged.accessors.size();

        for (const auto &bv : m.bufferViews) {
            tinygltf::BufferView nbv = bv;
            nbv.buffer = 0;
            nbv.byteOffset += (int)byte_off;
            merged.bufferViews.push_back(nbv);
        }

        for (const auto &acc : m.accessors) {
            tinygltf::Accessor nacc = acc;
            if (nacc.bufferView >= 0)
                nacc.bufferView += (int)bv_start;
            merged.accessors.push_back(nacc);
        }

        for (const auto &mesh : m.meshes) {
            tinygltf::Mesh nmesh;
            nmesh.name = part.mesh_name;
            for (const auto &prim : mesh.primitives) {
                tinygltf::Primitive np = prim;
                for (auto &kv : np.attributes)
                    kv.second += (int)acc_start;
                if (np.indices >= 0)
                    np.indices += (int)acc_start;
                nmesh.primitives.push_back(np);
            }
            int mesh_idx = (int)merged.meshes.size();
            merged.meshes.push_back(nmesh);

            tinygltf::Node node;
            node.name = part.mesh_name;
            node.mesh = mesh_idx;
            if (!part.matrix.empty())
                node.matrix = part.matrix;
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

/* ── geometry mapping_id helpers ────────────────────────────────── */

static std::vector<uint32_t> read_geom_mapping_ids(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return {};
    uint8_t hdr[72];
    if (fread(hdr, 1, 72, f) < 72) {
        fclose(f);
        return {};
    }
    uint32_t n;
    memcpy(&n, hdr + 12, 4);
    uint64_t off;
    memcpy(&off, hdr + 32, 8);
    fseek(f, (long)off, SEEK_SET);
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t e[16];
        if (fread(e, 1, 16, f) < 16)
            break;
        uint32_t mid;
        memcpy(&mid, e, 4);
        ids.push_back(mid);
    }
    fclose(f);
    return ids;
}

static std::map<uint32_t, uint32_t> read_geom_tri_counts(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return {};
    uint8_t hdr[72];
    if (fread(hdr, 1, 72, f) < 72) {
        fclose(f);
        return {};
    }
    uint32_t n;
    memcpy(&n, hdr + 12, 4);
    uint64_t off;
    memcpy(&off, hdr + 32, 8);
    fseek(f, (long)off, SEEK_SET);
    std::map<uint32_t, uint32_t> r;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t e[16];
        if (fread(e, 1, 16, f) < 16)
            break;
        uint32_t mid, cnt;
        memcpy(&mid, e, 4);
        memcpy(&cnt, e + 12, 4);
        r[mid] = cnt / 3;
    }
    fclose(f);
    return r;
}

static std::vector<uint32_t> read_geom_mapping_ids_from_mem(const uint8_t *data, size_t size) {
    if (size < 72)
        return {};
    uint32_t n;
    memcpy(&n, data + 12, 4);
    uint64_t off;
    memcpy(&off, data + 32, 8);
    if (off + (uint64_t)n * 16 > size)
        return {};
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t mid;
        memcpy(&mid, data + off + i * 16, 4);
        ids.push_back(mid);
    }
    return ids;
}

static std::map<uint32_t, uint32_t> read_geom_tri_counts_from_mem(const uint8_t *data, size_t size) {
    if (size < 72)
        return {};
    uint32_t n;
    memcpy(&n, data + 12, 4);
    uint64_t off;
    memcpy(&off, data + 32, 8);
    if (off + (uint64_t)n * 16 > size)
        return {};
    std::map<uint32_t, uint32_t> r;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t mid, cnt;
        memcpy(&mid, data + off + i * 16, 4);
        memcpy(&cnt, data + off + i * 16 + 12, 4);
        r[mid] = cnt / 3;
    }
    return r;
}

/* ── texture finder ─────────────────────────────────────────────── */

static const char *MFM_STRIP[] = {"_skinned", "_alpha", "_ep", nullptr};

static std::string find_texture(const std::string &dir, const std::string &st, const char *channel) {
    std::vector<std::string> cands = {st};
    for (const char **s = MFM_STRIP; *s; ++s) {
        size_t sl = strlen(*s);
        if (st.size() > sl && st.compare(st.size() - sl, sl, *s) == 0) {
            cands.push_back(st.substr(0, st.size() - sl));
            break;
        }
    }
    for (auto &cs : cands)
        for (auto ext : {".dd0", ".dd1", ".dds"}) {
            std::string p = dir + "/" + cs + channel + ext;
            if (wows_stitch_file_exists(p))
                return p;
        }
    return "";
}

static std::vector<uint8_t> load_texture_bytes(const std::string &tdir, const std::string &tstem, const char *channel,
                                               const std::string &game_dir, wows_file_provider_t file_provider,
                                               int max_sz) {
    std::string dds = find_texture(tdir, tstem, channel);
    if (!dds.empty())
        return wows_stitch_dds_to_png(dds, max_sz);

    if (!file_provider)
        return {};

    std::string mdir = wows_stitch_normalize_slashes(tdir);
    if (mdir.size() > game_dir.size() && mdir.compare(0, game_dir.size(), game_dir) == 0)
        mdir = mdir.substr(game_dir.size());
    if (!mdir.empty() && mdir[0] == '/')
        mdir = mdir.substr(1);

    std::vector<std::string> cands = {tstem};
    for (const char **s = MFM_STRIP; *s; ++s) {
        size_t sl = strlen(*s);
        if (tstem.size() > sl && tstem.compare(tstem.size() - sl, sl, *s) == 0) {
            cands.push_back(tstem.substr(0, tstem.size() - sl));
            break;
        }
    }
    for (const auto &cs : cands) {
        for (const char *ext : {".dd0", ".dd1", ".dds"}) {
            std::string rel = mdir + "/" + cs + channel + ext;
            auto buf = file_provider(rel);
            if (!buf.empty()) {
                auto png = wows_stitch_dds_to_png_from_memory(buf.data(), buf.size(), max_sz);
                if (!png.empty())
                    return png;
            }
        }
    }
    return {};
}

/* ── LOD selection ──────────────────────────────────────────────── */

static int best_lod(const wows_assets_bin_visual_info_t *vi, const std::set<uint32_t> &dmg,
                    const std::map<uint32_t, uint32_t> &tc) {
    int best = 0, btris = -1;
    for (size_t i = 0; i < vi->lod_count; ++i) {
        int tris = 0;
        for (size_t j = 0; j < vi->lods[i].count; ++j) {
            uint32_t mid = vi->lods[i].mapping_ids[j];
            if (dmg.count(mid))
                continue;
            auto it = tc.find(mid);
            if (it != tc.end())
                tris += (int)it->second;
        }
        if (tris > btris) {
            btris = tris;
            best = (int)i;
        }
    }
    return best;
}

/* ── texture application ────────────────────────────────────────── */

void wows_stitch_apply_textures(tinygltf::Model &model, const std::vector<std::string> &geom_order,
                                wows_assets_bin_pdb_t *pdb, const std::string &game_dir, int lod_level,
                                bool excl_damage, int max_tex, std::function<void(int)> progress_cb,
                                wows_file_provider_t file_provider) {
    if (!pdb || geom_order.empty())
        return;

    if (model.samplers.empty()) {
        tinygltf::Sampler samp;
        samp.magFilter = 9729;
        samp.minFilter = 9987;
        samp.wrapS = samp.wrapT = 10497;
        model.samplers.push_back(samp);
    }

    std::map<std::string, int> stem_to_mat;

    auto embed_png = [&](const std::vector<uint8_t> &png) -> int {
        auto &buf = model.buffers[0].data;
        while (buf.size() & 3)
            buf.push_back(0);
        size_t boff = buf.size();
        buf.insert(buf.end(), png.begin(), png.end());
        tinygltf::BufferView bv;
        bv.buffer = 0;
        bv.byteOffset = boff;
        bv.byteLength = png.size();
        int bvi = (int)model.bufferViews.size();
        model.bufferViews.push_back(bv);
        tinygltf::Image img;
        img.bufferView = bvi;
        img.mimeType = "image/png";
        int ii = (int)model.images.size();
        model.images.push_back(img);
        tinygltf::Texture tex;
        tex.sampler = 0;
        tex.source = ii;
        int ti = (int)model.textures.size();
        model.textures.push_back(tex);
        return ti;
    };

    auto ensure_mat = [&](const std::string &name, const std::vector<uint8_t> &png) -> int {
        auto it = stem_to_mat.find(name);
        if (it != stem_to_mat.end())
            return it->second;
        int ti = embed_png(png);
        tinygltf::Material mat;
        mat.name = name;
        mat.doubleSided = true;
        mat.pbrMetallicRoughness.baseColorTexture.index = ti;
        mat.pbrMetallicRoughness.metallicFactor = 0.0;
        mat.pbrMetallicRoughness.roughnessFactor = 0.8;
        int mi = (int)model.materials.size();
        model.materials.push_back(mat);
        stem_to_mat[name] = mi;
        return mi;
    };

    struct GtData {
        std::vector<uint32_t> geo_map_ids;
        std::set<uint32_t> allowed;
        std::map<uint32_t, int> geo_map_mat;
        std::map<uint32_t, std::string> geo_map_name;
    };
    std::map<std::string, GtData> cache;

    /* denominator for per-geometry progress within each phase */
    size_t n_geoms = geom_order.size();
    if (n_geoms == 0)
        return;
    auto report = [&](int pct) {
        if (progress_cb)
            progress_cb(pct);
    };

    /* strip game_dir prefix to produce archive-relative path */
    auto geom_rel = [&](const std::string &p) -> std::string {
        std::string r = wows_stitch_normalize_slashes(p);
        if (r.size() > game_dir.size() && r.compare(0, game_dir.size(), game_dir) == 0)
            r = r.substr(game_dir.size());
        if (!r.empty() && r[0] == '/')
            r = r.substr(1);
        return r;
    };

    for (size_t mi = 0; mi < model.meshes.size(); ++mi) {
        if (mi >= geom_order.size())
            break;
        const std::string &gp = geom_order[mi];
        if (gp.empty()) {
            report(20 + (int)((mi + 1) * 80 / n_geoms));
            continue;
        }

        if (!cache.count(gp)) {
            GtData &gt = cache[gp];
            std::string suf = wows_stitch_geom_to_visual_suffix(gp);
            wows_assets_bin_visual_info_t *vi = wows_assets_bin_get_visual_info(pdb, suf.c_str());
            if (vi) {
                gt.geo_map_ids = read_geom_mapping_ids(gp);
                if (gt.geo_map_ids.empty() && file_provider) {
                    auto fbuf = file_provider(geom_rel(gp));
                    if (!fbuf.empty())
                        gt.geo_map_ids = read_geom_mapping_ids_from_mem(fbuf.data(), fbuf.size());
                }

                const std::string geom_tag = wows_stitch_path_basename(gp);

                std::set<uint32_t> dmg;
                for (size_t ri = 0; ri < vi->rs_count; ++ri)
                    if (vi->render_sets[ri].is_damage)
                        dmg.insert(vi->render_sets[ri].indices_mapping_id);

                int clod;
                if (lod_level < 0 && vi->lod_count > 0) {
                    auto tc = read_geom_tri_counts(gp);
                    if (tc.empty() && file_provider) {
                        auto fbuf = file_provider(geom_rel(gp));
                        if (!fbuf.empty())
                            tc = read_geom_tri_counts_from_mem(fbuf.data(), fbuf.size());
                    }
                    clod = best_lod(vi, dmg, tc);
                } else {
                    clod = std::max(0, lod_level);
                }

                vlog(geom_tag.c_str(), "LOD%d/%zu, %zu render sets\n", clod, vi->lod_count, vi->rs_count);

                if (vi->lod_count > 0 && (size_t)clod < vi->lod_count)
                    for (size_t j = 0; j < vi->lods[clod].count; ++j)
                        gt.allowed.insert(vi->lods[clod].mapping_ids[j]);
                else
                    for (size_t ri = 0; ri < vi->rs_count; ++ri)
                        gt.allowed.insert(vi->render_sets[ri].indices_mapping_id);

                if (excl_damage)
                    for (uint32_t m : dmg)
                        gt.allowed.erase(m);

                /* pass 1: extract render-set metadata (names) — phase 2 */
                for (size_t ri = 0; ri < vi->rs_count; ++ri) {
                    const wows_assets_bin_rs_t &rs = vi->render_sets[ri];
                    uint32_t geo_map_id = rs.indices_mapping_id;
                    if (!gt.allowed.count(geo_map_id))
                        continue;
                    if (rs.section_name[0])
                        gt.geo_map_name[geo_map_id] = rs.section_name;
                    else if (rs.node_name[0])
                        gt.geo_map_name[geo_map_id] = rs.node_name;
                }
                /* pass 2: load textures — phase 3 */
                for (size_t ri = 0; ri < vi->rs_count; ++ri) {
                    const wows_assets_bin_rs_t &rs = vi->render_sets[ri];
                    uint32_t geo_map_id = rs.indices_mapping_id;
                    if (!gt.allowed.count(geo_map_id))
                        continue;
                    if (!rs.mfm_path[0] || max_tex <= 0)
                        continue;

                    std::string mfm = wows_stitch_normalize_slashes(rs.mfm_path);
                    auto sl = mfm.rfind('/');
                    std::string mdir = (sl != std::string::npos) ? mfm.substr(0, sl) : ".";
                    std::string mfile = (sl != std::string::npos) ? mfm.substr(sl + 1) : mfm;
                    std::string tstem = mfile.size() > 4 && mfile.compare(mfile.size() - 4, 4, ".mfm") == 0
                                            ? mfile.substr(0, mfile.size() - 4)
                                            : mfile;
                    std::string tdir = game_dir + "/" + mdir;

                    std::vector<uint8_t> png = load_texture_bytes(tdir, tstem, "_a", game_dir, file_provider, max_tex);
                    if (png.empty()) {
                        vlog(geom_tag.c_str(), "no albedo texture: %s\n", tstem.c_str());
                        continue;
                    }
                    int mat = ensure_mat(tstem, png);
                    gt.geo_map_mat[geo_map_id] = mat;
                    vlog(geom_tag.c_str(), "albedo %s → material slot %d (%zuKB)\n", tstem.c_str(), mat,
                         png.size() / 1024);
                }
                wows_assets_bin_visual_info_free(vi);
            }
        }

        GtData &gt = cache.at(gp);
        std::vector<tinygltf::Primitive> kept;
        std::vector<std::string> kept_names;
        for (size_t pi = 0; pi < model.meshes[mi].primitives.size(); ++pi) {
            tinygltf::Primitive &prim = model.meshes[mi].primitives[pi];
            std::string rs_name;
            if (pi < gt.geo_map_ids.size()) {
                uint32_t geo_map_id = gt.geo_map_ids[pi];
                if (!gt.allowed.empty() && !gt.allowed.count(geo_map_id))
                    continue;
                auto it = gt.geo_map_mat.find(geo_map_id);
                if (it != gt.geo_map_mat.end())
                    prim.material = it->second;
                auto nit = gt.geo_map_name.find(geo_map_id);
                if (nit != gt.geo_map_name.end())
                    rs_name = nit->second;
            }
            kept.push_back(prim);
            kept_names.push_back(rs_name);
        }
        model.meshes[mi].primitives = std::move(kept);

        /* split primitives into named child nodes */
        for (size_t pi = 0; pi < kept_names.size(); ++pi)
            if (kept_names[pi].empty())
                kept_names[pi] = model.meshes[mi].name + "_prim_" + std::to_string(pi);
        {
            std::map<std::string, int> name_count;
            for (const auto &n : kept_names)
                name_count[n]++;
            std::map<std::string, int> name_idx;
            for (auto &n : kept_names)
                if (name_count[n] > 1)
                    n += "[" + std::to_string(name_idx[n]++) + "]";
        }
        {
            int parent_ni = -1;
            for (int ni = 0; ni < (int)model.nodes.size(); ++ni)
                if (model.nodes[ni].mesh == (int)mi) {
                    parent_ni = ni;
                    break;
                }
            if (parent_ni >= 0) {
                /* copy primitives before pushing to model.meshes to avoid reallocation invalidating refs */
                std::vector<tinygltf::Primitive> prims = model.meshes[mi].primitives;
                std::vector<int> children;
                for (size_t pi = 0; pi < prims.size(); ++pi) {
                    tinygltf::Mesh cmesh;
                    cmesh.name = kept_names[pi];
                    cmesh.primitives.push_back(prims[pi]);
                    int cmesh_idx = (int)model.meshes.size();
                    model.meshes.push_back(cmesh);
                    tinygltf::Node cnode;
                    cnode.name = kept_names[pi];
                    cnode.mesh = cmesh_idx;
                    int cnode_idx = (int)model.nodes.size();
                    model.nodes.push_back(cnode);
                    children.push_back(cnode_idx);
                }
                model.nodes[parent_ni].mesh = -1;
                model.nodes[parent_ni].children = children;
                model.meshes[mi].primitives.clear();
            }
        }
        report(20 + (int)((mi + 1) * 80 / n_geoms));
    }
}

void wows_stitch_apply_default_material(tinygltf::Model &model) {
    /* find or create the fallback matte light-grey material */
    int default_mat = -1;
    for (int i = 0; i < (int)model.materials.size(); ++i) {
        if (model.materials[i].name == "__default_grey") {
            default_mat = i;
            break;
        }
    }
    if (default_mat < 0) {
        tinygltf::Material mat;
        mat.name = "__default_grey";
        mat.doubleSided = true;
        mat.pbrMetallicRoughness.baseColorFactor = {0.8, 0.8, 0.8, 1.0};
        mat.pbrMetallicRoughness.metallicFactor = 0.0;
        mat.pbrMetallicRoughness.roughnessFactor = 1.0;
        default_mat = (int)model.materials.size();
        model.materials.push_back(mat);
    }

    for (auto &mesh : model.meshes)
        for (auto &prim : mesh.primitives)
            if (prim.material < 0)
                prim.material = default_mat;
}
