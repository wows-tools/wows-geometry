#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include "game_params.h"

#include <cstdio>
#include <regex>
#include <string>
#include <vector>

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

static std::string py_str(PyObject *o) {
    if (!o || !PyUnicode_Check(o)) return "";
    const char *s = PyUnicode_AsUTF8(o);
    return s ? s : "";
}

bool load_hull_info(const char *gameparams_path,
                    const char *ship_name,
                    const char *hull_sel,
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
        std::vector<std::pair<std::string, PyObject *>> hits;
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
        Py_DECREF(info);
        return false;
    }

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
            Py_DECREF(info);
            return false;
        }
    } else {
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
            Py_DECREF(info);
            return false;
        }
    }

    out.hull_model = py_str(PyDict_GetItemString(upg_data, "hull_model"));

    PyObject *mounts = PyDict_GetItemString(upg_data, "mounts");
    if (mounts && PyDict_Check(mounts)) {
        PyObject *pk, *pv;
        Py_ssize_t pos = 0;
        while (PyDict_Next(mounts, &pos, &pk, &pv)) {
            std::string hp  = py_str(pk);
            std::string mdl = py_str(PyDict_GetItemString(pv, "model"));
            if (!mdl.empty())
                out.mounts.push_back({hp, mdl});
        }
    }

    Py_DECREF(info);
    return true;
}
