#!/usr/bin/env python3
"""
Stitch a ship's geometry parts into a single GLB.

How it works
============
1. Parse GameParams.data to find:
   - The hull upgrade config for the chosen hull (model path, mount points).
2. Locate all hull-part .geometry files by scanning the model directory.
   Hull parts share the same coordinate space, so no transform is needed.
3. For each geometry file, invoke the wows-geometry CLI to produce a
   temporary per-part GLB.
4. Merge all per-part GLBs into a single GLB using a minimal binary merger.
5. Optionally include turret / mounted-component models at their correct
   hardpoint positions (requires assets.bin parsing — see --with-turrets).

Turret placement
================
Exact turret transforms are stored in the VisualPrototype inside assets.bin
(BigWorld PrototypeDatabase format, parsed by assets_bin.py).

When --with-turrets is given and assets.bin is accessible, this script:
  - Derives the hull's .visual path suffix from the hull model path
  - Looks it up in assets.bin's path storage
  - Parses the VisualPrototype record (blob 1)
  - Extracts world-space HP_* transforms by composing the node hierarchy
  - Applies each HP_ transform as the mesh matrix for the corresponding turret

If assets.bin is not available, turrets fall back to origin (0,0,0).
Without --with-turrets (default), only hull geometry parts are stitched.
"""

import argparse
import glob
import json
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Optional

# Import our parsers from the same directory
sys.path.insert(0, os.path.dirname(__file__))
from game_params import load_game_params, get_params_root, extract_ship, list_ships
from assets_bin import get_hp_transforms, model_path_to_visual_suffix
from apply_textures import texture_hull_glb


# ---------------------------------------------------------------------------
# GLB format constants
# ---------------------------------------------------------------------------

GLB_MAGIC    = 0x46546C67  # "glTF" LE
GLB_VERSION  = 2
CHUNK_JSON   = 0x4E4F534A  # "JSON"
CHUNK_BIN    = 0x004E4942  # "BIN\0"


# ---------------------------------------------------------------------------
# Minimal GLB read / write helpers
# ---------------------------------------------------------------------------

def _read_glb(path: str) -> tuple[dict, bytes]:
    """Read a GLB file, return (json_dict, binary_bytes)."""
    with open(path, "rb") as f:
        data = f.read()

    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != GLB_VERSION:
        raise ValueError(f"Not a valid GLB file: {path}")

    offset = 12
    json_dict: dict = {}
    binary: bytes = b""

    while offset < length:
        chunk_len, chunk_type = struct.unpack_from("<II", data, offset)
        offset += 8
        chunk_data = data[offset : offset + chunk_len]
        offset += chunk_len

        if chunk_type == CHUNK_JSON:
            json_dict = json.loads(chunk_data.decode("utf-8").rstrip("\x00"))
        elif chunk_type == CHUNK_BIN:
            binary = bytes(chunk_data)

    return json_dict, binary


def _write_glb(json_dict: dict, binary: bytes, path: str) -> None:
    """Write a GLB file from a JSON dict and a binary blob."""
    json_bytes = json.dumps(json_dict, separators=(",", ":")).encode("utf-8")
    # JSON chunk must be padded to 4-byte boundary with spaces
    json_pad = (4 - len(json_bytes) % 4) % 4
    json_bytes += b" " * json_pad

    # Binary chunk must be padded to 4-byte boundary with zeros
    bin_pad = (4 - len(binary) % 4) % 4 if binary else 0
    binary_padded = binary + b"\x00" * bin_pad

    json_chunk = struct.pack("<II", len(json_bytes), CHUNK_JSON) + json_bytes
    bin_chunk  = struct.pack("<II", len(binary_padded), CHUNK_BIN) + binary_padded if binary_padded else b""

    total = 12 + len(json_chunk) + len(bin_chunk)
    header = struct.pack("<III", GLB_MAGIC, GLB_VERSION, total)

    with open(path, "wb") as f:
        f.write(header + json_chunk + bin_chunk)


# ---------------------------------------------------------------------------
# Matrix helpers
# ---------------------------------------------------------------------------

def _mat4_mul_col(a: list[float], b: list[float]) -> list[float]:
    """Multiply two 4×4 column-major matrices: result = a × b."""
    out = [0.0] * 16
    for col in range(4):
        for row in range(4):
            s = 0.0
            for k in range(4):
                s += a[k * 4 + row] * b[col * 4 + k]
            out[col * 4 + row] = s
    return out


# ---------------------------------------------------------------------------
# Multi-GLB merger
# ---------------------------------------------------------------------------

def merge_glbs(parts: list[tuple[dict, bytes, str, Optional[list[float]]]]) -> tuple[dict, bytes]:
    """
    Merge multiple (json_dict, binary, mesh_name, transform_16f) tuples into one GLB.

    transform_16f: column-major 4x4 matrix as 16 floats (None → identity / no node TRS).
    All geometry stays in one buffer; indices are renumbered to avoid collisions.
    """
    merged_binary = bytearray()

    acc_offset      = 0  # running accessor index
    bv_offset       = 0  # running bufferView index
    mesh_offset     = 0  # running mesh index
    node_offset     = 0  # running node index
    byte_offset     = 0  # running byte offset into merged binary

    merged_accessors    : list[dict] = []
    merged_buffer_views : list[dict] = []
    merged_meshes       : list[dict] = []
    merged_nodes        : list[dict] = []

    for (jd, binary, mesh_name, transform) in parts:
        part_bv_list  = jd.get("bufferViews", [])
        part_acc_list = jd.get("accessors",   [])
        part_mesh_list= jd.get("meshes",      [])

        # Append this part's binary blob (4-byte aligned)
        pad = (4 - len(merged_binary) % 4) % 4 if merged_binary else 0
        merged_binary += b"\x00" * pad
        byte_offset = len(merged_binary)
        merged_binary += binary

        # Remap bufferViews: shift byteOffset by byte_offset
        bv_start = len(merged_buffer_views)
        for bv in part_bv_list:
            new_bv = dict(bv)
            new_bv["byteOffset"] = bv.get("byteOffset", 0) + byte_offset
            new_bv["buffer"] = 0  # single merged buffer
            merged_buffer_views.append(new_bv)

        # Remap accessors: shift bufferView index
        acc_start = len(merged_accessors)
        for ac in part_acc_list:
            new_ac = dict(ac)
            if "bufferView" in new_ac:
                new_ac["bufferView"] = new_ac["bufferView"] + bv_start
            merged_accessors.append(new_ac)

        # Remap mesh primitives: shift accessor indices
        for mesh in part_mesh_list:
            new_mesh = {"name": mesh_name, "primitives": []}
            for prim in mesh.get("primitives", []):
                new_prim = dict(prim)
                new_attrs = {}
                for attr_name, acc_idx in prim.get("attributes", {}).items():
                    new_attrs[attr_name] = acc_idx + acc_start
                new_prim["attributes"] = new_attrs
                if "indices" in new_prim:
                    new_prim["indices"] = new_prim["indices"] + acc_start
                new_mesh["primitives"].append(new_prim)
            mesh_idx = len(merged_meshes)
            merged_meshes.append(new_mesh)

            # Create a node for this mesh
            node: dict = {"mesh": mesh_idx, "name": mesh_name}
            if transform is not None:
                node["matrix"] = transform  # column-major 4x4
            merged_nodes.append(node)

    # Build root scene
    scene_nodes = list(range(len(merged_nodes)))
    total_bytes = len(merged_binary)

    merged_json = {
        "asset": {"version": "2.0", "generator": "wows-geometry stitch_ship.py"},
        "scene": 0,
        "scenes": [{"nodes": scene_nodes}],
        "nodes": merged_nodes,
        "meshes": merged_meshes,
        "accessors": merged_accessors,
        "bufferViews": merged_buffer_views,
        "buffers": [{"byteLength": total_bytes}],
    }

    return merged_json, bytes(merged_binary)


# ---------------------------------------------------------------------------
# Geometry discovery
# ---------------------------------------------------------------------------

def model_path_to_geometry(model_path: str, game_dir: str) -> Optional[str]:
    """Convert a .model path to a local .geometry file path."""
    geom_rel = model_path.replace(".model", ".geometry")
    candidate = os.path.join(game_dir, geom_rel)
    if os.path.isfile(candidate):
        return candidate
    return None


def find_hull_geometry_files(hull_model_path: str, game_dir: str) -> list[str]:
    """
    Find all hull-part .geometry files for a ship model directory.

    The hull model path (from GameParams) is like:
      content/gameplay/japan/ship/battleship/JSB007_Kongo_1942/JSB007_Kongo_1942.model

    The ship directory contains multiple .geometry files:
      JSB007_Kongo_1942.geometry
      JSB007_Kongo_1942_Bow.geometry
      JSB007_Kongo_1942_MidFront.geometry
      ...

    All these parts share the same coordinate space and can be merged directly.
    """
    geom_path = model_path_to_geometry(hull_model_path, game_dir)
    if geom_path is None:
        return []

    ship_dir = os.path.dirname(geom_path)
    base_name = os.path.splitext(os.path.basename(geom_path))[0]

    # Find all .geometry files whose name starts with base_name but skip the
    # bare base file (e.g. JSB007_Kongo_1942.geometry) — it's a low-poly LOD
    # placeholder; the detailed sub-parts (_Bow, _MidFront, …) are the real mesh.
    all_geoms = glob.glob(os.path.join(ship_dir, "*.geometry"))
    hull_parts = sorted(
        p for p in all_geoms
        if os.path.basename(p).startswith(base_name)
        and os.path.splitext(os.path.basename(p))[0] != base_name
    )
    return hull_parts


def find_turret_geometry_files(
    mounts: dict,
    game_dir: str,
) -> dict[str, Optional[str]]:
    """
    For each HP_ mount, find its .geometry file path.
    Returns {hp_name: geometry_path_or_None}.
    """
    result = {}
    seen_models: dict[str, Optional[str]] = {}
    for hp_name, mount_info in mounts.items():
        model_path = mount_info.get("model", "")
        if not model_path:
            result[hp_name] = None
            continue
        if model_path not in seen_models:
            seen_models[model_path] = model_path_to_geometry(model_path, game_dir)
        result[hp_name] = seen_models[model_path]
    return result


# ---------------------------------------------------------------------------
# CLI invocation helpers
# ---------------------------------------------------------------------------

def run_geometry_to_glb(cli_path: str, geom_file: str, out_glb: str) -> bool:
    """Run the wows-geometry CLI to convert a .geometry to a .glb."""
    cmd = [cli_path, "-i", geom_file, "-g", out_glb]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print(f"  Warning: CLI failed for {geom_file}: {result.stderr.strip()}", file=sys.stderr)
            return False
        return True
    except FileNotFoundError:
        print(f"  Error: CLI not found at {cli_path}", file=sys.stderr)
        return False


# ---------------------------------------------------------------------------
# Main stitching logic
# ---------------------------------------------------------------------------

def stitch_ship(
    gameparams_path: str,
    game_dir: str,
    ship_name: str,
    output_glb: str,
    cli_path: str,
    hull_selection: Optional[str] = None,
    with_turrets: bool = False,
    assets_bin_path: Optional[str] = None,
    with_textures: bool = False,
    max_texture_size: int = 2048,
    lod_level: int = 0,
    exclude_damage: bool = True,
) -> None:
    print(f"Loading GameParams …", file=sys.stderr)
    gp = load_game_params(gameparams_path)
    root = get_params_root(gp)

    # Find ship param data
    param_data = root.get(ship_name)
    if param_data is None:
        # Try substring match
        needle = ship_name.lower()
        for pname, pdata in root.items():
            if not isinstance(pdata, dict):
                continue
            ti = pdata.get("typeinfo", {})
            if isinstance(ti, dict) and ti.get("type") == "Ship" and needle in pname.lower():
                param_data = pdata
                ship_name = pname
                break
    if param_data is None:
        print(f"Ship '{ship_name}' not found in GameParams.", file=sys.stderr)
        sys.exit(1)

    print(f"Found ship: {ship_name}", file=sys.stderr)

    ship_info = extract_ship(ship_name, param_data, include_armor=False)
    upgrades = ship_info.get("hull_upgrades", {})
    if not upgrades:
        print("No hull upgrades found for this ship.", file=sys.stderr)
        sys.exit(1)

    # Select hull upgrade
    if hull_selection:
        sel = hull_selection.lower()
        hull_upg_name = next(
            (k for k in sorted(upgrades) if sel in k.lower()), None
        )
        if hull_upg_name is None:
            print(f"Hull upgrade matching '{hull_selection}' not found.", file=sys.stderr)
            print(f"Available: {', '.join(sorted(upgrades))}", file=sys.stderr)
            sys.exit(1)
    else:
        hull_upg_name = sorted(upgrades)[0]

    hull_upg = upgrades[hull_upg_name]
    hull_model = hull_upg.get("hull_model", "")
    mounts = hull_upg.get("mounts", {})

    print(f"Hull upgrade: {hull_upg_name}", file=sys.stderr)
    print(f"Hull model:   {hull_model}", file=sys.stderr)
    print(f"Mounts:       {len(mounts)}", file=sys.stderr)

    if not hull_model:
        print("No hull model path found.", file=sys.stderr)
        sys.exit(1)

    # Locate hull geometry files
    hull_geoms = find_hull_geometry_files(hull_model, game_dir)
    if not hull_geoms:
        print(f"No hull geometry files found for model: {hull_model}", file=sys.stderr)
        print(f"  Looked in: {os.path.join(game_dir, os.path.dirname(hull_model))}", file=sys.stderr)
        sys.exit(1)

    print(f"\nHull parts ({len(hull_geoms)}):", file=sys.stderr)
    for g in hull_geoms:
        print(f"  {g}", file=sys.stderr)

    # Load HP transforms from assets.bin (if available and turrets requested).
    # Weapon hardpoints live in sub-part visuals (Bow, MidFront, etc.), not the
    # main hull visual, so we query each hull part visual and merge results.
    hp_transforms: dict[str, Optional[list[float]]] = {}
    if with_turrets and assets_bin_path:
        print(f"\nLoading HP transforms from assets.bin …", file=sys.stderr)
        total_hp = 0
        for geom_path in hull_geoms:
            visual = geom_path.replace(".geometry", ".visual")
            parts = visual.replace("\\", "/").split("/")
            suffix = "/".join(parts[-2:]) if len(parts) >= 2 else parts[-1]
            try:
                part_hp = get_hp_transforms(assets_bin_path, suffix)
                hp_transforms.update(part_hp)
                total_hp += len(part_hp)
                print(f"  {os.path.basename(suffix)}: {len(part_hp)} HP_ nodes", file=sys.stderr)
            except Exception as e:
                print(f"  Warning: could not load HP transforms from {suffix}: {e}", file=sys.stderr)
        print(f"  Total: {len(hp_transforms)} HP_ transforms.", file=sys.stderr)

    # Locate turret geometry files (if requested)
    turret_geoms: dict[str, Optional[str]] = {}
    if with_turrets:
        turret_geoms = find_turret_geometry_files(mounts, game_dir)
        present = {hp: p for hp, p in turret_geoms.items() if p}
        missing = {hp: mounts[hp]["model"] for hp, p in turret_geoms.items() if not p}
        print(f"\nTurret/mount models ({len(present)} found, {len(missing)} missing):", file=sys.stderr)
        for hp, path in sorted(present.items()):
            species   = mounts[hp].get("species") or ""
            transform = hp_transforms.get(hp)
            pos_str   = f"({transform[12]:.1f}, {transform[13]:.1f}, {transform[14]:.1f})" if transform else "origin"
            print(f"  {hp:30s} [{species:12s}] pos={pos_str} → {path}", file=sys.stderr)
        for hp, model in sorted(missing.items()):
            print(f"  {hp:30s} [MISSING] {model}", file=sys.stderr)

    # Convert each geometry to a temporary GLB via the C CLI
    glb_parts: list[tuple[dict, bytes, str, Optional[list[float]]]] = []
    # Tracks the geom_path for each entry added to glb_parts (same index = same mesh in merged GLB)
    geom_paths_ordered: list[str] = []

    with tempfile.TemporaryDirectory() as tmpdir:
        # Hull parts (no transform — already in ship coordinate space)
        for geom_path in hull_geoms:
            part_name = os.path.splitext(os.path.basename(geom_path))[0]
            tmp_glb = os.path.join(tmpdir, part_name + ".glb")
            print(f"  Converting hull part: {part_name} …", file=sys.stderr)
            if run_geometry_to_glb(cli_path, geom_path, tmp_glb):
                try:
                    jd, binary = _read_glb(tmp_glb)
                    glb_parts.append((jd, binary, part_name, None))
                    geom_paths_ordered.append(geom_path)
                except Exception as e:
                    print(f"  Warning: could not read {tmp_glb}: {e}", file=sys.stderr)

        if not glb_parts:
            print("No hull parts could be converted.", file=sys.stderr)
            sys.exit(1)

        # Turret models — place at HP_ world-space transform if available.
        # All turret/gun models face stern (+Z in game space) but the ship faces
        # bow (-Z), so we apply a 180° Y-axis rotation on top of the HP transform.
        _ROT180Y = [
            -1.0,  0.0, 0.0, 0.0,
             0.0,  1.0, 0.0, 0.0,
             0.0,  0.0,-1.0, 0.0,
             0.0,  0.0, 0.0, 1.0,
        ]  # column-major

        if with_turrets:
            for hp_name, geom_path in sorted(turret_geoms.items()):
                if not geom_path:
                    continue

                hp_mat = hp_transforms.get(hp_name)
                if hp_mat is not None:
                    # Compose: first rotate 180° around Y, then translate to HP position.
                    # result = hp_mat * ROT180Y  (in column-major, right-multiply)
                    transform = _mat4_mul_col(hp_mat, _ROT180Y)
                else:
                    transform = None

                part_name = os.path.splitext(os.path.basename(geom_path))[0]
                label     = f"{part_name} ({hp_name})"
                tmp_glb   = os.path.join(tmpdir, f"mount_{hp_name}.glb")
                print(f"  Converting turret:    {part_name} ({hp_name}) …", file=sys.stderr)
                if run_geometry_to_glb(cli_path, geom_path, tmp_glb):
                    try:
                        jd, binary = _read_glb(tmp_glb)
                        glb_parts.append((jd, binary, label, transform))
                        geom_paths_ordered.append(geom_path)
                    except Exception as e:
                        print(f"  Warning: could not read {tmp_glb}: {e}", file=sys.stderr)

        # Merge all parts into one GLB
        print(f"\nMerging {len(glb_parts)} parts …", file=sys.stderr)
        merged_json, merged_binary = merge_glbs(glb_parts)

        # Apply textures to all meshes (hull + turrets), filtered to chosen LOD
        if with_textures and assets_bin_path:
            merged_json, merged_binary = texture_hull_glb(
                merged_json,
                merged_binary,
                geom_paths_ordered,   # all geometry paths in mesh order
                assets_bin_path,
                hull_model,
                game_dir,
                max_texture_size=max_texture_size,
                lod_level=lod_level,
                exclude_damage=exclude_damage,
            )

        _write_glb(merged_json, merged_binary, output_glb)
        size_kb = os.path.getsize(output_glb) / 1024
        print(f"Written: {output_glb} ({size_kb:.1f} KB)", file=sys.stderr)
        print(f"  Meshes:      {len(merged_json.get('meshes', []))}", file=sys.stderr)
        print(f"  Accessors:   {len(merged_json.get('accessors', []))}", file=sys.stderr)
        print(f"  BufferViews: {len(merged_json.get('bufferViews', []))}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Stitch a ship's geometry parts into a single GLB.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  Stitch Kongo hull (auto-detect first hull upgrade):
    %(prog)s -g GameParams.data -d /game/content -s PJSB007 -o kongo.glb

  Stitch with turrets (placed at origin, no HP transform):
    %(prog)s -g GameParams.data -d /game/content -s PJSB007 -o kongo.glb --with-turrets

  Select a specific hull upgrade:
    %(prog)s -g GameParams.data -d /game/content -s PJSB007 -o kongo_b.glb --hull B_Hull

Notes:
  Turret transforms (HP_ hardpoint positions) are read from assets.bin's
  VisualPrototype records. Supply --assets-bin (or place assets.bin at
  <game-dir>/content/assets.bin) to get correctly positioned turrets.
  Without assets.bin, turrets are placed at the model origin (0,0,0).

  The -c/--cli argument should point to the compiled wows-geometry CLI binary
  (default: ./wows-geometry-cli or the path specified).
""",
    )
    ap.add_argument("-g", "--gameparams", required=True, metavar="FILE",
                    help="Path to GameParams.data")
    ap.add_argument("-d", "--game-dir", required=True, metavar="DIR",
                    help="Root game directory (parent of 'content/')")
    ap.add_argument("-s", "--ship", required=True, metavar="NAME",
                    help="Ship GameParam name or substring (e.g. PJSB007, Kongo)")
    ap.add_argument("-o", "--output", required=True, metavar="FILE",
                    help="Output .glb file path")
    ap.add_argument("-c", "--cli", metavar="PATH",
                    default=None,
                    help="Path to wows-geometry CLI binary (default: auto-detect)")
    ap.add_argument("--hull", metavar="UPGRADE",
                    help="Hull upgrade name or substring (default: first upgrade)")
    ap.add_argument("--with-turrets", action="store_true",
                    help="Include turret/mount models at their correct HP_ positions")
    ap.add_argument("--assets-bin", metavar="FILE",
                    help="Path to assets.bin (needed for turret positions and textures)")
    ap.add_argument("--textures", action="store_true",
                    help="Embed DDS textures as PNG in the output GLB (requires assets.bin)")
    ap.add_argument("--texture-size", metavar="N", type=int, default=2048,
                    help="Max texture dimension in pixels (default: 2048)")
    ap.add_argument("--lod", metavar="N", type=int, default=0,
                    help="LOD level to export: 0=highest detail (default), 1/2/3=lower")
    ap.add_argument("--damage", action="store_true",
                    help="Include damage/cross-section geometry at ship break points "
                         "(excluded by default)")
    args = ap.parse_args()

    # Auto-detect CLI binary
    cli = args.cli
    if cli is None:
        script_dir = os.path.dirname(os.path.abspath(__file__))
        project_dir = os.path.dirname(script_dir)
        candidates = [
            os.path.join(project_dir, "wows-geometry-cli"),
            os.path.join(project_dir, "build", "wows-geometry-cli"),
            "wows-geometry-cli",
        ]
        for c in candidates:
            if os.path.isfile(c) and os.access(c, os.X_OK):
                cli = c
                break
        if cli is None:
            # Try from PATH
            import shutil
            cli = shutil.which("wows-geometry-cli") or candidates[0]

    # Auto-detect assets.bin if not specified
    assets_bin = args.assets_bin
    if assets_bin is None and (args.with_turrets or args.textures):
        candidate = os.path.join(args.game_dir, "content", "assets.bin")
        if os.path.isfile(candidate):
            assets_bin = candidate
            print(f"Auto-detected assets.bin: {assets_bin}", file=sys.stderr)

    stitch_ship(
        gameparams_path=args.gameparams,
        game_dir=args.game_dir,
        ship_name=args.ship,
        output_glb=args.output,
        cli_path=cli,
        hull_selection=args.hull,
        with_turrets=args.with_turrets,
        assets_bin_path=assets_bin,
        with_textures=args.textures,
        max_texture_size=args.texture_size,
        lod_level=args.lod,
        exclude_damage=not args.damage,
    )


if __name__ == "__main__":
    main()
