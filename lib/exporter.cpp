#include <cstdint>
#include <cstring>
#include <cstdio>
extern bool wows_stitch_verbose;
#define vlog(tag, fmt, ...) do { if (wows_stitch_verbose) fprintf(stderr, "[%s] " fmt, tag, ##__VA_ARGS__); } while (0)
#include <cmath>
#include <limits>
#include <vector>
#include <string>

#include <tiny_gltf.h>

extern "C" {
#include "wows-geometry.h"
#include "internal.h"
}

/* ── per-axis-aligned bounding box ──────────────────────────────── */

struct AABB {
    float mn[3], mx[3];
    AABB() {
        for (int i = 0; i < 3; i++) {
            mn[i] = 1e30f;
            mx[i] = -1e30f;
        }
    }
    void update(float x, float y, float z) {
        float v[3] = {x, y, z};
        for (int i = 0; i < 3; i++) {
            if (v[i] < mn[i])
                mn[i] = v[i];
            if (v[i] > mx[i])
                mx[i] = v[i];
        }
    }
};

/* ── find the vertex mapping entry for a given index bloc ───────────
 *
 * Raw index values are 0-based relative to the draw call's vertex base
 * (vertex_bloc_map[j].items_offset). Within a packed_texel_density group there
 * may be multiple vertex_bloc_map / index_bloc_map entries matched by rank: the k-th
 * index_bloc_map entry ordered by items_count DESC (items_offset ASC as
 * tiebreaker) maps to the k-th vertex_bloc_map entry with the same ordering.
 * Ranking by count ensures the largest index bloc maps to the largest vertex
 * bloc even when a smaller vertex bloc sits at a lower buffer offset.
 */
static uint32_t find_vertex_base(const wows_geometry *geometry, uint32_t ibloc_idx, uint16_t *out_vtype) {
    const wows_geometry_info *idx_tgt = &geometry->index_bloc_map[ibloc_idx];
    uint16_t ptd = idx_tgt->packed_texel_density;
    uint32_t icnt = idx_tgt->items_count;
    uint32_t ioff = idx_tgt->items_offset;
    uint32_t nv = geometry->header->n_vertex_bloc;
    uint32_t ni = geometry->header->n_index_bloc;

    *out_vtype = 0;

    /* rank: how many same-PTD index blocs have higher count (or same count
     * and lower offset) than this one */
    uint32_t rank = 0;
    for (uint32_t i = 0; i < ni; i++) {
        const wows_geometry_info *idx_cand = &geometry->index_bloc_map[i];
        if (idx_cand->packed_texel_density != ptd)
            continue;
        uint32_t oc = idx_cand->items_count, oo = idx_cand->items_offset;
        if (oc > icnt || (oc == icnt && oo < ioff))
            rank++;
    }

    /* walk through the rank-th vertex_bloc_map entry sorted by count DESC, offset ASC */
    uint32_t prev_cnt = UINT32_MAX, prev_off = UINT32_MAX;
    bool have_prev = false;
    for (uint32_t pass = 0; pass <= rank; pass++) {
        uint32_t cur_cnt = 0, cur_off = UINT32_MAX;
        uint16_t cur_vt = 0;
        for (uint32_t j = 0; j < nv; j++) {
            const wows_geometry_info *vtx_cand = &geometry->vertex_bloc_map[j];
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
            *out_vtype = cur_vt;
            return cur_off;
        }
        prev_cnt = cur_cnt;
        prev_off = cur_off;
        have_prev = true;
    }
    return 0;
}

/* ─────────────────────────────────────────────────────────────────
 * Export to binary glTF (.glb) using tinygltf.
 * sections/n_sections: optional vertex-section whitelist (NULL = all).
 * ───────────────────────────────────────────────────────────────── */
static bool vtype_included(uint32_t k, const uint32_t *sections, uint32_t n_sections) {
    if (n_sections == 0)
        return true;
    for (uint32_t i = 0; i < n_sections; i++)
        if (sections[i] == k)
            return true;
    return false;
}

extern "C" int wows_geometry_to_glb_sections(wows_geometry *geometry, const char *output_path, const uint32_t *sections,
                                             uint32_t n_sections) {
    uint32_t n_vtypes = geometry->header->n_vertex_type;
    uint32_t n_itypes = geometry->header->n_index_type;
    uint32_t n_ibloc = geometry->header->n_index_bloc;

    tinygltf::Model model;
    tinygltf::TinyGLTF writer;

    /* single binary buffer accumulator */
    std::vector<uint8_t> blob;
    blob.reserve(1 << 21);

    auto pad4 = [&]() {
        while (blob.size() & 3)
            blob.push_back(0);
    };
    auto append = [&](const void *src, size_t n) {
        const uint8_t *p = static_cast<const uint8_t *>(src);
        blob.insert(blob.end(), p, p + n);
    };

    /* accessor index base for vertex attribute accessors */
    /* layout: for each vertex type k: pos(3k), norm(3k+1), uv(3k+2)
     * then for each index bloc i: indices(3*n_vtypes + i)              */

    /* ── Phase 1: vertex buffers ───────────────────────────────── */
    struct VtypeInfo {
        size_t pos_bv, norm_bv, uv_bv;
        uint32_t count;
        AABB bb;
    };
    std::vector<VtypeInfo> vinfo(n_vtypes);

    for (uint32_t k = 0; k < n_vtypes; k++) {
        if (!vtype_included(k, sections, n_sections) || !geometry->vertexes || !geometry->vertexes[k] ||
            !geometry->vertexes[k]->raw_data) {
            vinfo[k].count = 0;
            continue;
        }
        uint32_t total = geometry->vertexes[k]->vertex_count;
        uint16_t stride = geometry->vertex_meta_sections[k].s_vertex_size;
        const uint8_t *raw = geometry->vertexes[k]->raw_data;

        std::vector<float> pos(total * 3), norm(total * 3), uv(total * 2);
        AABB bb;

        for (uint32_t j = 0; j < total; j++) {
            const uint8_t *v = raw + (size_t)j * stride;
            float x, y, z;
            memcpy(&x, v + 0, 4);
            memcpy(&y, v + 4, 4);
            memcpy(&z, v + 8, 4);
            pos[j * 3 + 0] = x;
            pos[j * 3 + 1] = y;
            pos[j * 3 + 2] = z;
            bb.update(x, y, z);

            uint32_t pn, puv;
            memcpy(&pn, v + 12, 4);
            memcpy(&puv, v + 16, 4);
            wows_unpack_normal(pn, &norm[j * 3 + 0], &norm[j * 3 + 1], &norm[j * 3 + 2]);
            wows_unpack_uv(puv, &uv[j * 2 + 0], &uv[j * 2 + 1]);
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
        vinfo[k].bb = bb;
    }

    /* ── Phase 2: index slices with absolute vertex offsets ──────── */
    struct IdxInfo {
        size_t bv_offset;
        uint32_t count;
        uint16_t idx_size;
        uint16_t vtype;
    };
    std::vector<IdxInfo> iinfo(n_ibloc);

    for (uint32_t i = 0; i < n_ibloc; i++) {
        iinfo[i].count = 0;
        uint16_t ibuf = geometry->index_bloc_map[i].merged_buffer_index;
        uint32_t ioff = geometry->index_bloc_map[i].items_offset;
        uint32_t icnt = geometry->index_bloc_map[i].items_count;

        if (ibuf >= n_itypes || !geometry->indexes || !geometry->indexes[ibuf] || !geometry->indexes[ibuf]->raw_data ||
            ioff + icnt > geometry->indexes[ibuf]->index_count)
            continue;

        uint16_t is = geometry->indexes[ibuf]->index_size;
        const uint8_t *raw = geometry->indexes[ibuf]->raw_data + (size_t)ioff * is;

        uint16_t vt;
        uint32_t vbase = find_vertex_base(geometry, i, &vt);
        if (!vtype_included(vt, sections, n_sections))
            continue;
        iinfo[i].vtype = vt;
        iinfo[i].count = icnt;

        /* upgrade uint16 indices to uint32 when absolute values exceed uint16 range */
        uint16_t out_is = is;
        if (is == 2) {
            uint32_t raw_max = 0;
            for (uint32_t j = 0; j < icnt; j++) {
                uint16_t v;
                memcpy(&v, raw + j * 2, 2);
                if ((uint32_t)v > raw_max)
                    raw_max = v;
            }
            if (raw_max + vbase > 65535u)
                out_is = 4;
        }
        iinfo[i].idx_size = out_is;

        pad4();
        iinfo[i].bv_offset = blob.size();

        for (uint32_t j = 0; j < icnt; j++) {
            uint32_t v = 0;
            if (is == 2) {
                uint16_t t;
                memcpy(&t, raw + j * 2, 2);
                v = t;
            } else {
                memcpy(&v, raw + j * 4, 4);
            }
            uint32_t vo = v + vbase;
            if (out_is == 4) {
                append(&vo, 4);
            } else {
                uint16_t vo16 = (uint16_t)vo;
                append(&vo16, 2);
            }
        }
    }

    pad4();

    /* ── Phase 3: populate tinygltf model ────────────────────────── */
    model.asset.version = "2.0";
    model.asset.generator = "wows-geometry";

    /* one buffer */
    tinygltf::Buffer buf;
    buf.data = std::move(blob);
    model.buffers.push_back(std::move(buf));

    /* helper: add a bufferView */
    auto add_bv = [&](size_t offset, size_t byte_len, int target) -> int {
        tinygltf::BufferView bv;
        bv.buffer = 0;
        bv.byteOffset = offset;
        bv.byteLength = byte_len;
        bv.target = target;
        model.bufferViews.push_back(bv);
        return (int)model.bufferViews.size() - 1;
    };

    /* helper: add an accessor */
    auto add_acc = [&](int bv_idx, int comp_type, int type, int count) -> int {
        tinygltf::Accessor acc;
        acc.bufferView = bv_idx;
        acc.byteOffset = 0;
        acc.componentType = comp_type;
        acc.type = type;
        acc.count = count;
        model.accessors.push_back(acc);
        return (int)model.accessors.size() - 1;
    };

    /* vertex type accessors */
    std::vector<int> pos_acc(n_vtypes, -1), norm_acc(n_vtypes, -1), uv_acc(n_vtypes, -1);
    for (uint32_t k = 0; k < n_vtypes; k++) {
        if (vinfo[k].count == 0)
            continue;
        uint32_t vc = vinfo[k].count;

        int bv_pos = add_bv(vinfo[k].pos_bv, vc * 12, TINYGLTF_TARGET_ARRAY_BUFFER);
        int bv_norm = add_bv(vinfo[k].norm_bv, vc * 12, TINYGLTF_TARGET_ARRAY_BUFFER);
        int bv_uv = add_bv(vinfo[k].uv_bv, vc * 8, TINYGLTF_TARGET_ARRAY_BUFFER);

        pos_acc[k] = add_acc(bv_pos, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vc);
        norm_acc[k] = add_acc(bv_norm, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC3, vc);
        uv_acc[k] = add_acc(bv_uv, TINYGLTF_COMPONENT_TYPE_FLOAT, TINYGLTF_TYPE_VEC2, vc);

        /* POSITION requires min/max */
        AABB &bb = vinfo[k].bb;
        model.accessors[pos_acc[k]].minValues = {bb.mn[0], bb.mn[1], bb.mn[2]};
        model.accessors[pos_acc[k]].maxValues = {bb.mx[0], bb.mx[1], bb.mx[2]};
    }

    /* index accessors + primitives */
    tinygltf::Mesh mesh;
    for (uint32_t i = 0; i < n_ibloc; i++) {
        if (iinfo[i].count == 0)
            continue;
        uint16_t k = iinfo[i].vtype;
        if (k >= n_vtypes || pos_acc[k] < 0)
            continue;

        int bv_idx = add_bv(iinfo[i].bv_offset, (size_t)iinfo[i].count * iinfo[i].idx_size,
                            TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER);
        int comp =
            (iinfo[i].idx_size == 2) ? TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT : TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT;
        int idx_acc = add_acc(bv_idx, comp, TINYGLTF_TYPE_SCALAR, iinfo[i].count);

        tinygltf::Primitive prim;
        prim.attributes["POSITION"] = pos_acc[k];
        prim.attributes["NORMAL"] = norm_acc[k];
        prim.attributes["TEXCOORD_0"] = uv_acc[k];
        prim.indices = idx_acc;
        prim.mode = TINYGLTF_MODE_TRIANGLES;
        mesh.primitives.push_back(prim);
    }

    model.meshes.push_back(mesh);

    tinygltf::Node node;
    node.mesh = 0;
    model.nodes.push_back(node);

    tinygltf::Scene scene;
    scene.nodes.push_back(0);
    model.scenes.push_back(scene);
    model.defaultScene = 0;

    /* ── Phase 4: write GLB ──────────────────────────────────────── */
    std::string err, warn;
    bool ok = writer.WriteGltfSceneToFile(&model, std::string(output_path),
                                          /* embedImages */ true,
                                          /* embedBuffers */ true,
                                          /* prettyPrint */ false,
                                          /* writeBinary */ true);

    if (!ok) {
        vlog("output.glb", "tinygltf write failed\n");
        return WOWS_ERROR_UNKNOWN;
    }
    return 0;
}

extern "C" int wows_geometry_to_glb(wows_geometry *geometry, const char *output_path) {
    return wows_geometry_to_glb_sections(geometry, output_path, NULL, 0);
}
