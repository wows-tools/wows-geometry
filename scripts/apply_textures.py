#!/usr/bin/env python3
"""
Apply DDS textures to a WoWS ship GLB.

Pipeline
========
1. Parse the VisualPrototype render sets from assets.bin for the hull visual.
2. For each render set, resolve its MFM path ID to a full path in assets.bin,
   derive the texture stem from the MFM filename, and find the albedo DDS in
   the sibling textures/ directory.
3. Parse each geometry file to read vertex-bloc mapping_ids (which primitive
   in the exported GLB corresponds to which render set).
4. Convert DDS → PNG in memory using Pillow (reads .dd0 / .dds directly).
5. Embed PNG textures and PBR materials into the GLB JSON; assign material
   indices to primitives.

Texture channel suffixes
========================
_a  → albedo (baseColorTexture)          required
_n  → normal map (normalTexture)         optional
_mg → metallic/gloss  (metallicRoughnessTexture) optional

File extensions in priority order: .dd0, .dd1, .dds
"""

import io
import json
import os
import struct
import sys
from typing import Optional

try:
    from PIL import Image
except ImportError:
    Image = None  # type: ignore[assignment]

# ---------------------------------------------------------------------------
# Geometry mapping_id reader
# ---------------------------------------------------------------------------

def read_geometry_mapping_ids(geom_path: str) -> list[int]:
    """
    Read index-bloc mapping_ids from a .geometry file header.

    GLB primitives are produced one-per-index-bloc (section_2) in iteration
    order.  The mapping_id in section_2 matches RenderSet.indices_mapping_id,
    which is how we determine which material each primitive gets.
    """
    with open(geom_path, "rb") as f:
        hdr = f.read(72)

    if len(hdr) < 72:
        return []

    n_index_bloc  = struct.unpack_from("<I", hdr, 12)[0]   # offset 12
    off_sec_2     = struct.unpack_from("<Q", hdr, 32)[0]   # abs offset to index bloc table

    with open(geom_path, "rb") as f:
        f.seek(off_sec_2)
        table = f.read(n_index_bloc * 16)

    mapping_ids = []
    for i in range(n_index_bloc):
        mid = struct.unpack_from("<I", table, i * 16)[0]
        mapping_ids.append(mid)

    return mapping_ids


def read_geometry_tri_counts(geom_path: str) -> dict[int, int]:
    """Return {mapping_id: triangle_count} for every index bloc in the geometry file."""
    with open(geom_path, "rb") as f:
        hdr = f.read(72)
    if len(hdr) < 72:
        return {}
    n_index_bloc = struct.unpack_from("<I", hdr, 12)[0]
    off_sec_2    = struct.unpack_from("<Q", hdr, 32)[0]
    with open(geom_path, "rb") as f:
        f.seek(off_sec_2)
        table = f.read(n_index_bloc * 16)
    result: dict[int, int] = {}
    for i in range(n_index_bloc):
        mid        = struct.unpack_from("<I", table, i * 16)[0]
        items_cnt  = struct.unpack_from("<I", table, i * 16 + 12)[0]
        result[mid] = items_cnt // 3
    return result


def best_lod_for_visual(visual, damage_mids: set[int], tri_counts: dict[int, int]) -> int:
    """
    Return the LOD level whose non-damage geometry has the most triangles.

    LOD0 in WoWS is designed for extreme close-up damage viewing: it prioritises
    crack-seam and cross-section faces and may have minimal main-hull coverage.
    LOD1 is the standard combat LOD and typically carries the complete undamaged
    hull panels.  Selecting the LOD with the highest non-damage triangle count
    gives the most visually complete undamaged ship for static renders.
    """
    best_level = 0
    best_tris  = -1
    for lod_i, lod_mids in enumerate(visual.lods):
        tris = sum(tri_counts.get(m, 0) for m in lod_mids if m not in damage_mids)
        if tris > best_tris:
            best_tris  = tris
            best_level = lod_i
    return best_level


# ---------------------------------------------------------------------------
# Texture file finder
# ---------------------------------------------------------------------------

# MFM-only suffixes that never appear in texture filenames.
# e.g. "JGM178_460mm_45_Type94_skinned.mfm" → texture "JGM178_460mm_45_Type94_a.dd0"
_MFM_ONLY_SUFFIXES = ("_skinned", "_alpha", "_ep")


def _mfm_stem_candidates(stem: str) -> list[str]:
    """Return texture-stem candidates: full stem first, then with MFM-only suffix stripped."""
    candidates = [stem]
    for suffix in _MFM_ONLY_SUFFIXES:
        if stem.endswith(suffix):
            candidates.append(stem[: -len(suffix)])
            break
    return candidates


def find_texture(textures_dir: str, stem: str, channel: str) -> Optional[tuple[str, str]]:
    """
    Find a texture file for the given stem and channel suffix.
    Returns (path, effective_stem) or None.
    Tries stripping MFM-only suffixes (_skinned, _alpha, _ep) if the canonical stem has no file.
    Prefers .dd0 (highest resolution), falls back to .dd1, .dds.
    """
    for try_stem in _mfm_stem_candidates(stem):
        for ext in (".dd0", ".dd1", ".dds"):
            path = os.path.join(textures_dir, f"{try_stem}{channel}{ext}")
            if os.path.isfile(path):
                return path, try_stem
    return None


def dds_to_png_bytes(dds_path: str, max_size: Optional[int] = 2048) -> Optional[bytes]:
    """Convert a DDS/DD0 file to PNG bytes (RGBA, max_size×max_size)."""
    if Image is None:
        return None
    try:
        img = Image.open(dds_path).convert("RGBA")
        # Force alpha=255 — game albedo alpha stores non-opacity data
        r, g, b, _ = img.split()
        img = Image.merge("RGBA", (r, g, b, Image.new("L", img.size, 255)))

        if max_size and (img.width > max_size or img.height > max_size):
            scale = max_size / max(img.width, img.height)
            img = img.resize(
                (max(1, int(img.width * scale)), max(1, int(img.height * scale))),
                Image.LANCZOS,
            )

        buf = io.BytesIO()
        img.save(buf, format="PNG")
        return buf.getvalue()
    except Exception as e:
        print(f"  Warning: DDS conversion failed for {dds_path}: {e}", file=sys.stderr)
        return None


# ---------------------------------------------------------------------------
# Build material map: render-set vertices_mapping_id → (texture stem, png bytes)
# ---------------------------------------------------------------------------

def build_material_map(
    db,                        # PrototypeDatabase
    visual,                    # VisualPrototype
    game_dir: str,
) -> dict[int, dict]:
    """
    For each render set in the visual, resolve its MFM path → texture stem →
    texture files.  Returns:
      {indices_mapping_id: {"stem": str, "albedo_png": bytes|None, "dir": str}}

    Keyed on indices_mapping_id because GLB primitives are produced from index
    blocs (section_2), whose mapping_id matches RenderSet.indices_mapping_id.
    """
    id_idx = db.build_self_id_index()
    result: dict[int, dict] = {}

    for rs in visual.render_sets:
        # Resolve MFM path from path storage
        path_entry = id_idx.get(rs.mfm_path_id)
        if path_entry is None:
            continue
        mfm_full_path = db.reconstruct_path(path_entry, id_idx)
        if not mfm_full_path:
            continue

        # Derive stem and textures directory
        mfm_dir  = mfm_full_path.rsplit("/", 1)[0]
        mfm_file = mfm_full_path.rsplit("/", 1)[-1]
        stem     = mfm_file[:-4] if mfm_file.endswith(".mfm") else mfm_file

        textures_dir = os.path.join(game_dir, mfm_dir)

        # Find albedo texture; find_texture returns (path, effective_stem) or None.
        result_tex = find_texture(textures_dir, stem, "_a")
        if result_tex is not None:
            albedo_path, stem = result_tex  # update stem to the effective (stripped) one
            albedo_png = dds_to_png_bytes(albedo_path)
        else:
            albedo_path = None
            albedo_png  = None

        if albedo_png is None:
            print(f"  Texture: no albedo found for {stem}", file=sys.stderr)
        else:
            size_kb = len(albedo_png) // 1024
            print(f"  Texture: {os.path.basename(albedo_path)} → {size_kb} KB PNG", file=sys.stderr)

        # Damage materials: Razlom (torn metal) and grid/alpha overlays used at break points
        is_damage_mat = any(kw in stem for kw in ("Razlom", "C011_Grid"))

        result[rs.indices_mapping_id] = {
            "stem":        stem,
            "albedo_png":  albedo_png,
            "dir":         textures_dir,
            "is_damage":   is_damage_mat,
        }

    return result


# ---------------------------------------------------------------------------
# GLB texture/material patcher
# ---------------------------------------------------------------------------

def _pad4(n: int) -> int:
    return (4 - n % 4) % 4


def apply_textures_to_glb(
    json_dict: dict,
    binary: bytes,
    geom_path_to_mapping_ids: dict[str, list[int]],
    geom_path_to_mesh_indices: dict[str, list[int]],
    geom_path_to_material_map: dict[str, dict[int, dict]],
    geom_path_to_allowed: dict[str, set[int]],
) -> tuple[dict, bytes]:
    """
    Patch a merged GLB's JSON and binary to add textures and material assignments.

    geom_path_to_mapping_ids:  {geom_path: [mapping_id, ...]}   (primitive order in GLB mesh)
    geom_path_to_mesh_indices: {geom_path: [mesh_index, ...]}   (one path → N instances)
    geom_path_to_material_map: {geom_path: {mid: {"stem", "albedo_png", ...}}}
                               Per-geometry material maps — avoids mid collisions across models.
    geom_path_to_allowed:      {geom_path: set[mid]}            (LOD+damage filtered)
    """
    binary = bytearray(binary)

    images_list:   list[dict] = list(json_dict.get("images", []))
    textures_list: list[dict] = list(json_dict.get("textures", []))
    samplers_list: list[dict] = list(json_dict.get("samplers", []))
    materials_list: list[dict] = list(json_dict.get("materials", []))
    bv_list: list[dict] = list(json_dict.get("bufferViews", []))

    # Single linear sampler shared across all materials
    sampler_idx = len(samplers_list)
    samplers_list.append({
        "magFilter": 9729,   # LINEAR
        "minFilter": 9987,   # LINEAR_MIPMAP_LINEAR
        "wrapS":     10497,  # REPEAT
        "wrapT":     10497,
    })

    def _embed_png(png_bytes: bytes) -> int:
        """Append PNG to binary buffer, return texture index."""
        pad = _pad4(len(binary))
        binary.extend(b"\x00" * pad)
        bv_offset = len(binary)
        binary.extend(png_bytes)
        bv_idx = len(bv_list)
        bv_list.append({"buffer": 0, "byteOffset": bv_offset, "byteLength": len(png_bytes)})
        img_idx = len(images_list)
        images_list.append({"bufferView": bv_idx, "mimeType": "image/png"})
        tex_idx = len(textures_list)
        textures_list.append({"sampler": sampler_idx, "source": img_idx})
        return tex_idx

    _V_FLIP = {
        "extensions": {
            "KHR_texture_transform": {
                "scale":  [1.0, -1.0],
                "offset": [0.0,  1.0],
            }
        }
    }

    def _tex_info(tex_idx: int) -> dict:
        info = {"index": tex_idx, "texCoord": 0}
        info.update(_V_FLIP)
        return info

    # Shared stem → material index dedup across all geometry files.
    # Stems are globally unique names (e.g. "JSB039_Yamato_1945_Hull"), so this is safe.
    stem_to_mat_idx: dict[str, int] = {}

    def _ensure_material(stem: str, albedo_png: bytes) -> int:
        if stem not in stem_to_mat_idx:
            tex_idx = _embed_png(albedo_png)
            mat_idx = len(materials_list)
            materials_list.append({
                "name": stem,
                "pbrMetallicRoughness": {
                    "baseColorTexture": _tex_info(tex_idx),
                    "metallicFactor":   0.0,
                    "roughnessFactor":  0.8,
                },
                "doubleSided": True,
            })
            stem_to_mat_idx[stem] = mat_idx
        return stem_to_mat_idx[stem]

    # Process each geometry path independently.  The same path may appear multiple times
    # (same turret model reused for N instances) — apply materials to all mesh instances.
    meshes: list[dict] = json_dict.get("meshes", [])
    for geom_path, mesh_indices in geom_path_to_mesh_indices.items():
        material_map  = geom_path_to_material_map.get(geom_path, {})
        allowed_mids  = geom_path_to_allowed.get(geom_path)        # None = no filter
        mapping_ids   = geom_path_to_mapping_ids.get(geom_path, [])

        # Build mid → material index for this geometry's own material map.
        # (indices_mapping_ids are only unique within one geometry file's render sets.)
        mid_to_mat: dict[int, int] = {}
        for mid, info in material_map.items():
            stem       = info.get("stem", "")
            albedo_png = info.get("albedo_png")
            if albedo_png is not None and stem:
                mid_to_mat[mid] = _ensure_material(stem, albedo_png)

        for mesh_idx in mesh_indices:
            if mesh_idx >= len(meshes):
                continue
            mesh = meshes[mesh_idx]
            kept_prims = []
            for prim_idx, prim in enumerate(mesh.get("primitives", [])):
                if prim_idx < len(mapping_ids):
                    mid = mapping_ids[prim_idx]
                    if allowed_mids is not None and mid not in allowed_mids:
                        continue  # skip this primitive (wrong LOD or filtered damage)
                    mat_idx = mid_to_mat.get(mid)
                    if mat_idx is not None:
                        prim["material"] = mat_idx
                    kept_prims.append(prim)
                else:
                    kept_prims.append(prim)
            mesh["primitives"] = kept_prims

    out_json = dict(json_dict)
    out_json["bufferViews"] = bv_list
    out_json["images"]      = images_list
    out_json["textures"]    = textures_list
    out_json["samplers"]    = samplers_list
    out_json["materials"]   = materials_list
    out_json["buffers"]     = [{"byteLength": len(binary)}]

    exts_used = list(out_json.get("extensionsUsed", []))
    if "KHR_texture_transform" not in exts_used:
        exts_used.append("KHR_texture_transform")
    out_json["extensionsUsed"] = exts_used

    return out_json, bytes(binary)


# ---------------------------------------------------------------------------
# High-level: texture a merged hull GLB
# ---------------------------------------------------------------------------

def _geom_path_to_visual_suffix(geom_path: str) -> str:
    """Derive assets.bin visual path suffix from a .geometry file path."""
    visual = geom_path.replace(".geometry", ".visual")
    parts  = visual.replace("\\", "/").split("/")
    if len(parts) >= 2:
        return "/".join(parts[-2:])
    return parts[-1]


def texture_hull_glb(
    glb_json: dict,
    glb_binary: bytes,
    hull_geom_paths: list[str],
    assets_bin_path: str,
    hull_model_path: str,
    game_dir: str,
    max_texture_size: int = 2048,
    lod_level: int = -1,
    exclude_damage: bool = True,
) -> tuple[dict, bytes]:
    """
    Apply textures to a merged hull+turret GLB and filter to a single LOD level.

    hull_geom_paths: ordered list of geometry file paths (same order as meshes in the GLB).
    hull_model_path: unused (kept for API compatibility).
    lod_level: LOD to export.  -1 (default) = auto-select per visual the LOD with the
               most non-damage triangles; 0 = highest detail, 1/2/3 = lower.
               Note: WoWS LOD0 is designed for extreme close-up damage viewing and may
               have sparse main-hull geometry; LOD1 typically has the complete undamaged
               hull panels.  Auto-selection (-1) picks the best per section.
    exclude_damage: when True (default), remove damage/cross-section primitives (node
                    names containing '_crack_*_in' or '_hide').
    """
    if Image is None:
        print("  Warning: Pillow not installed — cannot apply textures.", file=sys.stderr)
        return glb_json, glb_binary

    sys.path.insert(0, os.path.dirname(__file__))
    from assets_bin import (
        parse_assets_bin, parse_visual, VISUAL_ITEM_SIZE, VISUAL_BLOB_INDEX,
    )

    auto_lod = lod_level < 0
    lod_label = "auto" if auto_lod else str(lod_level)
    damage_label = "excluding damage" if exclude_damage else "including damage"
    print(f"\nApplying textures from assets.bin (LOD {lod_label}, {damage_label}) …",
          file=sys.stderr)

    db = parse_assets_bin(assets_bin_path)

    # Per-geometry maps — keyed on geometry file path.
    # indices_mapping_ids are NOT globally unique across geometry files, so we must
    # never merge them into a single dict.
    geom_to_material_map: dict[str, dict[int, dict]] = {}
    geom_to_allowed:      dict[str, set[int]]        = {}
    geom_to_chosen_lod:   dict[str, int]             = {}

    for geom_path in hull_geom_paths:
        if not os.path.isfile(geom_path):
            continue
        if geom_path in geom_to_material_map:
            continue  # repeated instance — already processed
        suffix = _geom_path_to_visual_suffix(geom_path)
        loc = db.resolve_path(suffix)
        if loc is None:
            print(f"  Warning: visual not found: {suffix}", file=sys.stderr)
            continue
        blob_idx, rec_idx, full_path = loc
        if blob_idx != VISUAL_BLOB_INDEX:
            continue
        record_data = db.get_prototype_data(blob_idx, rec_idx, VISUAL_ITEM_SIZE)
        visual = parse_visual(record_data)

        # Auto-select the LOD with the most non-damage triangles for this visual.
        if auto_lod and visual.lod_count > 0:
            tri_counts = read_geometry_tri_counts(geom_path)
            damage_mids = set(visual.damage_indices_mapping_ids(db.strings))
            chosen_lod = best_lod_for_visual(visual, damage_mids, tri_counts)
        else:
            chosen_lod = max(0, lod_level)

        lod_info = f"LOD{chosen_lod}/{visual.lod_count}" if visual.lod_count else "no LODs"
        print(f"  Visual: {full_path} ({len(visual.render_sets)} render sets, {lod_info})",
              file=sys.stderr)

        mat_map = build_material_map(db, visual, game_dir)
        geom_to_material_map[geom_path] = mat_map

        allowed: set[int] = set(visual.lod_indices_mapping_ids(chosen_lod))
        if exclude_damage:
            damage: set[int] = set(visual.damage_indices_mapping_ids(db.strings))
            n_before = len(allowed)
            allowed -= damage
            print(f"  Damage filter: removed {n_before - len(allowed)} "
                  f"of {n_before} LOD{chosen_lod} primitives", file=sys.stderr)
        geom_to_allowed[geom_path]     = allowed
        geom_to_chosen_lod[geom_path]  = chosen_lod

    if not geom_to_material_map:
        print("  Warning: no textures resolved.", file=sys.stderr)
        return glb_json, glb_binary

    # Map geometry paths to their primitive ordering and all mesh indices in the GLB.
    # The same path may appear multiple times when a turret model is reused across instances.
    geom_to_mapping_ids: dict[str, list[int]] = {}
    geom_to_mesh_idxs:   dict[str, list[int]] = {}

    for mesh_idx, geom_path in enumerate(hull_geom_paths):
        if not os.path.isfile(geom_path):
            continue
        if geom_path not in geom_to_mapping_ids:
            mapping_ids = read_geometry_mapping_ids(geom_path)
            geom_to_mapping_ids[geom_path] = mapping_ids
            mat_map  = geom_to_material_map.get(geom_path, {})
            allowed  = geom_to_allowed.get(geom_path, set())
            kept     = sum(1 for mid in mapping_ids if mid in allowed)
            total    = len(mapping_ids)
            textured = sum(1 for mid in mapping_ids if mid in mat_map and mid in allowed)
            chosen   = geom_to_chosen_lod.get(geom_path, lod_level)
            print(f"  {os.path.basename(geom_path)}: {total} prims total, "
                  f"{kept} in LOD{chosen}, {textured} textured", file=sys.stderr)
        geom_to_mesh_idxs.setdefault(geom_path, []).append(mesh_idx)

    return apply_textures_to_glb(
        glb_json, glb_binary,
        geom_to_mapping_ids, geom_to_mesh_idxs,
        geom_to_material_map,
        geom_to_allowed,
    )
