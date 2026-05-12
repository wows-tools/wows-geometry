#!/usr/bin/env python3
"""
Parse GameParams.data and extract ship assembly information.

GameParams.data is a byte-reversed, zlib-compressed Python pickle.
Format: reverse(zlib(pickle(root_dict)))
  root_dict[""]["param_name"] = param_dict   (older builds)
  OR list[0]["param_name"] = param_dict       (newer builds)

Each ship param_dict has:
  typeinfo.type = "Ship"
  ShipUpgradeInfo: dict of upgrade → ucType/_Hull etc. with component lists
  <CompName>: dict with model path, HP_ mount points, armor, etc.
"""

import io
import pickle
import zlib
import json
import sys
import os
import argparse
from typing import Any

COMPONENT_TYPES = [
    "hull", "artillery", "atba", "airDefense",
    "directors", "finders", "radars", "torpedoes",
]


class _Stub:
    """
    Stand-in for unknown GameParams classes.

    Pickle restores instances by updating __dict__, so we expose the resulting
    attributes as a dict-like interface so the rest of the code can use .get(),
    iteration, isinstance(x, dict) being False is fine — we duck-type instead.
    """
    def __init__(self, *args, **kwargs):
        pass

    def get(self, key, default=None):
        return self.__dict__.get(key, default)

    def __getitem__(self, key):
        return self.__dict__[key]

    def __setitem__(self, key, val):
        self.__dict__[key] = val

    def __contains__(self, key):
        return key in self.__dict__

    def __iter__(self):
        return iter(self.__dict__)

    def items(self):
        return self.__dict__.items()

    def keys(self):
        return self.__dict__.keys()

    def values(self):
        return self.__dict__.values()

    def __repr__(self):
        return f"_Stub({self.__dict__!r})"


def _to_plain(obj: Any, _memo: dict | None = None) -> Any:
    """Recursively convert _Stub instances to plain dicts (cycle-safe)."""
    if _memo is None:
        _memo = {}
    oid = id(obj)
    if oid in _memo:
        return _memo[oid]

    if isinstance(obj, _Stub):
        result: dict = {}
        _memo[oid] = result        # register before recursing to break cycles
        result.update({str(k): _to_plain(v, _memo) for k, v in obj.__dict__.items()})
        return result
    if isinstance(obj, dict):
        result = {}
        _memo[oid] = result
        result.update({str(k) if not isinstance(k, str) else k: _to_plain(v, _memo)
                       for k, v in obj.items()})
        return result
    if isinstance(obj, list):
        result_list: list = []
        _memo[oid] = result_list
        result_list.extend(_to_plain(v, _memo) for v in obj)
        return result_list
    if isinstance(obj, tuple):
        # tuples are immutable — no cycle registration needed
        return tuple(_to_plain(v, _memo) for v in obj)
    return obj


class _PermissiveUnpickler(pickle.Unpickler):
    """Unpickler that replaces unknown classes with _Stub."""
    def find_class(self, module, name):
        try:
            return super().find_class(module, name)
        except (ModuleNotFoundError, AttributeError):
            return _Stub


def load_game_params(path: str) -> Any:
    """Load and deserialize GameParams.data."""
    with open(path, "rb") as f:
        data = f.read()
    data = data[::-1]            # reverse bytes (BigWorld packing)
    data = zlib.decompress(data) # zlib decompress
    raw = _PermissiveUnpickler(io.BytesIO(data)).load()
    return _to_plain(raw)


def get_params_root(game_params: Any) -> dict:
    """Extract the flat {param_name: param_dict} mapping from the pickle root."""
    if isinstance(game_params, dict):
        # Older builds: top-level dict with "" key
        if "" in game_params:
            inner = game_params[""]
            if isinstance(inner, dict):
                return inner
        return game_params
    if isinstance(game_params, (list, tuple)):
        first = game_params[0]
        if isinstance(first, dict):
            return first
    return {}


def _as_list(val: Any) -> list:
    """Coerce a string, list, or tuple to a list."""
    if val is None:
        return []
    if isinstance(val, str):
        return [val]
    if isinstance(val, (list, tuple)):
        return list(val)
    return []


def _read_armor(armor_dict: dict) -> dict:
    """Convert raw armor dict to {material_id: {model_index: thickness_mm}}."""
    result = {}
    for raw_key, thickness in armor_dict.items():
        try:
            k = int(raw_key)
        except (ValueError, TypeError):
            continue
        try:
            t = float(thickness)
        except (ValueError, TypeError):
            continue
        model_index = k >> 16
        material_id = k & 0xFFFF
        result.setdefault(material_id, {})[model_index] = t
    return result


def extract_ship(ship_name: str, ship_data: dict, include_armor: bool = False) -> dict:
    """
    Extract hull-upgrade configs and mount points for a ship.

    Returns a dict with structure:
    {
      "name": str,
      "hull_upgrades": {
        "<upgrade_name>": {
          "hull_component": str,
          "hull_model": str,          # .model path → replace suffix to get .geometry
          "mounts": {
            "<HP_name>": {
              "model": str,           # .model path for this mount
              "component": str,
              "component_type": str,
            }, ...
          },
          "armor": { ... }            # only if include_armor=True
        }, ...
      }
    }
    """
    result = {"name": ship_name, "hull_upgrades": {}}

    upgrade_info = ship_data.get("ShipUpgradeInfo", {})
    if not isinstance(upgrade_info, dict):
        return result

    for upgrade_name, upgrade_data in upgrade_info.items():
        if not isinstance(upgrade_data, dict):
            continue
        if upgrade_data.get("ucType") != "_Hull":
            continue

        components = upgrade_data.get("components", {})
        if not isinstance(components, dict):
            continue

        hull_comp_names = _as_list(components.get("hull"))
        if not hull_comp_names:
            continue

        hull_comp_name = hull_comp_names[0]
        hull_data = ship_data.get(hull_comp_name, {})
        if not isinstance(hull_data, dict):
            continue

        hull_model = hull_data.get("model", "")
        detection_km = hull_data.get("visibilityFactor", 0.0)

        mounts = {}
        for ct in COMPONENT_TYPES:
            comp_names = _as_list(components.get(ct))
            for comp_name in comp_names:
                comp_data = ship_data.get(comp_name, {})
                if not isinstance(comp_data, dict):
                    continue
                for key, val in comp_data.items():
                    if not key.startswith("HP_"):
                        continue
                    if not isinstance(val, dict):
                        continue
                    model_path = val.get("model", "")
                    if not model_path:
                        continue
                    typeinfo = val.get("typeinfo", {})
                    species = typeinfo.get("species", "") if isinstance(typeinfo, dict) else ""
                    mounts[key] = {
                        "model": model_path,
                        "component": comp_name,
                        "component_type": ct,
                        "species": species,
                    }

        upgrade_entry: dict = {
            "hull_component": hull_comp_name,
            "hull_model": hull_model,
            "detection_km": float(detection_km) if detection_km else 0.0,
            "mounts": mounts,
        }

        if include_armor:
            raw_armor = hull_data.get("armor", {})
            if isinstance(raw_armor, dict):
                upgrade_entry["armor"] = _read_armor(raw_armor)

        # Only store upgrades that carry useful data
        if hull_model or mounts:
            result["hull_upgrades"][upgrade_name] = upgrade_entry

    return result


def list_ships(params_root: dict) -> list[dict]:
    """Return a sorted list of {name, index, nation, model_path} for all ships."""
    ships = []
    for param_name, param_data in params_root.items():
        if not isinstance(param_data, dict):
            continue
        typeinfo = param_data.get("typeinfo", {})
        if not isinstance(typeinfo, dict):
            continue
        if typeinfo.get("type") != "Ship":
            continue

        # Quick model path extraction from first hull
        model_path = ""
        upgrade_info = param_data.get("ShipUpgradeInfo", {})
        if isinstance(upgrade_info, dict):
            for _, upg in upgrade_info.items():
                if isinstance(upg, dict) and upg.get("ucType") == "_Hull":
                    comps = upg.get("components", {})
                    hull_names = _as_list(comps.get("hull") if isinstance(comps, dict) else [])
                    if hull_names:
                        hull_data = param_data.get(hull_names[0], {})
                        if isinstance(hull_data, dict):
                            model_path = hull_data.get("model", "")
                    break

        ships.append({
            "name": param_name,
            "index": param_data.get("index", ""),
            "nation": typeinfo.get("nation", ""),
            "species": typeinfo.get("species", ""),
            "level": param_data.get("level", 0),
            "model_path": model_path,
        })

    return sorted(ships, key=lambda s: (s["nation"], s["name"]))


def _json_default(obj: Any) -> Any:
    """Fallback serialiser for types json.dumps can't handle natively."""
    if isinstance(obj, bytes):
        return obj.hex()
    if isinstance(obj, (set, frozenset)):
        return list(obj)
    return str(obj)


def main():
    ap = argparse.ArgumentParser(
        description="Parse GameParams.data and extract ship info.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  List all ships:
    %(prog)s -g GameParams.data --list

  Extract one ship by GameParam name (e.g. "PJSB018"):
    %(prog)s -g GameParams.data --ship PJSB018

  Extract with armor data:
    %(prog)s -g GameParams.data --ship PJSB018 --armor

  Search by display name substring:
    %(prog)s -g GameParams.data --search Yamato

  Dump raw entry for any param key:
    %(prog)s -g GameParams.data --key PJSB018

  Dump the entire params root as JSON:
    %(prog)s -g GameParams.data --dump -o all.json
""",
    )
    ap.add_argument("-g", "--gameparams", required=True, metavar="FILE",
                    help="Path to GameParams.data")
    ap.add_argument("--list", action="store_true",
                    help="List all ships (name, nation, species, level, model)")
    ap.add_argument("--ship", metavar="NAME",
                    help="GameParam name or model-dir substring to extract")
    ap.add_argument("--search", metavar="QUERY",
                    help="Case-insensitive substring search on ship name/index")
    ap.add_argument("--key", metavar="KEY",
                    help="Dump the raw params entry for KEY as JSON")
    ap.add_argument("--dump", action="store_true",
                    help="Dump the entire params root as JSON (can be large)")
    ap.add_argument("--armor", action="store_true",
                    help="Include armor data in output")
    ap.add_argument("-o", "--output", metavar="FILE",
                    help="Write JSON output to FILE (default: stdout)")
    args = ap.parse_args()

    print("Loading GameParams.data …", file=sys.stderr)
    gp = load_game_params(args.gameparams)
    root = get_params_root(gp)
    print(f"Loaded {len(root)} params.", file=sys.stderr)

    if args.list:
        ships = list_ships(root)
        out = json.dumps(ships, indent=2, default=_json_default)
    elif args.ship:
        # Exact name first, then substring on model_path
        param_data = root.get(args.ship)
        if param_data is None:
            needle = args.ship.lower()
            for pname, pdata in root.items():
                if not isinstance(pdata, dict):
                    continue
                if needle in pname.lower():
                    param_data = pdata
                    args.ship = pname
                    break
        if param_data is None:
            print(f"Ship '{args.ship}' not found.", file=sys.stderr)
            sys.exit(1)
        info = extract_ship(args.ship, param_data, include_armor=args.armor)
        out = json.dumps(info, indent=2, default=_json_default)
    elif args.search:
        needle = args.search.lower()
        matches = []
        for s in list_ships(root):
            if (needle in s["name"].lower() or needle in s["index"].lower()):
                matches.append(s)
        out = json.dumps(matches, indent=2, default=_json_default)
    elif args.key:
        entry = root.get(args.key)
        if entry is None:
            print(f"Key '{args.key}' not found.", file=sys.stderr)
            sys.exit(1)
        out = json.dumps(entry, indent=2, default=_json_default)
    elif args.dump:
        out = json.dumps(root, indent=2, default=_json_default)
    else:
        ap.print_help()
        sys.exit(0)

    if args.output:
        with open(args.output, "w") as f:
            f.write(out)
        print(f"Written to {args.output}", file=sys.stderr)
    else:
        print(out)


if __name__ == "__main__":
    main()
