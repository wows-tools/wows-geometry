#!/usr/bin/env python3
"""
Parse assets.bin (BigWorld PrototypeDatabase) and extract VisualPrototype data.

Format overview
===============
File header (16 bytes):
  u32 magic       = 0x42574442 ("BWDB")
  u32 version     = 0x01010000
  u32 checksum
  u16 architecture
  u16 endianness

Body at offset 0x10:
  Strings section  (base=0x10, 0x28 bytes of fields)
  r2p section      (base=0x38, 0x18 bytes of fields)
  pathsStorage     (base=0x50, 0x10 bytes of fields)
  databases        (fields at 0x60, relptr base=0x10)

Hashmaps use open addressing with linear probing.
  strings offsets_map: 8B buckets (u32 key + u32 sentinel), 4B values (u32 str_offset)
  r2p map:            16B buckets (u64 key + u64 sentinel), 4B values (u32 flat_index)

Flat r2p index encoding:
  blob_index  = (value & 0xFF) // 4
  record_index = value >> 8

VisualPrototype (blob index 1, item_size=0x70):
  Contains scene-graph node hierarchy with 4x4 column-major f32 matrices and
  HP_ hardpoint nodes. World-space transform = compose matrices up to root.
"""

import re
import struct
import sys
import os
import argparse
import json
from typing import Optional

BWDB_MAGIC   = 0x42574442
BWDB_VERSION = 0x01010000

VISUAL_ITEM_SIZE = 0x70
VISUAL_BLOB_INDEX = 1

# 0xFFFF = sentinel for "no parent" (root node)
NO_PARENT = 0xFFFF


# ---------------------------------------------------------------------------
# Low-level helpers
# ---------------------------------------------------------------------------

def _u32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]

def _u64(data: bytes, off: int) -> int:
    return struct.unpack_from("<Q", data, off)[0]

def _i64(data: bytes, off: int) -> int:
    return struct.unpack_from("<q", data, off)[0]

def _u16(data: bytes, off: int) -> int:
    return struct.unpack_from("<H", data, off)[0]

def _u8(data: bytes, off: int) -> int:
    return data[off]

def _f32_array(data: bytes, off: int, count: int) -> list[float]:
    return list(struct.unpack_from(f"<{count}f", data, off))

def _resolve(base: int, relptr: int) -> int:
    """Resolve a relative pointer: absolute = base + relptr."""
    return base + relptr

def _read_null_str(data: bytes, off: int) -> str:
    end = data.index(b"\x00", off)
    return data[off:end].decode("utf-8", errors="replace")


# ---------------------------------------------------------------------------
# Hashmap lookup
# ---------------------------------------------------------------------------

def _hashmap_lookup_u32key(
    data: bytes,
    capacity: int,
    buckets_off: int,
    values_off: int,
    key: int,
) -> Optional[int]:
    """
    Open-addressing lookup for the strings offsets_map.
    Bucket = 8 bytes: (u32 key, u32 sentinel). Value = u32.
    Empty when key==0 and sentinel==0.
    """
    if capacity == 0:
        return None
    start = key % capacity
    for probe in range(capacity):
        slot = (start + probe) % capacity
        boff = buckets_off + slot * 8
        bkey = _u32(data, boff)
        sent = _u32(data, boff + 4)
        if bkey == 0 and sent == 0:
            return None
        if bkey == key:
            return _u32(data, values_off + slot * 4)
    return None


def _hashmap_lookup_u64key(
    data: bytes,
    capacity: int,
    buckets_off: int,
    values_off: int,
    key: int,
) -> Optional[int]:
    """
    Open-addressing lookup for the r2p map.
    Bucket = 16 bytes: (u64 key, u64 sentinel). Value = u32.
    Empty when key==0 and sentinel==0.
    """
    if capacity == 0:
        return None
    start = key % capacity
    for probe in range(capacity):
        slot = (start + probe) % capacity
        boff = buckets_off + slot * 16
        bkey = _u64(data, boff)
        sent = _u64(data, boff + 8)
        if bkey == 0 and sent == 0:
            return None
        if bkey == key:
            return _u32(data, values_off + slot * 4)
    return None


# ---------------------------------------------------------------------------
# Strings section
# ---------------------------------------------------------------------------

class StringsSection:
    def __init__(
        self,
        data: bytes,
        capacity: int,
        buckets_off: int,
        values_off: int,
        string_data_off: int,
        string_data_size: int,
    ):
        self._data = data
        self._capacity = capacity
        self._buckets_off = buckets_off
        self._values_off = values_off
        self._str_off = string_data_off
        self._str_size = string_data_size

    def get_string_by_id(self, name_id: int) -> Optional[str]:
        """Look up a string by its hashed name ID."""
        str_offset = _hashmap_lookup_u32key(
            self._data, self._capacity,
            self._buckets_off, self._values_off,
            name_id,
        )
        if str_offset is None:
            return None
        abs_off = self._str_off + str_offset
        if abs_off >= self._str_off + self._str_size:
            return None
        return _read_null_str(self._data, abs_off)


# ---------------------------------------------------------------------------
# Path entries
# ---------------------------------------------------------------------------

class PathEntry:
    __slots__ = ("self_id", "parent_id", "name")

    def __init__(self, self_id: int, parent_id: int, name: str):
        self.self_id  = self_id
        self.parent_id = parent_id
        self.name      = name


def _parse_path_entries(data: bytes, data_off: int, count: int) -> list[PathEntry]:
    result = []
    for i in range(count):
        base = data_off + i * 32
        self_id   = _u64(data, base)
        parent_id = _u64(data, base + 8)

        # Packed string at base + 0x10
        name_base   = base + 0x10
        char_count  = _u32(data, name_base)
        # +4 pad, +8 relptr
        text_relptr = _i64(data, name_base + 8)

        if char_count > 0:
            text_off = _resolve(name_base, text_relptr)
            raw = data[text_off: text_off + char_count]
            name = raw.rstrip(b"\x00").decode("utf-8", errors="replace")
        else:
            name = ""

        result.append(PathEntry(self_id, parent_id, name))
    return result


# ---------------------------------------------------------------------------
# Database entries and blob access
# ---------------------------------------------------------------------------

class DatabaseEntry:
    __slots__ = ("prototype_magic", "prototype_checksum", "size", "data", "record_count")

    def __init__(self, magic: int, checksum: int, size: int, data: bytes, record_count: int):
        self.prototype_magic    = magic
        self.prototype_checksum = checksum
        self.size               = size
        self.data               = data
        self.record_count       = record_count


def _parse_database_entries(data: bytes, entries_off: int, count: int) -> list[DatabaseEntry]:
    result = []
    for i in range(count):
        base          = entries_off + i * 0x18
        proto_magic   = _u32(data, base)
        proto_chk     = _u32(data, base + 4)
        size          = _u32(data, base + 8)
        # +12 pad
        data_relptr   = _i64(data, base + 16)

        if size > 0:
            blob_off    = _resolve(base, data_relptr)
            blob_bytes  = data[blob_off: blob_off + size]
            record_count = _u64(blob_bytes, 0)
        else:
            blob_bytes   = b""
            record_count = 0

        result.append(DatabaseEntry(proto_magic, proto_chk, size, blob_bytes, record_count))
    return result


# ---------------------------------------------------------------------------
# PrototypeDatabase
# ---------------------------------------------------------------------------

class PrototypeDatabase:
    def __init__(
        self,
        data: bytes,
        strings: StringsSection,
        r2p_capacity: int,
        r2p_buckets_off: int,
        r2p_values_off: int,
        paths: list[PathEntry],
        databases: list[DatabaseEntry],
    ):
        self._data       = data
        self.strings     = strings
        self._r2p_cap    = r2p_capacity
        self._r2p_boff   = r2p_buckets_off
        self._r2p_voff   = r2p_values_off
        self.paths       = paths
        self.databases   = databases
        self._self_id_idx: Optional[dict[int, int]] = None

    def build_self_id_index(self) -> dict[int, int]:
        if self._self_id_idx is None:
            self._self_id_idx = {e.self_id: i for i, e in enumerate(self.paths)}
        return self._self_id_idx

    def lookup_r2p(self, self_id: int) -> Optional[int]:
        return _hashmap_lookup_u64key(
            self._data, self._r2p_cap,
            self._r2p_boff, self._r2p_voff,
            self_id,
        )

    def decode_r2p_value(self, value: int) -> tuple[int, int]:
        """Return (blob_index, record_index)."""
        type_tag     = value & 0xFF
        record_index = value >> 8
        blob_index   = type_tag // 4
        return blob_index, record_index

    def get_prototype_data(self, blob_index: int, record_index: int, item_size: int) -> bytes:
        """Return the blob slice starting at the record's offset (extends to blob end)."""
        db = self.databases[blob_index]
        header_size   = 16
        record_offset = header_size + record_index * item_size
        return db.data[record_offset:]

    def reconstruct_path(self, entry_index: int, id_idx: dict[int, int]) -> str:
        parts = []
        current = entry_index
        for _ in range(200):
            e = self.paths[current]
            if e.name:
                parts.append(e.name)
            if e.parent_id == 0:
                break
            parent = id_idx.get(e.parent_id)
            if parent is None:
                break
            current = parent
        parts.reverse()
        return "/".join(parts)

    def find_path_by_suffix(self, suffix: str) -> Optional[tuple[int, str]]:
        """Find a path entry whose reconstructed path ends with the given suffix."""
        id_idx   = self.build_self_id_index()
        suffix_leaf = suffix.rsplit("/", 1)[-1]
        for i, e in enumerate(self.paths):
            if not e.name.endswith(suffix_leaf):
                continue
            full = self.reconstruct_path(i, id_idx)
            if full.endswith(suffix):
                return i, full
        return None

    def resolve_path(self, path_suffix: str) -> Optional[tuple[int, int, str]]:
        """
        Find a path by suffix, look it up in r2p, decode.
        Returns (blob_index, record_index, full_path) or None.
        """
        hit = self.find_path_by_suffix(path_suffix)
        if hit is None:
            return None
        entry_index, full_path = hit
        self_id = self.paths[entry_index].self_id
        r2p_val = self.lookup_r2p(self_id)
        if r2p_val is None:
            return None
        blob_index, record_index = self.decode_r2p_value(r2p_val)
        return blob_index, record_index, full_path


# ---------------------------------------------------------------------------
# Top-level parser
# ---------------------------------------------------------------------------

def parse_assets_bin(path: str) -> PrototypeDatabase:
    with open(path, "rb") as f:
        data = f.read()

    magic   = _u32(data, 0)
    version = _u32(data, 4)
    if magic != BWDB_MAGIC:
        raise ValueError(f"Bad magic: 0x{magic:08X} (expected 0x{BWDB_MAGIC:08X})")
    if version != BWDB_VERSION:
        raise ValueError(f"Bad version: 0x{version:08X} (expected 0x{BWDB_VERSION:08X})")

    # Body at 0x10
    body_base = 0x10

    # Strings section base = body_base = 0x10
    strings_base = body_base
    offsets_map_cap     = _u32(data, strings_base)
    # +4 pad
    offsets_map_brelptr = _i64(data, strings_base + 8)
    offsets_map_vrelptr = _i64(data, strings_base + 16)
    string_data_size    = _u32(data, strings_base + 24)
    # +28 pad
    string_data_relptr  = _i64(data, strings_base + 32)

    offsets_map_boff = _resolve(strings_base, offsets_map_brelptr)
    offsets_map_voff = _resolve(strings_base, offsets_map_vrelptr)
    string_data_off  = _resolve(strings_base, string_data_relptr)

    strings = StringsSection(
        data,
        offsets_map_cap,
        offsets_map_boff,
        offsets_map_voff,
        string_data_off,
        string_data_size,
    )

    # r2p section base = body_base + 0x28 = 0x38
    r2p_base = body_base + 0x28
    r2p_cap      = _u32(data, r2p_base)
    # +4 pad
    r2p_brelptr  = _i64(data, r2p_base + 8)
    r2p_vrelptr  = _i64(data, r2p_base + 16)

    r2p_boff = _resolve(r2p_base, r2p_brelptr)
    r2p_voff = _resolve(r2p_base, r2p_vrelptr)

    # pathsStorage base = body_base + 0x40 = 0x50
    paths_base = body_base + 0x40
    paths_count    = _u32(data, paths_base)
    # +4 pad
    paths_relptr   = _i64(data, paths_base + 8)
    paths_data_off = _resolve(paths_base, paths_relptr)
    paths = _parse_path_entries(data, paths_data_off, paths_count)

    # databases: count/relptr at body_base + 0x50, relptr base = body_base
    db_field_off    = body_base + 0x50
    db_count        = _u32(data, db_field_off)
    # +4 pad
    db_relptr       = _i64(data, db_field_off + 8)
    db_entries_off  = _resolve(body_base, db_relptr)
    databases = _parse_database_entries(data, db_entries_off, db_count)

    return PrototypeDatabase(data, strings, r2p_cap, r2p_boff, r2p_voff, paths, databases)


# ---------------------------------------------------------------------------
# VisualPrototype: render set
# ---------------------------------------------------------------------------

RENDER_SET_SIZE = 0x28

class RenderSet:
    __slots__ = ("name_id", "mat_name_id", "vertices_mapping_id", "indices_mapping_id",
                 "mfm_path_id", "node_name_id")

    def __init__(
        self,
        name_id: int,
        mat_name_id: int,
        vertices_mapping_id: int,
        indices_mapping_id: int,
        mfm_path_id: int,
        node_name_id: int = 0,
    ):
        self.name_id              = name_id
        self.mat_name_id          = mat_name_id
        self.vertices_mapping_id  = vertices_mapping_id
        self.indices_mapping_id   = indices_mapping_id
        self.mfm_path_id          = mfm_path_id
        self.node_name_id         = node_name_id


def _parse_render_sets(record_data: bytes, offset: int, count: int) -> list[RenderSet]:
    result = []
    for i in range(count):
        base = offset + i * RENDER_SET_SIZE
        name_id              = _u32(record_data, base)
        mat_name_id          = _u32(record_data, base + 4)
        vertices_mapping_id  = _u32(record_data, base + 8)
        indices_mapping_id   = _u32(record_data, base + 12)
        mfm_path_id          = _u64(record_data, base + 16)
        # +24 skinned u8, +25 nodes_count u8, +26..31 padding, +32 node_name_ids_relptr i64
        nodes_cnt   = record_data[base + 25]
        node_rp     = _i64(record_data, base + 32)
        node_name_id = 0
        if nodes_cnt > 0 and node_rp != 0:
            node_arr_off = (base + 32) + node_rp
            if node_arr_off + 4 <= len(record_data):
                node_name_id = _u32(record_data, node_arr_off)
        result.append(RenderSet(name_id, mat_name_id, vertices_mapping_id, indices_mapping_id,
                                mfm_path_id, node_name_id))
    return result


# ---------------------------------------------------------------------------
# VisualPrototype: nodes
# ---------------------------------------------------------------------------

class VisualNodes:
    __slots__ = ("name_map_name_ids", "name_map_node_ids", "name_ids", "matrices", "parent_ids")

    def __init__(
        self,
        name_map_name_ids: list[int],
        name_map_node_ids: list[int],
        name_ids: list[int],
        matrices: list[list[float]],
        parent_ids: list[int],
    ):
        self.name_map_name_ids = name_map_name_ids
        self.name_map_node_ids = name_map_node_ids
        self.name_ids          = name_ids
        self.matrices          = matrices
        self.parent_ids        = parent_ids


class VisualPrototype:
    def __init__(self, nodes: VisualNodes, merged_geometry_path_id: int,
                 render_sets: list | None = None,
                 lods: list[list[int]] | None = None):
        self.nodes                   = nodes
        self.merged_geometry_path_id = merged_geometry_path_id
        self.render_sets: list[RenderSet] = render_sets or []
        # lods[i] = list of indices_mapping_ids for LOD level i (0=highest detail)
        self.lods: list[list[int]] = lods or []

    def lod_indices_mapping_ids(self, lod_level: int) -> set[int]:
        """Return the set of indices_mapping_ids that belong to the given LOD level."""
        if not self.lods or lod_level >= len(self.lods):
            # Fall back: return all render set indices_mapping_ids
            return {rs.indices_mapping_id for rs in self.render_sets}
        return set(self.lods[lod_level])

    @property
    def lod_count(self) -> int:
        return len(self.lods)

    def find_node_index_by_name(self, name: str, strings: StringsSection) -> Optional[int]:
        for i, name_id in enumerate(self.nodes.name_map_name_ids):
            resolved = strings.get_string_by_id(name_id)
            if resolved == name:
                return self.nodes.name_map_node_ids[i]
        return None

    def find_hardpoint_transform(self, hp_name: str, strings: StringsSection) -> Optional[list[float]]:
        """Return world-space 4x4 column-major matrix (16 floats) for the named HP_ node."""
        node_idx = self.find_node_index_by_name(hp_name, strings)
        if node_idx is None:
            return None

        result  = list(self.nodes.matrices[node_idx])
        current = node_idx
        while True:
            parent = self.nodes.parent_ids[current]
            if parent == NO_PARENT or parent >= len(self.nodes.matrices):
                break
            result  = _mat4_mul(self.nodes.matrices[parent], result)
            current = parent

        return result

    def list_hp_nodes(self, strings: StringsSection) -> list[str]:
        """Return sorted list of node names that start with 'HP_'."""
        result = []
        for name_id in self.nodes.name_ids:
            name = strings.get_string_by_id(name_id)
            if name and name.startswith("HP_"):
                result.append(name)
        return sorted(result)

    def damage_indices_mapping_ids(self, strings: StringsSection) -> set[int]:
        """Return mapping_ids for render sets that are damage/cross-section geometry.

        Node naming convention in BigWorld WoWS:
          Xxx_crack_YYY            → exterior hull face at joint (NOT damage)
          Xxx_crack_YYY_DeckHouse  → exterior deckhouse face at joint (NOT damage)
          Xxx_crack_YYY_Bulge      → exterior bulge plating at joint (NOT damage)
          Xxx_crack_YYY_wire       → rigging at joint (NOT damage)
          Xxx_crack_YYY_in         → inner cross-section face (DAMAGE)
          Xxx_crack_YYY_in1        → inner cross-section variant (DAMAGE)
          Xxx_crack_YYY_in_*       → inner cross-section sub-element (DAMAGE)
          Xxx_crack_YYY_*_in       → inner face with suffix (DAMAGE)
          Xxx_crack_YYY_inside     → inner face alternate spelling (DAMAGE)
          Xxx_hide                 → hidden torn-metal mesh (DAMAGE)
        """
        result: set[int] = set()
        for rs in self.render_sets:
            name = strings.get_string_by_id(rs.node_name_id) or ""
            if "_hide" in name:
                result.add(rs.indices_mapping_id)
            elif "_crack_" in name:
                # Damage interior faces always contain _in as a suffix component
                # (_in, _in1, _in_*, _*_in) or _inside. Exterior faces do not.
                if re.search(r"_in(?:\d|_|$)|_inside(?:_|$)", name):
                    result.add(rs.indices_mapping_id)
        return result


def _mat4_mul(a: list[float], b: list[float]) -> list[float]:
    """Multiply two 4x4 column-major matrices."""
    out = [0.0] * 16
    for col in range(4):
        for row in range(4):
            s = 0.0
            for k in range(4):
                s += a[k * 4 + row] * b[col * 4 + k]
            out[col * 4 + row] = s
    return out


def parse_visual(record_data: bytes) -> VisualPrototype:
    """Parse a VisualPrototype from blob record data (slice from record start to blob end)."""
    if len(record_data) < VISUAL_ITEM_SIZE:
        raise ValueError(f"Record too short: {len(record_data)} < {VISUAL_ITEM_SIZE}")

    nodes_count = _u32(record_data, 0)
    # +4 pad
    name_map_name_ids_rp = _i64(record_data, 8)
    name_map_node_ids_rp = _i64(record_data, 16)
    name_ids_rp          = _i64(record_data, 24)
    matrices_rp          = _i64(record_data, 32)
    parent_ids_rp        = _i64(record_data, 40)
    merged_geom_path_id  = _u64(record_data, 48)
    # +56: underwater_model u8, abovewater_model u8, render_sets_count u16, lods_count u8, 3×pad
    render_sets_count = _u16(record_data, 58)
    lods_count        = record_data[60]
    # BoundingBox at +64 (32 bytes)
    # render_sets_relptr at +96 (i64), lods_relptr at +104 (i64)
    render_sets_rp = _i64(record_data, 96)
    lods_rp        = _i64(record_data, 104)

    base = 0

    if nodes_count > 0:
        name_map_name_ids = list(struct.unpack_from(
            f"<{nodes_count}I", record_data,
            _resolve(base, name_map_name_ids_rp),
        ))
        name_map_node_ids = list(struct.unpack_from(
            f"<{nodes_count}H", record_data,
            _resolve(base, name_map_node_ids_rp),
        ))
        name_ids = list(struct.unpack_from(
            f"<{nodes_count}I", record_data,
            _resolve(base, name_ids_rp),
        ))
        mat_off = _resolve(base, matrices_rp)
        matrices = [
            list(struct.unpack_from("<16f", record_data, mat_off + i * 64))
            for i in range(nodes_count)
        ]
        parent_ids = list(struct.unpack_from(
            f"<{nodes_count}H", record_data,
            _resolve(base, parent_ids_rp),
        ))
    else:
        name_map_name_ids = []
        name_map_node_ids = []
        name_ids          = []
        matrices          = []
        parent_ids        = []

    nodes = VisualNodes(name_map_name_ids, name_map_node_ids, name_ids, matrices, parent_ids)

    render_sets: list[RenderSet] = []
    if render_sets_count > 0:
        rs_off = _resolve(base, render_sets_rp)
        render_sets = _parse_render_sets(record_data, rs_off, render_sets_count)

    # Build name_id → render_set lookup for LOD parsing
    name_to_rs = {rs.name_id: rs for rs in render_sets}

    # LOD table: each entry is 16 bytes
    #   [0..3]  float dist_threshold
    #   [4..5]  u16 unknown
    #   [6..7]  u16 rs_count  (number of render set name IDs in this LOD)
    #   [8..15] i64 rs_names_relptr (relative from THIS entry's base offset)
    lods: list[list[int]] = []
    if lods_count > 0:
        lods_off = _resolve(base, lods_rp)
        for i in range(lods_count):
            entry_base = lods_off + i * 16
            rs_cnt     = _u16(record_data, entry_base + 6)
            rp         = _i64(record_data, entry_base + 8)
            names_off  = entry_base + rp
            name_ids   = struct.unpack_from(f"<{rs_cnt}I", record_data, names_off)
            # Resolve name IDs → indices_mapping_ids
            mid_list = [
                name_to_rs[nid].indices_mapping_id
                for nid in name_ids
                if nid in name_to_rs
            ]
            lods.append(mid_list)

    return VisualPrototype(nodes, merged_geom_path_id, render_sets, lods)


# ---------------------------------------------------------------------------
# High-level API used by stitch_ship.py
# ---------------------------------------------------------------------------

def get_hp_transforms(
    assets_bin_path: str,
    visual_path_suffix: str,
) -> dict[str, list[float]]:
    """
    Parse assets.bin and return {hp_name: [16 floats column-major]} for all
    HP_ nodes in the given visual file path.

    visual_path_suffix: a path suffix like
      "JSB007_Kongo_1942/JSB007_Kongo_1942.visual"
    that will be matched against the paths stored in assets.bin.

    Returns an empty dict if the visual cannot be found or has no HP_ nodes.
    """
    db = parse_assets_bin(assets_bin_path)

    loc = db.resolve_path(visual_path_suffix)
    if loc is None:
        return {}

    blob_index, record_index, _full = loc
    if blob_index != VISUAL_BLOB_INDEX:
        return {}

    record_data = db.get_prototype_data(blob_index, record_index, VISUAL_ITEM_SIZE)
    visual      = parse_visual(record_data)

    result: dict[str, list[float]] = {}
    for name_id in visual.nodes.name_ids:
        name = db.strings.get_string_by_id(name_id)
        if name and name.startswith("HP_"):
            transform = visual.find_hardpoint_transform(name, db.strings)
            if transform is not None:
                result[name] = transform

    return result


def model_path_to_visual_suffix(hull_model_path: str) -> str:
    """
    Convert a hull model path to the visual path suffix used in assets.bin.

    E.g.  "content/gameplay/japan/ship/battleship/JSB007_Kongo_1942/JSB007_Kongo_1942.model"
    →     "JSB007_Kongo_1942/JSB007_Kongo_1942.visual"
    """
    visual = hull_model_path.replace(".model", ".visual")
    parts  = visual.replace("\\", "/").split("/")
    # Keep last two path components: <dir>/<file>.visual
    if len(parts) >= 2:
        return "/".join(parts[-2:])
    return parts[-1]


_IDENTITY_16: list[float] = [
    1.0, 0.0, 0.0, 0.0,
    0.0, 1.0, 0.0, 0.0,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
]


def _mat4_rotation_inverse(m: list[float]) -> list[float]:
    """Inverse of a 4×4 column-major pure-rotation (or reflection) matrix.

    For orthogonal matrices the inverse equals the transpose of the 3×3 block.
    Any translation in column 3 is discarded — blend-bone nodes are rotations only.
    """
    # column-major: element (row r, col c) lives at index c*4+r
    # Transpose: result[c*4+r] = m[r*4+c]
    return [
        m[0],  m[4],  m[8],  0.0,
        m[1],  m[5],  m[9],  0.0,
        m[2],  m[6],  m[10], 0.0,
        0.0,   0.0,   0.0,   1.0,
    ]


def get_blendbone_corrections(
    assets_bin_path: str,
    model_paths: list[str],
) -> dict[str, list[float]]:
    """Return per-model blend-bone correction matrices.

    BigWorld .geometry files store vertices in *bind pose* — before the
    BlendBone rest-pose transform is applied.  To place a turret model at its
    HP_ hardpoint with the correct facing direction we must undo that rest-pose
    rotation: correction = inverse(Rotate_Y_BlendBone_local_rotation).

    For models whose Rotate_Y_BlendBone is a Z-flip ([[1,0,0],[0,1,0],[0,0,-1]])
    the correction is the Z-flip itself (self-inverse), which rotates bind-pose
    barrels from -Z to +Z.  For models with an identity BlendBone the correction
    is identity (no change needed).

    Returns {model_path: [16 floats column-major]} for every path in model_paths.
    Paths whose visual cannot be found in assets.bin default to identity.
    """
    db = parse_assets_bin(assets_bin_path)
    result: dict[str, list[float]] = {}

    for model_path in model_paths:
        visual_suffix = model_path_to_visual_suffix(model_path)
        loc = db.resolve_path(visual_suffix)
        if loc is None:
            result[model_path] = list(_IDENTITY_16)
            continue

        blob_index, record_index, _full = loc
        if blob_index != VISUAL_BLOB_INDEX:
            result[model_path] = list(_IDENTITY_16)
            continue

        record_data = db.get_prototype_data(blob_index, record_index, VISUAL_ITEM_SIZE)
        visual = parse_visual(record_data)

        # Prefer Rotate_Y_BlendBone (encodes yaw rest pose); fall back to Root_BlendBone.
        node_idx = visual.find_node_index_by_name("Rotate_Y_BlendBone", db.strings)
        if node_idx is None:
            node_idx = visual.find_node_index_by_name("Root_BlendBone", db.strings)

        if node_idx is None or node_idx >= len(visual.nodes.matrices):
            result[model_path] = list(_IDENTITY_16)
            continue

        local_mat = visual.nodes.matrices[node_idx]
        result[model_path] = _mat4_rotation_inverse(local_mat)

    return result


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(
        description="Parse assets.bin and extract VisualPrototype HP_ transforms.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  List all paths containing a keyword:
    %(prog)s assets.bin --list-paths Kongo

  Dump HP_ transforms for a visual file:
    %(prog)s assets.bin --visual JSB007_Kongo_1942/JSB007_Kongo_1942.visual

  Get HP_ transforms in JSON:
    %(prog)s assets.bin --visual JSB007_Kongo_1942/JSB007_Kongo_1942.visual -o hp.json
""",
    )
    ap.add_argument("assets_bin", metavar="assets.bin", help="Path to assets.bin")
    ap.add_argument("--list-paths", metavar="KEYWORD",
                    help="List all .visual paths containing KEYWORD")
    ap.add_argument("--visual", metavar="SUFFIX",
                    help="Resolve visual path suffix and dump its HP_ node transforms")
    ap.add_argument("-o", "--output", metavar="FILE",
                    help="Write JSON output to FILE (default: stdout)")
    args = ap.parse_args()

    print("Parsing assets.bin …", file=sys.stderr)
    db = parse_assets_bin(args.assets_bin)
    id_idx = db.build_self_id_index()
    print(f"  {len(db.paths)} path entries, {len(db.databases)} databases.", file=sys.stderr)

    if args.list_paths:
        needle = args.list_paths.lower()
        hits = []
        for i, e in enumerate(db.paths):
            if not e.name.endswith(".visual"):
                continue
            full = db.reconstruct_path(i, id_idx)
            if needle in full.lower():
                hits.append(full)
        out = json.dumps(sorted(hits), indent=2)

    elif args.visual:
        transforms = get_hp_transforms(args.assets_bin, args.visual)
        if not transforms:
            print(f"No HP_ transforms found for '{args.visual}'.", file=sys.stderr)
            sys.exit(1)
        print(f"Found {len(transforms)} HP_ nodes.", file=sys.stderr)
        out = json.dumps({k: v for k, v in sorted(transforms.items())}, indent=2)

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
