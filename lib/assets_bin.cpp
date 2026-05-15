/* C++ port of scripts/assets_bin.py — BigWorld PrototypeDatabase parser.
 * Provides HP_ hardpoint transforms and BlendBone correction matrices.
 */
#include "wows-assets-bin.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

bool wows_assets_bin_verbose = false;
#define vlog(tag, fmt, ...) do { if (wows_assets_bin_verbose) fprintf(stderr, "[%s] " fmt, tag, ##__VA_ARGS__); } while (0)

/* ── raw read helpers ─────────────────────────────────────────────── */
static inline uint32_t ru32(const uint8_t *d, size_t o) {
    uint32_t v;
    memcpy(&v, d + o, 4);
    return v;
}
static inline uint64_t ru64(const uint8_t *d, size_t o) {
    uint64_t v;
    memcpy(&v, d + o, 8);
    return v;
}
static inline int64_t ri64(const uint8_t *d, size_t o) {
    int64_t v;
    memcpy(&v, d + o, 8);
    return v;
}
static inline uint16_t ru16(const uint8_t *d, size_t o) {
    uint16_t v;
    memcpy(&v, d + o, 2);
    return v;
}
static inline float rf32(const uint8_t *d, size_t o) {
    float v;
    memcpy(&v, d + o, 4);
    return v;
}

static std::string read_cstr(const uint8_t *d, size_t dlen, size_t off) {
    if (off >= dlen)
        return "";
    size_t end = off;
    while (end < dlen && d[end])
        ++end;
    return {reinterpret_cast<const char *>(d + off), end - off};
}

/* ── open-addressing hashmap lookups ──────────────────────────────── */

/* 8-byte buckets: u32 key + u32 sentinel; u32 values */
static bool hmap32(const uint8_t *d, size_t dlen, uint32_t cap, size_t boff, size_t voff, uint32_t key, uint32_t *out) {
    if (!cap)
        return false;
    for (uint32_t p = 0; p < cap; ++p) {
        uint32_t slot = (key + p) % cap;
        size_t b = boff + slot * 8;
        if (b + 8 > dlen)
            return false;
        uint32_t bk = ru32(d, b), bs = ru32(d, b + 4);
        if (!bk && !bs)
            return false;
        if (bk == key) {
            *out = ru32(d, voff + slot * 4);
            return true;
        }
    }
    return false;
}

/* 16-byte buckets: u64 key + u64 sentinel; u32 values */
static bool hmap64(const uint8_t *d, size_t dlen, uint32_t cap, size_t boff, size_t voff, uint64_t key, uint32_t *out) {
    if (!cap)
        return false;
    uint64_t start = key % cap;
    for (uint64_t p = 0; p < cap; ++p) {
        uint64_t slot = (start + p) % cap;
        size_t b = boff + slot * 16;
        if (b + 16 > dlen)
            return false;
        uint64_t bk = ru64(d, b), bs = ru64(d, b + 8);
        if (!bk && !bs)
            return false;
        if (bk == key) {
            *out = ru32(d, voff + slot * 4);
            return true;
        }
    }
    return false;
}

/* ── PrototypeDatabase structures ─────────────────────────────────── */

struct StringSec {
    uint32_t cap;
    size_t boff, voff, str_off, str_size;

    std::string get(const uint8_t *d, size_t dlen, uint32_t id) const {
        uint32_t soff = 0;
        if (!hmap32(d, dlen, cap, boff, voff, id, &soff))
            return "";
        size_t abs = str_off + soff;
        if (abs >= str_off + str_size || abs >= dlen)
            return "";
        return read_cstr(d, dlen, abs);
    }
};

struct PathEntry {
    uint64_t self_id, parent_id;
    std::string name;
};

struct DbBlob {
    size_t data_off; /* absolute offset in raw file */
    uint32_t size;
    uint64_t record_count;
};

#define BWDB_MAGIC 0x42574442u
#define BWDB_VERSION 0x01010000u
#define VISUAL_BLOB 1u
#define VISUAL_ISIZE 0x70u
#define NO_PARENT 0xFFFFu

struct PDB {
    std::vector<uint8_t> raw;
    StringSec str;
    uint32_t r2p_cap;
    size_t r2p_boff, r2p_voff;
    std::vector<PathEntry> paths;
    std::vector<DbBlob> blobs;
    std::unordered_map<uint64_t, size_t> sid_idx;

    const uint8_t *d() const {
        return raw.data();
    }
    size_t len() const {
        return raw.size();
    }

    bool r2p_lookup(uint64_t sid, uint32_t *out) const {
        return hmap64(d(), len(), r2p_cap, r2p_boff, r2p_voff, sid, out);
    }
    void decode_r2p(uint32_t v, int *bi, int *ri) const {
        *bi = (int)((v & 0xFF) / 4);
        *ri = (int)(v >> 8);
    }

    /* absolute file offset of record, or 0 on error */
    size_t record_abs(int bi, int ri, size_t isize) const {
        if (bi < 0 || (size_t)bi >= blobs.size())
            return 0;
        const DbBlob &b = blobs[bi];
        size_t off = b.data_off + 16 + (size_t)ri * isize;
        if (off + isize > b.data_off + b.size || off >= raw.size())
            return 0;
        return off;
    }

    std::string reconstruct_path(size_t idx) const {
        std::vector<std::string> parts;
        size_t cur = idx;
        for (int i = 0; i < 200; ++i) {
            const PathEntry &e = paths[cur];
            if (!e.name.empty())
                parts.push_back(e.name);
            if (!e.parent_id)
                break;
            auto it = sid_idx.find(e.parent_id);
            if (it == sid_idx.end())
                break;
            cur = it->second;
        }
        std::reverse(parts.begin(), parts.end());
        std::string r;
        for (size_t i = 0; i < parts.size(); ++i) {
            if (i)
                r += '/';
            r += parts[i];
        }
        return r;
    }

    bool find_by_suffix(const std::string &suffix, size_t *out_idx, std::string *full) const {
        std::string leaf = suffix;
        auto sl = suffix.rfind('/');
        if (sl != std::string::npos)
            leaf = suffix.substr(sl + 1);
        for (size_t i = 0; i < paths.size(); ++i) {
            const std::string &n = paths[i].name;
            if (n.size() < leaf.size())
                continue;
            if (n.compare(n.size() - leaf.size(), leaf.size(), leaf) != 0)
                continue;
            std::string fp = reconstruct_path(i);
            if (fp.size() >= suffix.size() && fp.compare(fp.size() - suffix.size(), suffix.size(), suffix) == 0) {
                *out_idx = i;
                *full = fp;
                return true;
            }
        }
        return false;
    }

    /* Returns (blob_idx, rec_idx) or false */
    bool resolve(const std::string &suffix, int *bi, int *ri) const {
        size_t ei = 0;
        std::string fp;
        if (!find_by_suffix(suffix, &ei, &fp))
            return false;
        uint32_t v = 0;
        if (!r2p_lookup(paths[ei].self_id, &v))
            return false;
        decode_r2p(v, bi, ri);
        return true;
    }
};

static bool parse_pdb(PDB &pdb) {
    const uint8_t *d = pdb.d();
    size_t len = pdb.len();
    if (len < 0x70)
        return false;
    if (ru32(d, 0) != BWDB_MAGIC || ru32(d, 4) != BWDB_VERSION)
        return false;

    constexpr size_t body = 0x10;

    /* strings @ body */
    pdb.str.cap = ru32(d, body);
    pdb.str.boff = body + (size_t)ri64(d, body + 8);
    pdb.str.voff = body + (size_t)ri64(d, body + 16);
    pdb.str.str_size = ru32(d, body + 24);
    pdb.str.str_off = body + (size_t)ri64(d, body + 32);

    /* r2p @ body+0x28 */
    constexpr size_t r2p_base = body + 0x28;
    pdb.r2p_cap = ru32(d, r2p_base);
    pdb.r2p_boff = r2p_base + (size_t)ri64(d, r2p_base + 8);
    pdb.r2p_voff = r2p_base + (size_t)ri64(d, r2p_base + 16);

    /* paths @ body+0x40 */
    constexpr size_t paths_base = body + 0x40;
    uint32_t pcount = ru32(d, paths_base);
    size_t pdata = paths_base + (size_t)ri64(d, paths_base + 8);
    pdb.paths.reserve(pcount);
    for (uint32_t i = 0; i < pcount; ++i) {
        size_t base = pdata + i * 32;
        if (base + 32 > len)
            break;
        PathEntry e;
        e.self_id = ru64(d, base);
        e.parent_id = ru64(d, base + 8);
        size_t nb = base + 0x10;
        uint32_t cc = ru32(d, nb);
        if (cc > 0) {
            size_t toff = (size_t)((int64_t)nb + ri64(d, nb + 8));
            if (toff + cc <= len)
                e.name = std::string(reinterpret_cast<const char *>(d + toff), cc);
        }
        /* strip embedded nulls */
        e.name.erase(std::remove(e.name.begin(), e.name.end(), '\0'), e.name.end());
        pdb.paths.push_back(std::move(e));
    }
    for (size_t i = 0; i < pdb.paths.size(); ++i)
        pdb.sid_idx[pdb.paths[i].self_id] = i;

    /* databases @ body+0x50 (relptr base = body) */
    constexpr size_t db_field = body + 0x50;
    uint32_t db_count = ru32(d, db_field);
    size_t db_eoff = body + (size_t)ri64(d, db_field + 8);
    pdb.blobs.reserve(db_count);
    for (uint32_t i = 0; i < db_count; ++i) {
        size_t base = db_eoff + i * 0x18;
        if (base + 0x18 > len)
            break;
        DbBlob b;
        b.size = ru32(d, base + 8);
        b.data_off = (size_t)((int64_t)base + ri64(d, base + 16));
        b.record_count = (b.size >= 8 && b.data_off + 8 <= len) ? ru64(d, b.data_off) : 0;
        pdb.blobs.push_back(b);
    }
    return true;
}

static bool load_pdb(const char *path, PDB &pdb) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return false;
    }
    pdb.raw.resize((size_t)sz);
    bool ok = (long)fread(pdb.raw.data(), 1, (size_t)sz, f) == sz;
    fclose(f);
    return ok && parse_pdb(pdb);
}

static bool load_pdb_from_bytes(const uint8_t *data, size_t size, PDB &pdb) {
    if (!data || !size)
        return false;
    pdb.raw.assign(data, data + size);
    return parse_pdb(pdb);
}

/* ── VisualPrototype node parsing ─────────────────────────────────── */

struct VisNodes {
    std::vector<uint32_t> nm_name_ids; /* name_map key: name hash */
    std::vector<uint16_t> nm_node_ids; /* name_map value: node index */
    std::vector<uint32_t> name_ids;    /* node → name hash */
    std::vector<std::array<float, 16>> mats;
    std::vector<uint16_t> parents;
};

static bool parse_vis_nodes(const PDB &pdb, size_t rd_off, VisNodes &vn) {
    const uint8_t *d = pdb.d();
    size_t dlen = pdb.len();
    if (rd_off + VISUAL_ISIZE > dlen)
        return false;

    /* relative pointer resolver: value at (rd_off + field_off), base = rd_off */
    auto rel = [&](size_t field_off) -> size_t { return (size_t)((int64_t)rd_off + ri64(d, rd_off + field_off)); };

    uint32_t nc = ru32(d, rd_off);
    if (!nc)
        return true;

    size_t nm_ni_off = rel(8);
    size_t nm_nd_off = rel(16);
    size_t ni_off = rel(24);
    size_t mat_off = rel(32);
    size_t par_off = rel(40);

    vn.nm_name_ids.resize(nc);
    vn.nm_node_ids.resize(nc);
    vn.name_ids.resize(nc);
    vn.parents.resize(nc);
    vn.mats.resize(nc);

    for (uint32_t i = 0; i < nc; ++i) {
        size_t o4 = nm_ni_off + i * 4;
        size_t o2 = nm_nd_off + i * 2;
        size_t n4 = ni_off + i * 4;
        size_t p2 = par_off + i * 2;
        size_t m = mat_off + i * 64; /* 16 floats */
        if (o4 + 4 <= dlen)
            vn.nm_name_ids[i] = ru32(d, o4);
        if (o2 + 2 <= dlen)
            vn.nm_node_ids[i] = ru16(d, o2);
        if (n4 + 4 <= dlen)
            vn.name_ids[i] = ru32(d, n4);
        if (p2 + 2 <= dlen)
            vn.parents[i] = ru16(d, p2);
        if (m + 64 <= dlen)
            for (int j = 0; j < 16; ++j)
                vn.mats[i][j] = rf32(d, m + j * 4);
        else
            vn.mats[i] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    }
    return true;
}

/* ── matrix helpers ───────────────────────────────────────────────── */

static void mat4_mul(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k)
                s += a[k * 4 + row] * b[col * 4 + k];
            out[col * 4 + row] = s;
        }
}

static void mat4_rot_inv(const float m[16], float out[16]) {
    /* inverse of a pure-rotation = transpose of 3×3 block */
    out[0] = m[0];
    out[1] = m[4];
    out[2] = m[8];
    out[3] = 0;
    out[4] = m[1];
    out[5] = m[5];
    out[6] = m[9];
    out[7] = 0;
    out[8] = m[2];
    out[9] = m[6];
    out[10] = m[10];
    out[11] = 0;
    out[12] = 0;
    out[13] = 0;
    out[14] = 0;
    out[15] = 1;
}

static const float IDENTITY[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

static int find_node_by_name(const VisNodes &vn, const PDB &pdb, const char *name) {
    for (size_t i = 0; i < vn.nm_name_ids.size(); ++i) {
        std::string s = pdb.str.get(pdb.d(), pdb.len(), vn.nm_name_ids[i]);
        if (s == name)
            return (int)vn.nm_node_ids[i];
    }
    return -1;
}

static void world_transform(const VisNodes &vn, int idx, float out[16]) {
    if (idx < 0 || (size_t)idx >= vn.mats.size()) {
        memcpy(out, IDENTITY, 64);
        return;
    }
    memcpy(out, vn.mats[idx].data(), 64);
    int cur = idx;
    for (int i = 0; i < 200; ++i) {
        uint16_t p = vn.parents[cur];
        if (p == NO_PARENT || (size_t)p >= vn.mats.size())
            break;
        float tmp[16];
        mat4_mul(vn.mats[p].data(), out, tmp);
        memcpy(out, tmp, 64);
        cur = (int)p;
    }
}

/* ── model path → visual suffix ──────────────────────────────────── */

static std::string model_to_visual_suffix(const char *mp) {
    std::string s = mp;
    auto pos = s.rfind(".model");
    if (pos != std::string::npos)
        s.replace(pos, 6, ".visual");
    for (auto &c : s)
        if (c == '\\')
            c = '/';
    auto sl = s.rfind('/');
    if (sl != std::string::npos && sl > 0) {
        auto sl2 = s.rfind('/', sl - 1);
        if (sl2 != std::string::npos)
            return s.substr(sl2 + 1);
    }
    return s;
}

/* ── helper: load PDB + parse visual nodes for a suffix ──────────── */

static bool get_vis(const PDB &pdb, const char *suffix, VisNodes *vn_out) {
    int bi, ri;
    if (!pdb.resolve(suffix, &bi, &ri))
        return false;
    if ((unsigned)bi != VISUAL_BLOB)
        return false;
    size_t rd_off = pdb.record_abs(bi, ri, VISUAL_ISIZE);
    if (!rd_off)
        return false;
    return parse_vis_nodes(pdb, rd_off, *vn_out);
}

/* ── propeller lookup ─────────────────────────────────────────────── */

#define SKEL_EXT_BLOB 2u

/* Last two slash-separated path components (keeps the original extension). */
static std::string two_part_suffix(const char *path) {
    std::string s = path;
    for (auto &c : s)
        if (c == '\\')
            c = '/';
    auto sl = s.rfind('/');
    if (sl == std::string::npos || sl == 0)
        return s;
    auto sl2 = s.rfind('/', sl - 1);
    if (sl2 == std::string::npos)
        return s;
    return s.substr(sl2 + 1);
}

/* Record byte-size for a blob, computed from stored metadata. */
static size_t blob_isize(const PDB &pdb, int bi) {
    if (bi < 0 || (size_t)bi >= pdb.blobs.size())
        return 0;
    const DbBlob &b = pdb.blobs[bi];
    if (!b.record_count || b.size < 16)
        return 0;
    return (b.size - 16) / b.record_count;
}

/* Build name_id → propeller-model-name map from the PDB string table.
 * Matches "MP_*Propeller*" strings; strips _INDEX_N suffix for deduplication. */
/* TODO: propeller support is incomplete — discovery, placement, and orientation
 * are all wrong for several ship types; the functions below are WIP.
 * Known discovery issues:
 *   - Only finds propellers whose compound model is recorded in the PDB skel_ext
 *     blobs.  Many ships (Yamato, Alabama, Fletcher, …) have propellers baked
 *     directly into the hull geometry and are therefore never discovered here.
 *   - The MP_*Propeller* name_id scan assumes a fixed string-table layout that
 *     may not hold for all game versions.
 */

static std::unordered_map<uint32_t, std::string> build_prop_mount_id_map(const PDB &pdb) {
    std::unordered_map<uint32_t, std::string> out;
    const uint8_t *d = pdb.d();
    size_t dlen = pdb.len();
    for (uint32_t slot = 0; slot < pdb.str.cap; ++slot) {
        size_t b = pdb.str.boff + slot * 8;
        if (b + 8 > dlen) break;
        uint32_t bk = ru32(d, b), bs = ru32(d, b + 4);
        if (!bk && !bs) continue;
        uint32_t soff = ru32(d, pdb.str.voff + slot * 4);
        size_t abs = pdb.str.str_off + soff;
        if (abs >= pdb.str.str_off + pdb.str.str_size || abs >= dlen) continue;
        std::string s = read_cstr(d, dlen, abs);
        if (s.size() < 7 || s.substr(0, 3) != "MP_") continue;
        if (s.find("Propeller") == std::string::npos &&
            s.find("Screw") == std::string::npos) continue;
        std::string model_name = s.substr(3);
        auto dot = model_name.find('.');
        if (dot != std::string::npos) model_name = model_name.substr(0, dot);
        auto idx = model_name.find("_INDEX_");
        if (idx != std::string::npos) model_name = model_name.substr(0, idx);
        out[bk] = model_name;
    }
    return out;
}

/* Resolve a propeller model name to a geometry path via its visual record. */
/* TODO: verify geometry path resolution for all compound model naming schemes. */
static std::string resolve_prop_model_to_geom(const PDB &pdb, const std::string &model_name) {
    const uint8_t *d = pdb.d();
    size_t dlen = pdb.len();
    std::string vis_suf = model_name + "/" + model_name + ".visual";
    int vbi, vri;
    if (!pdb.resolve(vis_suf.c_str(), &vbi, &vri)) return "";
    if ((unsigned)vbi != VISUAL_BLOB) return "";
    size_t vrd_off = pdb.record_abs(vbi, vri, VISUAL_ISIZE);
    if (!vrd_off || vrd_off + VISUAL_ISIZE > dlen) return "";
    uint64_t geom_sid = ru64(d, vrd_off + 48);
    if (!geom_sid) return "";
    auto sit = pdb.sid_idx.find(geom_sid);
    if (sit == pdb.sid_idx.end()) return "";
    return pdb.reconstruct_path(sit->second);
}

/* Scan one skel_ext record for propeller data.
 *
 * Returns:
 *   l_path / r_path  – geometry paths for the L and R propeller models
 *   n_l / n_r        – number of L / R propeller instances found
 *   bone_mats        – all 64-byte world-space 4×4 matrices from Region C
 *                      (the section of the record that follows the filler block)
 *
 * Region C is located by finding the last propeller name_id in the record,
 * then skipping forward past filler (0x10c30510) to the first non-filler value.
 *
 * TODO: Region C parsing is fragile — the filler sentinel (0x10c30510) may not
 * be reliable across all game versions or ship types.  Validate against more ships.
 */
static bool scan_skel_ext(const PDB &pdb, int bi, int ri,
                           const std::unordered_map<uint32_t, std::string> &prop_ids,
                           std::string &l_path, std::string &r_path,
                           int &n_l, int &n_r,
                           std::vector<std::array<float, 16>> &bone_mats) {
    const uint8_t *d = pdb.d();
    size_t isize = blob_isize(pdb, bi);
    if (!isize) return false;
    size_t rd_off = pdb.record_abs(bi, ri, isize);
    if (!rd_off) return false;

    static const uint32_t FILLER = 0x10c30510u;

    /* Scan record for propeller name_ids, tracking the last offset found. */
    std::unordered_map<std::string, std::string> l_models, r_models;
    size_t last_prop_k = 0;
    bool found_any = false;

    for (size_t k = 0; k + 4 <= isize; k += 4) {
        auto it = prop_ids.find(ru32(d, rd_off + k));
        if (it == prop_ids.end()) continue;
        const std::string &mn = it->second;
        vlog("propellers", "  skel_ext[+%zu] name_id=0x%08x model='%s'\n",
             k, ru32(d, rd_off + k), mn.c_str());
        last_prop_k = k;
        found_any = true;
        bool is_l = mn.size() >= 2 && mn.substr(mn.size() - 2) == "_L";
        bool is_r = mn.size() >= 2 && mn.substr(mn.size() - 2) == "_R";
        if (is_l && !l_models.count(mn)) {
            std::string gp = resolve_prop_model_to_geom(pdb, mn);
            if (!gp.empty()) l_models[mn] = gp;
        }
        if (is_r && !r_models.count(mn)) {
            std::string gp = resolve_prop_model_to_geom(pdb, mn);
            if (!gp.empty()) r_models[mn] = gp;
        }
    }
    if (!found_any) return false;

    /* Count L and R propeller instances (each unique name_id counts once). */
    n_l = 0; n_r = 0;
    for (size_t k = 0; k + 4 <= isize; k += 4) {
        auto it = prop_ids.find(ru32(d, rd_off + k));
        if (it == prop_ids.end()) continue;
        const std::string &mn = it->second;
        if (mn.size() >= 2 && mn.substr(mn.size() - 2) == "_L") ++n_l;
        if (mn.size() >= 2 && mn.substr(mn.size() - 2) == "_R") ++n_r;
    }
    /* n_l / n_r count total name_id slots, not distinct models;
     * cap to ensure at least one if a side was found. */
    if (!l_models.empty() && n_l == 0) n_l = 1;
    if (!r_models.empty() && n_r == 0) n_r = 1;

    if (!l_models.empty()) l_path = l_models.begin()->second;
    if (!r_models.empty()) r_path = r_models.begin()->second;

    /* Find Region C: skip past filler that follows the name_ids block.
     * Region C contains the world-space 4×4 matrices for all skel_ext bones. */
    size_t rc_off = rd_off + last_prop_k + 4;
    /* skip any remaining non-filler values (e.g. extra name_ids) */
    while (rc_off + 4 <= rd_off + isize && ru32(d, rc_off) != FILLER)
        rc_off += 4;
    /* skip filler */
    while (rc_off + 4 <= rd_off + isize && ru32(d, rc_off) == FILLER)
        rc_off += 4;

    /* Read 64-byte column-major 4×4 matrices from Region C. */
    bone_mats.clear();
    for (size_t boff = rc_off; boff + 64 <= rd_off + isize; boff += 64) {
        std::array<float, 16> m;
        for (int j = 0; j < 16; ++j)
            m[j] = rf32(d, boff + j * 4);
        /* Sanity: last row should be (0,0,0,1). */
        if (fabsf(m[3]) > 0.1f || fabsf(m[7]) > 0.1f ||
            fabsf(m[11]) > 0.1f || fabsf(m[15] - 1.0f) > 0.1f)
            break;
        bone_mats.push_back(m);
    }

    vlog("propellers", "  skel_ext: n_l=%d n_r=%d region_c_bones=%zu\n",
         n_l, n_r, bone_mats.size());
    if (wows_assets_bin_verbose) {
        for (size_t bi2 = 0; bi2 < bone_mats.size(); ++bi2) {
            const auto &m = bone_mats[bi2];
            vlog("propellers", "  bone[%zu]: col0=(%.3f,%.3f,%.3f) col1=(%.3f,%.3f,%.3f) col2=(%.3f,%.3f,%.3f) pos=(%.3f,%.3f,%.3f)\n",
                 bi2, m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10], m[12],m[13],m[14]);
        }
    }
    return !bone_mats.empty() && (!l_path.empty() || !r_path.empty());
}



/* TODO: shaft-pair selection heuristics (spin-axis filter, col0-direction filter,
 * scale-consistency filter, solo-bone fallback) are tuned against a small sample
 * of ships and will likely need adjustment.  Known issues:
 *   - Y coordinate correction (keel-relative vs. visual space) is not reliable
 *     across all ship types.
 *   - Multi-shaft ships where all shafts are on the centreline are not handled.
 */
static wows_assets_bin_propeller_list_t *propellers_from_pdb(
    const PDB &pdb,
    const char *hull_model_path,
    const char **hull_geom_paths,
    size_t n_geom_paths)
{
    /* Build the global propeller mount-point name_id map once. */
    auto prop_ids = build_prop_mount_id_map(pdb);
    if (prop_ids.empty()) {
        vlog("propellers", "no MP_*Propeller* strings found in PDB\n");
        return nullptr;
    }

    size_t isize = blob_isize(pdb, SKEL_EXT_BLOB);
    if (!isize) {
        vlog("propellers", "skel_ext blob not found in PDB\n");
        return nullptr;
    }

    /* Scan each hull skel_ext record.  Prefer records that have both L and
     * R propeller name_ids; among those prefer non-bow sections. */
    std::string best_l, best_r;
    int best_nl = 0, best_nr = 0;
    std::vector<std::array<float, 16>> best_mats;
    bool best_has_lr = false, best_nonbow = false;

    auto lc_str = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return s;
    };

    for (size_t gi = 0; gi < n_geom_paths; ++gi) {
        std::string gsuf = two_part_suffix(hull_geom_paths[gi]);
        {
            auto ep = gsuf.rfind(".geometry");
            if (ep == std::string::npos) continue;
            gsuf.replace(ep, 9, ".skel_ext");
        }
        int bi, ri;
        if (!pdb.resolve(gsuf.c_str(), &bi, &ri)) continue;
        if ((unsigned)bi != SKEL_EXT_BLOB) continue;

        std::string l_path, r_path;
        int nl = 0, nr = 0;
        std::vector<std::array<float, 16>> mats;

        vlog("propellers", "scanning skel_ext %s\n", gsuf.c_str());
        if (!scan_skel_ext(pdb, bi, ri, prop_ids, l_path, r_path, nl, nr, mats))
            continue;

        bool has_lr = !l_path.empty() && !r_path.empty();
        bool nonbow = lc_str(hull_geom_paths[gi]).find("bow") == std::string::npos;

        bool better = best_mats.empty();
        if (!better) {
            if (has_lr && !best_has_lr)                        better = true;
            else if (has_lr == best_has_lr && nonbow && !best_nonbow) better = true;
        }
        if (better) {
            best_l = l_path; best_r = r_path;
            best_nl = nl; best_nr = nr;
            best_mats = std::move(mats);
            best_has_lr = has_lr; best_nonbow = nonbow;
        }
        if (best_has_lr && best_nonbow) break;
    }

    if (best_mats.empty() || (best_l.empty() && best_r.empty())) {
        vlog("propellers", "no propeller data found in any skel_ext\n");
        return nullptr;
    }

    /* Number of propellers per side = total name_id instances per side.
     * n_side = max(n_L, n_R) to handle unequal counts gracefully. */
    int n_side = std::max(best_nl, best_nr);
    if (n_side < 1) n_side = 1;
    vlog("propellers", "propeller geometry L='%s' R='%s' n_l=%d n_r=%d n_side=%d\n",
         best_l.c_str(), best_r.c_str(), best_nl, best_nr, n_side);

    /* Find symmetric pairs in Region C.
     * Criteria: bones on opposite sides of centreline, near-perfect mirror
     * symmetry in X, same height (Y), same longitudinal position (Z), and
     * a minimum off-centre offset in X (propeller shafts are never on the
     * centreline). */
    struct Pair {
        size_t i_neg, i_pos; /* index of negative-x and positive-x bone */
        float abs_z;
        float scale; /* col0 magnitude — proxy for uniform bone scale */
    };
    std::vector<Pair> pairs;
    size_t n = best_mats.size();
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = i + 1; j < n; ++j) {
            /* translation at mat[12..14] (column-major) */
            float xi = best_mats[i][12], yi = best_mats[i][13], zi = best_mats[i][14];
            float xj = best_mats[j][12], yj = best_mats[j][13], zj = best_mats[j][14];
            if (xi * xj >= 0.0f) continue;           /* must straddle centreline */
            if (fabsf(xi + xj) > 0.01f) continue;    /* near-perfect X symmetry */
            if (fabsf(yi - yj) > 0.3f) continue;     /* same height */
            if (fabsf(zi - zj) > 0.3f) continue;     /* same Z position */
            if (fabsf(xi) < 0.01f) continue;          /* must be off-centre */
            /* Spin axis filter: propeller shafts spin around the bow-stern (Z) axis.
             * col2_z = mat[10] in column-major layout.  Reject bones whose local
             * Z axis is not approximately aligned with world Z (e.g. rudder hinges). */
            if (fabsf(best_mats[i][10]) < 0.7f || fabsf(best_mats[j][10]) < 0.7f) continue;
            /* col0-direction filter: the bone's local-X axis (col0) must point
             * primarily in the world-X direction.  Propeller shaft bones have their
             * blade plane perpendicular to Z; symmetric non-propeller structures
             * (e.g. breakwaters, angled braces) have col0 rotated significantly
             * around Y.  Require |col0_x_normalised| > 0.9 for both bones. */
            {
                const auto &mi = best_mats[i]; const auto &mj = best_mats[j];
                float sxi = sqrtf(mi[0]*mi[0] + mi[1]*mi[1] + mi[2]*mi[2]);
                float sxj = sqrtf(mj[0]*mj[0] + mj[1]*mj[1] + mj[2]*mj[2]);
                if (sxi < 1e-6f || sxj < 1e-6f) continue;
                if (fabsf(mi[0]) / sxi < 0.9f || fabsf(mj[0]) / sxj < 0.9f) continue;
            }
            size_t neg_idx = (xi < 0.0f) ? i : j;
            size_t pos_idx = (xi < 0.0f) ? j : i;
            /* Scale = col0 magnitude (proxy for uniform scale of the bone). */
            const auto &mi = best_mats[i]; const auto &mj = best_mats[j];
            float si = sqrtf(mi[0]*mi[0] + mi[1]*mi[1] + mi[2]*mi[2]);
            float sj = sqrtf(mj[0]*mj[0] + mj[1]*mj[1] + mj[2]*mj[2]);
            pairs.push_back({neg_idx, pos_idx, fabsf(zi), (si + sj) * 0.5f});
        }
    }

    if (pairs.empty()) {
        vlog("propellers", "no symmetric shaft pairs found in Region C\n");
        /* Fallback: single-shaft or centerline-only ships (e.g. KGV) have
         * only one bone that passes the quality filters.  Output it directly. */
        std::vector<size_t> solo_bones;
        for (size_t i2 = 0; i2 < n; ++i2) {
            const auto &m = best_mats[i2];
            if (fabsf(m[10]) < 0.7f) continue;
            float col0_len = sqrtf(m[0]*m[0] + m[1]*m[1] + m[2]*m[2]);
            if (col0_len < 1e-6f || fabsf(m[0]) / col0_len < 0.9f) continue;
            solo_bones.push_back(i2);
        }
        if (solo_bones.empty()) {
            vlog("propellers", "no valid solo bones either\n");
            return nullptr;
        }
        if ((int)solo_bones.size() > n_side)
            solo_bones.resize(n_side);
        vlog("propellers", "using %zu solo bone(s) as propeller position(s)\n", solo_bones.size());
        auto *list = new wows_assets_bin_propeller_list_t;
        list->count = solo_bones.size();
        list->entries = new wows_assets_bin_propeller_t[list->count]();
        for (size_t pi = 0; pi < solo_bones.size(); ++pi) {
            auto &e = list->entries[pi];
            memcpy(e.mat, best_mats[solo_bones[pi]].data(), 64);
            const std::string &gp = best_l.empty() ? best_r : best_l;
            strncpy(e.geom_path, gp.c_str(), 511); e.geom_path[511] = '\0';
            snprintf(e.name, 63, "Propeller_%zu", pi + 1);
            vlog("propellers", "  solo prop_%zu pos=(%.3f,%.3f,%.3f) geom=%s\n",
                 pi + 1, e.mat[12], e.mat[13], e.mat[14], gp.c_str());
        }
        return list;
    }

    /* Sort by |Z| descending: stern-most pairs first. */
    std::sort(pairs.begin(), pairs.end(),
              [](const Pair &a, const Pair &b) { return a.abs_z > b.abs_z; });

    /* Scale-consistency filter: all propeller pairs on a ship share the same
     * propeller geometry, so their bone scales should match the outermost pair.
     * Reject any pair whose scale deviates more than 8 % from the reference.
     * This discards placeholder/bounding-box bones that happen to be symmetric
     * but represent a different object (e.g. a shaft strut attachment). */
    {
        float ref_scale = pairs[0].scale;
        auto it = std::remove_if(pairs.begin() + 1, pairs.end(),
            [ref_scale](const Pair &p) {
                return fabsf(p.scale - ref_scale) > 0.05f * ref_scale;
            });
        pairs.erase(it, pairs.end());
    }

    /* Take the top n_side pairs. */
    if ((int)pairs.size() > n_side)
        pairs.resize(n_side);

    vlog("propellers", "selected %zu propeller pair(s)\n", pairs.size());

    auto *list = new wows_assets_bin_propeller_list_t;
    list->count = pairs.size() * 2;
    list->entries = new wows_assets_bin_propeller_t[list->count]();
    size_t ei = 0;

    for (size_t pi = 0; pi < pairs.size(); ++pi) {
        const Pair &pr = pairs[pi];

        /* L (port, negative X) entry */
        {
            auto &e = list->entries[ei++];
            memcpy(e.mat, best_mats[pr.i_neg].data(), 64);
            const std::string &gp = best_l.empty() ? best_r : best_l;
            strncpy(e.geom_path, gp.c_str(), 511); e.geom_path[511] = '\0';
            snprintf(e.name, 63, "Propeller_L_%zu", pi + 1);
            vlog("propellers", "  prop L_%zu pos=(%.3f,%.3f,%.3f) geom=%s\n",
                 pi + 1, e.mat[12], e.mat[13], e.mat[14], gp.c_str());
        }

        /* R (starboard, positive X) entry */
        {
            auto &e = list->entries[ei++];
            memcpy(e.mat, best_mats[pr.i_pos].data(), 64);
            const std::string &gp = best_r.empty() ? best_l : best_r;
            strncpy(e.geom_path, gp.c_str(), 511); e.geom_path[511] = '\0';
            snprintf(e.name, 63, "Propeller_R_%zu", pi + 1);
            vlog("propellers", "  prop R_%zu pos=(%.3f,%.3f,%.3f) geom=%s\n",
                 pi + 1, e.mat[12], e.mat[13], e.mat[14], gp.c_str());
        }
    }

    return list;
}

/* ── public C API ─────────────────────────────────────────────────── */

static wows_assets_bin_hp_list_t *hp_transforms_from_pdb(const PDB &pdb, const char *visual_suffix) {
    VisNodes vn;
    if (!get_vis(pdb, visual_suffix, &vn))
        return nullptr;

    /* Collect HP_ nodes from the name_ids array */
    std::vector<std::pair<std::string, int>> hp;
    for (size_t i = 0; i < vn.name_ids.size(); ++i) {
        std::string name = pdb.str.get(pdb.d(), pdb.len(), vn.name_ids[i]);
        vlog("nodes", "  node[%zu] in %s: %s\n", i, visual_suffix, name.c_str());
        if (name.size() < 3 || name.substr(0, 3) != "HP_")
            continue;
        int ni = find_node_by_name(vn, pdb, name.c_str());
        if (ni >= 0)
            hp.push_back({name, ni});
    }

    auto *list = new wows_assets_bin_hp_list_t;
    list->count = hp.size();
    list->entries = new wows_assets_bin_hp_t[hp.size()];
    for (size_t i = 0; i < hp.size(); ++i) {
        strncpy(list->entries[i].name, hp[i].first.c_str(), 255);
        list->entries[i].name[255] = '\0';
        world_transform(vn, hp[i].second, list->entries[i].mat);
        vlog("HP_xform", "  %s: pos=(%.3f,%.3f,%.3f)\n",
             hp[i].first.c_str(),
             list->entries[i].mat[12], list->entries[i].mat[13], list->entries[i].mat[14]);
    }
    return list;
}

static wows_assets_bin_bb_list_t *blendbone_corrections_from_pdb(const PDB &pdb, const char **model_paths, size_t n_paths) {
    auto *list = new wows_assets_bin_bb_list_t;
    list->count = n_paths;
    list->entries = new wows_assets_bin_bb_t[n_paths];

    for (size_t i = 0; i < n_paths; ++i) {
        strncpy(list->entries[i].model_path, model_paths[i], 511);
        list->entries[i].model_path[511] = '\0';
        memcpy(list->entries[i].correction, IDENTITY, 64);

        std::string suffix = model_to_visual_suffix(model_paths[i]);
        VisNodes vn;
        if (!get_vis(pdb, suffix.c_str(), &vn))
            continue;

        int ni = find_node_by_name(vn, pdb, "Rotate_Y_BlendBone");
        if (ni < 0)
            ni = find_node_by_name(vn, pdb, "Root_BlendBone");
        if (ni < 0 || (size_t)ni >= vn.mats.size())
            continue;

        mat4_rot_inv(vn.mats[ni].data(), list->entries[i].correction);
    }
    return list;
}

extern "C" {

wows_assets_bin_hp_list_t *wows_assets_bin_get_hp_transforms(const char *path, const char *visual_suffix) {
    PDB pdb;
    if (!load_pdb(path, pdb))
        return nullptr;
    return hp_transforms_from_pdb(pdb, visual_suffix);
}

void wows_assets_bin_hp_list_free(wows_assets_bin_hp_list_t *list) {
    if (!list)
        return;
    delete[] list->entries;
    delete list;
}

wows_assets_bin_bb_list_t *wows_assets_bin_get_blendbone_corrections(const char *path, const char **model_paths, size_t n_paths) {
    PDB pdb;
    if (!load_pdb(path, pdb))
        return nullptr;
    return blendbone_corrections_from_pdb(pdb, model_paths, n_paths);
}

void wows_assets_bin_bb_list_free(wows_assets_bin_bb_list_t *list) {
    if (!list)
        return;
    delete[] list->entries;
    delete list;
}

/* ── PDB opaque handle ──────────────────────────────────────────── */

wows_assets_bin_pdb_t *wows_assets_bin_pdb_open(const char *path) {
    PDB *pdb = new PDB;
    if (!load_pdb(path, *pdb)) {
        delete pdb;
        return nullptr;
    }
    return reinterpret_cast<wows_assets_bin_pdb_t *>(pdb);
}

wows_assets_bin_pdb_t *wows_assets_bin_pdb_open_memory(const uint8_t *data, size_t size) {
    PDB *pdb = new PDB;
    if (!load_pdb_from_bytes(data, size, *pdb)) {
        delete pdb;
        return nullptr;
    }
    return reinterpret_cast<wows_assets_bin_pdb_t *>(pdb);
}

wows_assets_bin_hp_list_t *wows_assets_bin_get_hp_transforms_pdb(wows_assets_bin_pdb_t *handle, const char *visual_suffix) {
    PDB &pdb = *reinterpret_cast<PDB *>(handle);
    return hp_transforms_from_pdb(pdb, visual_suffix);
}

wows_assets_bin_bb_list_t *wows_assets_bin_get_blendbone_corrections_pdb(wows_assets_bin_pdb_t *handle, const char **model_paths,
                                                                          size_t n_paths) {
    PDB &pdb = *reinterpret_cast<PDB *>(handle);
    return blendbone_corrections_from_pdb(pdb, model_paths, n_paths);
}

void wows_assets_bin_pdb_free(wows_assets_bin_pdb_t *handle) {
    delete reinterpret_cast<PDB *>(handle);
}

/* ── visual info (render sets + LODs) ───────────────────────────── */

#define RENDER_SET_SIZE 0x28u
#define VIS_RS_COUNT_OFF 58u
#define VIS_LOD_COUNT_OFF 60u
#define VIS_RS_RP_OFF 96u
#define VIS_LOD_RP_OFF 104u

wows_assets_bin_visual_info_t *wows_assets_bin_get_visual_info(wows_assets_bin_pdb_t *handle, const char *visual_suffix) {
    PDB &pdb = *reinterpret_cast<PDB *>(handle);
    int bi, ri;
    if (!pdb.resolve(visual_suffix, &bi, &ri))
        return nullptr;
    if ((unsigned)bi != VISUAL_BLOB)
        return nullptr;
    size_t rd_off = pdb.record_abs(bi, ri, VISUAL_ISIZE);
    if (!rd_off)
        return nullptr;

    const uint8_t *d = pdb.d();
    size_t dlen = pdb.len();
    if (rd_off + VISUAL_ISIZE > dlen)
        return nullptr;

    uint16_t rs_count = ru16(d, rd_off + VIS_RS_COUNT_OFF);
    uint8_t lod_count = d[rd_off + VIS_LOD_COUNT_OFF];

    const char *vis_sl = strrchr(visual_suffix, '/');
    const char *vis_tag = vis_sl ? vis_sl + 1 : visual_suffix;

    vlog(vis_tag, "%u render sets, %u LODs\n", rs_count, lod_count);

    /* absolute start of render sets and LOD arrays */
    size_t rs_abs = (size_t)((int64_t)rd_off + ri64(d, rd_off + VIS_RS_RP_OFF));
    size_t lod_abs = (size_t)((int64_t)rd_off + ri64(d, rd_off + VIS_LOD_RP_OFF));

    auto *info = new wows_assets_bin_visual_info_t;
    info->rs_count = rs_count;
    info->lod_count = lod_count;
    info->render_sets = rs_count ? new wows_assets_bin_rs_t[rs_count]() : nullptr;
    info->lods = lod_count ? new wows_assets_bin_lod_t[lod_count]() : nullptr;

    /* parse render sets; build name_id → indices_mapping_id table for LODs */
    std::unordered_map<uint32_t, uint32_t> name_to_mid;

    for (uint16_t i = 0; i < rs_count; ++i) {
        size_t base = rs_abs + i * RENDER_SET_SIZE;
        if (base + RENDER_SET_SIZE > dlen)
            break;

        uint32_t name_id = ru32(d, base);
        uint32_t indices_mapping_id = ru32(d, base + 12);
        uint64_t mfm_path_id = ru64(d, base + 16);
        uint8_t nodes_cnt = d[base + 25];
        int64_t node_rp = ri64(d, base + 32);

        name_to_mid[name_id] = indices_mapping_id;

        wows_assets_bin_rs_t &rs = info->render_sets[i];
        rs.indices_mapping_id = indices_mapping_id;
        rs.mfm_path[0] = '\0';
        rs.section_name[0] = '\0';
        rs.node_name[0] = '\0';
        rs.is_damage = 0;

        /* resolve render-set section name */
        {
            std::string sn = pdb.str.get(d, dlen, name_id);
            strncpy(rs.section_name, sn.c_str(), 255);
            rs.section_name[255] = '\0';
        }

        /* resolve MFM full path */
        auto it = pdb.sid_idx.find(mfm_path_id);
        if (it != pdb.sid_idx.end()) {
            std::string fp = pdb.reconstruct_path(it->second);
            strncpy(rs.mfm_path, fp.c_str(), 511);
            rs.mfm_path[511] = '\0';
        }

        /* resolve node name (first entry in node_name_ids array) */
        if (nodes_cnt > 0 && node_rp != 0) {
            size_t arr_abs = (size_t)((int64_t)(base + 32) + node_rp);
            if (arr_abs + 4 <= dlen) {
                uint32_t node_name_id = ru32(d, arr_abs);
                std::string nm = pdb.str.get(d, dlen, node_name_id);
                strncpy(rs.node_name, nm.c_str(), 255);
                rs.node_name[255] = '\0';
            }
        }

    }

    if (wows_assets_bin_verbose) {
        std::map<std::string, int> rs_name_count, rs_name_seq;
        for (uint16_t i = 0; i < rs_count; ++i) {
            const char *n = info->render_sets[i].node_name;
            rs_name_count[n[0] ? n : "(unnamed)"]++;
        }
        for (uint16_t i = 0; i < rs_count; ++i) {
            size_t base = rs_abs + i * RENDER_SET_SIZE;
            const wows_assets_bin_rs_t &rs = info->render_sets[i];
            const char *primary = rs.section_name[0] ? rs.section_name
                                  : rs.node_name[0]  ? rs.node_name : "(unnamed)";
            std::string base_name(primary);
            std::string label = base_name;
            if (rs_name_count[base_name] > 1)
                label += "[" + std::to_string(rs_name_seq[base_name]++) + "]";
            uint32_t name_id = ru32(d, base);
            vlog(vis_tag, "  render_set %s (node: %s):\n", label.c_str(),
                 rs.node_name[0] ? rs.node_name : "(none)");
            vlog(vis_tag, "    +00 rs_name_id=%08x  +04 id_a=%08x  +08 id_b=%08x\n",
                 name_id, ru32(d, base + 4), ru32(d, base + 8));
            vlog(vis_tag, "    +0c geo_map_id=%08x  +10 material_id_lo=%08x  +14 material_id_hi=%08x\n",
                 rs.indices_mapping_id, ru32(d, base + 16), ru32(d, base + 20));
            uint16_t pad16 = (uint16_t)(d[base + 26] | (d[base + 27] << 8));
            uint32_t pad32 = ru32(d, base + 28);
            uint8_t nodes_cnt = d[base + 25];
            vlog(vis_tag, "    +18 multi_use=%02x  +19 nodes_cnt=%02x", d[base + 24], nodes_cnt);
            if (pad16) vlog(vis_tag, "  +1a pad16=%04x", pad16);
            if (pad32) vlog(vis_tag, "  +1c pad32=%08x", pad32);
            vlog(vis_tag, "\n");
        }
    }

    /* parse LOD table */
    for (uint8_t i = 0; i < lod_count; ++i) {
        size_t entry_base = lod_abs + i * 16u;
        if (entry_base + 16 > dlen)
            break;

        uint16_t rs_cnt = ru16(d, entry_base + 6);
        int64_t rp = ri64(d, entry_base + 8);
        size_t names_abs = (size_t)((int64_t)entry_base + rp);

        wows_assets_bin_lod_t &lod = info->lods[i];
        lod.count = 0;
        lod.mapping_ids = rs_cnt ? new unsigned int[rs_cnt] : nullptr;

        for (uint16_t j = 0; j < rs_cnt; ++j) {
            size_t noff = names_abs + j * 4u;
            if (noff + 4 > dlen)
                break;
            uint32_t nid = ru32(d, noff);
            auto it = name_to_mid.find(nid);
            if (it != name_to_mid.end())
                lod.mapping_ids[lod.count++] = it->second;
        }
    }

    /* mark render sets absent from every LOD as damage geometry */
    std::set<uint32_t> in_any_lod;
    for (uint8_t i = 0; i < lod_count; ++i)
    {
        std::map<std::string, int> rs_name_count, rs_name_seq;
        for (uint16_t i = 0; i < rs_count; ++i) {
            const char *n = info->render_sets[i].node_name;
            rs_name_count[n[0] ? n : "(unnamed)"]++;
        }
        for (uint16_t i = 0; i < rs_count; ++i) {
            wows_assets_bin_rs_t &rs = info->render_sets[i];
            const char *primary = rs.section_name[0] ? rs.section_name
                                  : rs.node_name[0]  ? rs.node_name : "(unnamed)";
            std::string base_name(primary);
            std::string label = base_name;
            if (rs_name_count[base_name] > 1)
                label += "[" + std::to_string(rs_name_seq[base_name]++) + "]";
            rs.is_damage = (label.find("_crack") != std::string::npos)
                               ? 1
                               : 0;
            vlog(vis_tag, "    %-40s geo_map_id=%08x is_damage=%d\n",
                 label.c_str(), rs.indices_mapping_id, rs.is_damage);
        }
    }

    return info;
}

void wows_assets_bin_visual_info_free(wows_assets_bin_visual_info_t *info) {
    if (!info)
        return;
    delete[] info->render_sets;
    if (info->lods) {
        for (size_t i = 0; i < info->lod_count; ++i)
            delete[] info->lods[i].mapping_ids;
        delete[] info->lods;
    }
    delete info;
}

wows_assets_bin_propeller_list_t *wows_assets_bin_get_propellers_pdb(
    wows_assets_bin_pdb_t *handle,
    const char *hull_model_path,
    const char **hull_geom_paths,
    size_t n_geom_paths)
{
    PDB &pdb = *reinterpret_cast<PDB *>(handle);
    return propellers_from_pdb(pdb, hull_model_path, hull_geom_paths, n_geom_paths);
}

void wows_assets_bin_propeller_list_free(wows_assets_bin_propeller_list_t *list) {
    if (!list)
        return;
    delete[] list->entries;
    delete list;
}

} /* extern "C" */
