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


# ---------------------------------------------------------------------------
# Texture file finder
# ---------------------------------------------------------------------------

def find_texture(textures_dir: str, stem: str, channel: str) -> Optional[str]:
    """
    Find a texture file for the given stem and channel suffix.
    Prefers .dd0 (highest resolution), falls back to .dd1, .dds.
    """
    for ext in (".dd0", ".dd1", ".dds"):
        path = os.path.join(textures_dir, f"{stem}{channel}{ext}")
        if os.path.isfile(path):
            return path
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

        # Find albedo texture
        albedo_path = find_texture(textures_dir, stem, "_a")
        albedo_png  = dds_to_png_bytes(albedo_path) if albedo_path else None

        if albedo_png is None:
            print(f"  Texture: no albedo found for {stem}", file=sys.stderr)
        else:
            size_kb = len(albedo_png) // 1024
            print(f"  Texture: {os.path.basename(albedo_path)} → {size_kb} KB PNG", file=sys.stderr)

        result[rs.indices_mapping_id] = {
            "stem":        stem,
            "albedo_png":  albedo_png,
            "dir":         textures_dir,
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
    geom_path_to_mesh_index: dict[str, int],
    material_map: dict[int, dict],
) -> tuple[dict, bytes]:
    """
    Patch a merged GLB's JSON and binary to add textures and material assignments.

    geom_path_to_mapping_ids: {geom_path: [mapping_id, ...]}  (primitive order)
    geom_path_to_mesh_index:  {geom_path: mesh_index_in_merged_json}
    material_map:             {vertices_mapping_id: {"stem", "albedo_png"}}
    """
    binary = bytearray(binary)

    # Collect unique (stem, albedo_png) pairs → deduplicate textures
    stem_to_tex_idx: dict[str, int] = {}
    images_list:  list[dict] = list(json_dict.get("images", []))
    textures_list: list[dict] = list(json_dict.get("textures", []))
    samplers_list: list[dict] = list(json_dict.get("samplers", []))
    materials_list: list[dict] = list(json_dict.get("materials", []))
    bv_list: list[dict] = list(json_dict.get("bufferViews", []))

    # Single linear sampler
    sampler_idx = len(samplers_list)
    samplers_list.append({
        "magFilter": 9729,   # LINEAR
        "minFilter": 9987,   # LINEAR_MIPMAP_LINEAR
        "wrapS":     10497,  # REPEAT
        "wrapT":     10497,
    })

    def _embed_png(png_bytes: bytes) -> int:
        """Append PNG to binary buffer, return texture index."""
        # Align binary to 4 bytes
        pad = _pad4(len(binary))
        binary.extend(b"\x00" * pad)
        bv_offset = len(binary)
        binary.extend(png_bytes)

        bv_idx = len(bv_list)
        bv_list.append({
            "buffer":     0,
            "byteOffset": bv_offset,
            "byteLength": len(png_bytes),
        })
        img_idx = len(images_list)
        images_list.append({"bufferView": bv_idx, "mimeType": "image/png"})
        tex_idx = len(textures_list)
        textures_list.append({"sampler": sampler_idx, "source": img_idx})
        return tex_idx

    # V-flip texture transform: BigWorld geometry stores V with origin at bottom,
    # but DDS textures are stored top-to-bottom. Applying scale=[1,-1], offset=[0,1]
    # corrects: new_v = -1*v + 1 = 1-v
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

    # Build stem → material index map (one material per unique stem)
    stem_to_mat_idx: dict[str, int] = {}
    for mid, info in material_map.items():
        stem = info["stem"]
        if stem in stem_to_mat_idx:
            continue
        albedo_png = info.get("albedo_png")
        if albedo_png is None:
            continue
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

    # mapping_id → material index
    mid_to_mat: dict[int, int] = {}
    for mid, info in material_map.items():
        stem = info["stem"]
        if stem in stem_to_mat_idx:
            mid_to_mat[mid] = stem_to_mat_idx[stem]

    # Assign materials to primitives in each mesh
    meshes: list[dict] = json_dict.get("meshes", [])
    for geom_path, mesh_idx in geom_path_to_mesh_index.items():
        if mesh_idx >= len(meshes):
            continue
        mapping_ids = geom_path_to_mapping_ids.get(geom_path, [])
        mesh = meshes[mesh_idx]
        for prim_idx, prim in enumerate(mesh.get("primitives", [])):
            if prim_idx < len(mapping_ids):
                mid = mapping_ids[prim_idx]
                mat_idx = mid_to_mat.get(mid)
                if mat_idx is not None:
                    prim["material"] = mat_idx

    # Declare the KHR_texture_transform extension as used
    out_json = dict(json_dict)
    exts_used = list(out_json.get("extensionsUsed", []))
    if "KHR_texture_transform" not in exts_used:
        exts_used.append("KHR_texture_transform")
    out_json["extensionsUsed"] = exts_used
    out_json["bufferViews"] = bv_list
    out_json["images"]      = images_list
    out_json["textures"]    = textures_list
    out_json["samplers"]    = samplers_list
    out_json["materials"]   = materials_list
    out_json["buffers"]     = [{"byteLength": len(binary)}]

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
) -> tuple[dict, bytes]:
    """
    Apply textures to a merged hull GLB.

    hull_geom_paths: ordered list of geometry file paths (same order as meshes
                     in the GLB — hull parts only, no turrets).
    hull_model_path: unused (kept for API compatibility).
    """
    if Image is None:
        print("  Warning: Pillow not installed — cannot apply textures.", file=sys.stderr)
        return glb_json, glb_binary

    sys.path.insert(0, os.path.dirname(__file__))
    from assets_bin import (
        parse_assets_bin, parse_visual, VISUAL_ITEM_SIZE, VISUAL_BLOB_INDEX,
    )

    print("\nApplying textures from assets.bin …", file=sys.stderr)

    db = parse_assets_bin(assets_bin_path)

    # Load one visual per geometry file; merge all material maps.
    # vertices_mapping_id values are globally unique hashes — no collisions.
    material_map: dict[int, dict] = {}
    for geom_path in hull_geom_paths:
        if not os.path.isfile(geom_path):
            continue
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
        print(f"  Visual: {full_path} ({len(visual.render_sets)} render sets)", file=sys.stderr)
        material_map.update(build_material_map(db, visual, game_dir))

    if not material_map:
        print("  Warning: no textures resolved.", file=sys.stderr)
        return glb_json, glb_binary

    # Map geometry paths to their primitive ordering and mesh index in the GLB
    geom_to_mapping_ids: dict[str, list[int]] = {}
    geom_to_mesh_idx:   dict[str, int]        = {}

    for mesh_idx, geom_path in enumerate(hull_geom_paths):
        if not os.path.isfile(geom_path):
            continue
        mapping_ids = read_geometry_mapping_ids(geom_path)
        geom_to_mapping_ids[geom_path] = mapping_ids
        geom_to_mesh_idx[geom_path]    = mesh_idx

        matched = sum(1 for mid in mapping_ids if mid in material_map)
        print(f"  {os.path.basename(geom_path)}: {len(mapping_ids)} prims, "
              f"{matched} textured", file=sys.stderr)

    return apply_textures_to_glb(
        glb_json, glb_binary,
        geom_to_mapping_ids, geom_to_mesh_idx,
        material_map,
    )
