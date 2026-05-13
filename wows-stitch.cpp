/* wows-stitch: stitch WoWS ship geometry parts into a single GLB.
 *
 * Uses libpython (via Python C API) exclusively for GameParams.data
 * (byte-reversed zlib-compressed pickle); everything else is C/C++.
 *
 * Pipeline:
 *   1. Python C API  → embedded game_params.py → hull model + mounts
 *   2. assets_bin.cpp → HP_ world-space transforms + BlendBone corrections
 *   3. lib/stitch.cpp → geometry → tinygltf::Model, DDS textures, GLB merge
 *   4. tinygltf → write output GLB
 */
#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <tiny_gltf.h>

extern "C" {
#include "wows-geometry.h"
#include "assets_bin.h"
}
#include "stitch.h"

#include <argp.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

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

/* ── Python C API: load ship info from GameParams ─────────────────── */

static std::string py_str(PyObject *o) {
    if (!o || !PyUnicode_Check(o)) return "";
    const char *s = PyUnicode_AsUTF8(o);
    return s ? s : "";
}

static bool load_hull_info(const char *gameparams_path,
                           const char *ship_name,
                           const char *hull_sel, /* nullptr = latest */
                           HullInfo &out) {
    PyObject *mod = PyImport_AddModule("game_params");
    if (!mod) { PyErr_Print(); return false; }
    PyObject *ns = PyModule_GetDict(mod);
    {
        PyObject *code = Py_CompileString(GAME_PARAMS_PY, "game_params.py", Py_file_input);
        if (!code) { PyErr_Print(); return false; }
        PyObject *r = PyEval_EvalCode(code, ns, ns);
        Py_DECREF(code);
        if (!r) { PyErr_Print(); return false; }
        Py_DECREF(r);
    }

    PyObject *gp = PyObject_CallMethod(mod, "load_game_params", "s", gameparams_path);
    if (!gp) { PyErr_Print(); return false; }

    PyObject *root = PyObject_CallMethod(mod, "get_params_root", "O", gp);
    Py_DECREF(gp);
    if (!root) { PyErr_Print(); return false; }

    /* find ship: exact match first, then case-insensitive word-pattern regex */
    std::string found_name = ship_name;
    PyObject *ship_data = PyDict_GetItemString(root, ship_name);
    if (!ship_data) {
        /* build ".*word1.*word2.*" from space/dot/underscore/dash-separated words */
        std::string pat = ".*";
        std::string word;
        for (unsigned char c : std::string(ship_name)) {
            if (c == ' ' || c == '.' || c == '_' || c == '-') {
                if (!word.empty()) { pat += word + ".*"; word.clear(); }
            } else {
                word += (char)tolower(c);
            }
        }
        if (!word.empty()) pat += word + ".*";
        std::regex re(pat, std::regex::icase);
        /* collect matches that are actual ships (have ShipUpgradeInfo) */
        std::vector<std::pair<std::string,PyObject*>> hits;
        PyObject *pk, *pv;
        Py_ssize_t pos = 0;
        while (PyDict_Next(root, &pos, &pk, &pv)) {
            std::string k = py_str(pk);
            if (!std::regex_match(k, re)) continue;
            if (!PyDict_Check(pv)) continue;
            if (!PyDict_GetItemString(pv, "ShipUpgradeInfo")) continue;
            hits.push_back({k, pv});
        }
        if (hits.size() == 1) {
            found_name = hits[0].first;
            ship_data  = hits[0].second;
        } else if (hits.size() > 1) {
            fprintf(stderr, "Ambiguous ship pattern '%s' matches:\n", ship_name);
            for (auto &h : hits) fprintf(stderr, "  %s\n", h.first.c_str());
            fprintf(stderr, "Use a more specific name.\n");
            Py_DECREF(root);
            return false;
        }
    }
    if (!ship_data) {
        fprintf(stderr, "Ship '%s' not found in GameParams.\n", ship_name);
        Py_DECREF(root);
        return false;
    }
    if (found_name != ship_name)
        fprintf(stderr, "Matched ship: %s\n", found_name.c_str());

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
        /* last upgrade in sorted order (= highest hull letter, e.g. C > B > A) */
        PyObject *keys = PyDict_Keys(upgrades);
        PyList_Sort(keys);
        Py_ssize_t nk = PyList_Size(keys);
        if (nk > 0) {
            PyObject *k = PyList_GetItem(keys, nk - 1);
            upg_data = PyDict_GetItem(upgrades, k);
            stitch_vlog("  Hull upgrade: %s\n", py_str(k).c_str());
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
    {"ship",         's', "NAME",  0, "Ship name / pattern (words joined as .*word1.*word2.*, case-insensitive)"},
    {"output",       'o', "FILE",  0, "Output .glb file"},
    {"hull",         'H', "UPG",   0, "Hull upgrade name substring (default: latest)"},
    {"no-turrets",   't', nullptr, 0, "Exclude turret / mounted-component models (default: included)"},
    {"assets-bin",   'a', "FILE",  0, "assets.bin (auto-detected from -d if omitted)"},
    {"no-textures",  'T', nullptr, 0, "Skip DDS texture application (default: applied)"},
    {"texture-size", 'Z', "N",     0, "Max texture dimension in pixels (default: 2048)"},
    {"lod",          'L', "N",     0, "LOD level to export (-1=auto, default: -1)"},
    {"damage",       'D', nullptr, 0, "Include damage/crack geometry (default: excluded)"},
    {"verbose",      'v', nullptr, 0, "Verbose progress output"},
    {0}
};

struct Args {
    char *gameparams   = nullptr;
    char *game_dir     = nullptr;
    char *ship         = nullptr;
    char *output       = nullptr;
    char *hull         = nullptr;
    char *assets_bin   = nullptr;
    bool  with_turrets = true;
    bool  textures     = true;
    int   tex_size     = 2048;
    int   lod          = -1;
    bool  damage       = false;
    bool  verbose      = false;
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
    case 't': a->with_turrets = false; break;
    case 'T': a->textures     = false; break;
    case 'Z': a->tex_size     = atoi(arg); break;
    case 'L': a->lod          = atoi(arg); break;
    case 'D': a->damage       = true; break;
    case 'v': a->verbose      = true; break;
    default:  return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = {options, parse_opt, nullptr, doc};

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv) {
    Args args;
    argp_parse(&argp, argc, argv, 0, nullptr, &args);
    g_stitch_verbose = args.verbose;

    if (!args.game_dir || !args.ship || !args.output) {
        fprintf(stderr, "Error: -d, -s, and -o are required.\n");
        return 1;
    }

    std::string game_dir = stitch_normalize_slashes(args.game_dir);

    /* ── auto-detect GameParams.data ─── */
    std::string gameparams_path = args.gameparams ? args.gameparams : "";
    if (gameparams_path.empty()) {
        gameparams_path = stitch_find_game_file(game_dir, "GameParams.data");
        if (gameparams_path.empty()) {
            fprintf(stderr,
                    "Error: GameParams.data not found under %s.\n"
                    "       Supply -g <path> to specify it explicitly.\n",
                    game_dir.c_str());
            return 1;
        }
        stitch_vlog("Auto-detected GameParams: %s\n", gameparams_path.c_str());
    }

    /* ── auto-detect assets.bin ─── */
    std::string assets_bin_path = args.assets_bin ? args.assets_bin : "";
    if (assets_bin_path.empty()) {
        assets_bin_path = stitch_find_game_file(game_dir, "assets.bin");
        if (!assets_bin_path.empty())
            stitch_vlog("Auto-detected assets.bin: %s\n", assets_bin_path.c_str());
    }

    stitch_vlog("Loading GameParams …\n");
    Py_Initialize();

    HullInfo hull;
    if (!load_hull_info(gameparams_path.c_str(), args.ship, args.hull, hull)) {
        Py_Finalize();
        return 1;
    }

    Py_Finalize();

    stitch_vlog("Hull model: %s\n", hull.hull_model.c_str());
    stitch_vlog("Mounts:     %zu\n", hull.mounts.size());

    /* ── hull geometry files ─── */
    std::vector<std::string> hull_geoms = stitch_find_hull_geoms(hull.hull_model, game_dir);
    if (hull_geoms.empty()) {
        fprintf(stderr, "Error: no hull geometry files found for %s\n",
                hull.hull_model.c_str());
        return 1;
    }
    stitch_vlog("Hull parts: %zu\n", hull_geoms.size());

    /* ── HP transforms and BlendBone corrections ─── */
    std::map<std::string, Mat16d> hp_transforms;
    std::map<std::string, Mat16d> bb_corrections;

    if (args.with_turrets && !assets_bin_path.empty()) {
        stitch_vlog("Loading HP transforms from assets.bin …\n");

        for (const auto &gp : hull_geoms) {
            std::string suffix = stitch_geom_to_visual_suffix(gp);
            assets_bin_hp_list_t *hp = assets_bin_get_hp_transforms(
                assets_bin_path.c_str(), suffix.c_str());
            if (!hp) continue;
            for (size_t i = 0; i < hp->count; ++i) {
                hp_transforms[hp->entries[i].name] =
                    stitch_float_to_double_mat(hp->entries[i].mat);
            }
            assets_bin_hp_list_free(hp);
        }
        stitch_vlog("  %zu HP_ transforms.\n", hp_transforms.size());

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
                        stitch_float_to_double_mat(bb->entries[i].correction);
                assets_bin_bb_list_free(bb);
            }
        }
    }

    /* ── convert + collect GLB parts ─── */
    std::vector<GlbPart> parts;

    for (const auto &gp : hull_geoms) {
        stitch_vlog("  Hull part: %s …\n", stitch_path_basename(gp).c_str());
        GlbPart part;
        part.mesh_name = stitch_stem(stitch_path_basename(gp));
        part.geom_path = gp;
        if (stitch_geom_to_model(gp, part.model))
            parts.push_back(std::move(part));
    }

    if (parts.empty()) {
        fprintf(stderr, "Error: no hull parts could be converted.\n");
        return 1;
    }

    if (args.with_turrets) {
        std::map<std::string, std::string> model_to_geom_map;
        for (const auto &m : hull.mounts) {
            if (model_to_geom_map.count(m.model_path)) continue;
            std::string gp = stitch_model_to_geom_path(m.model_path, game_dir);
            model_to_geom_map[m.model_path] = stitch_file_exists(gp) ? gp : "";
        }

        std::vector<MountEntry> sorted_mounts = hull.mounts;
        std::sort(sorted_mounts.begin(), sorted_mounts.end(),
                  [](const MountEntry &a, const MountEntry &b) {
                      return a.hp_name < b.hp_name;
                  });

        for (const auto &m : sorted_mounts) {
            const std::string &geom_path = model_to_geom_map[m.model_path];
            if (geom_path.empty()) continue;

            Mat16d transform;
            auto hp_it = hp_transforms.find(m.hp_name);
            if (hp_it != hp_transforms.end()) {
                transform = hp_it->second;
                auto bb_it = bb_corrections.find(m.model_path);
                if (bb_it != bb_corrections.end())
                    transform = stitch_mat4_mul_d(transform, bb_it->second);
            }

            std::string label = stitch_stem(stitch_path_basename(geom_path)) + " (" + m.hp_name + ")";
            stitch_vlog("  Turret: %s …\n", label.c_str());

            GlbPart part;
            part.mesh_name = label;
            part.geom_path = geom_path;
            part.matrix    = transform;
            if (stitch_geom_to_model(geom_path, part.model))
                parts.push_back(std::move(part));
        }
    }

    stitch_vlog("Merging %zu part(s) …\n", parts.size());
    tinygltf::Model merged = stitch_merge_parts(parts);

    if (args.textures && !assets_bin_path.empty()) {
        stitch_vlog("Applying textures (size=%d, lod=%d, damage=%s) …\n",
                    args.tex_size, args.lod, args.damage ? "yes" : "no");
        assets_bin_pdb_t *pdb = assets_bin_pdb_open(assets_bin_path.c_str());
        if (!pdb) {
            fprintf(stderr, "Warning: failed to open assets.bin for textures.\n");
        } else {
            std::vector<std::string> geom_order;
            for (const auto &p : parts) geom_order.push_back(p.geom_path);
            stitch_apply_textures(merged, geom_order, pdb, game_dir,
                                  args.lod, !args.damage, args.tex_size);
            assets_bin_pdb_free(pdb);
        }
    } else if (args.textures) {
        fprintf(stderr, "Warning: textures enabled but assets.bin not found (use -a or -d).\n");
    }

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
