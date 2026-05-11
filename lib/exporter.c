#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include "wows-geometry.h"
#include "internal.h"

#define GLB_MAGIC    0x46546C67u  // "glTF"
#define GLB_VERSION  2u
#define CHUNK_JSON   0x4E4F534Au  // "JSON"
#define CHUNK_BIN    0x004E4942u  // "BIN\0"

/* ── dynamic string buffer ─────────────────────────────────────── */

typedef struct { char *buf; size_t len, cap; } strbuf;

static int sb_init(strbuf *s, size_t n) {
    s->buf = malloc(n); s->len = 0; s->cap = s->buf ? n : 0;
    return s->buf ? 0 : -1;
}

static int sb_printf(strbuf *s, const char *fmt, ...) {
    for (;;) {
        size_t av = s->cap - s->len;
        va_list ap; va_start(ap, fmt);
        int n = vsnprintf(s->buf + s->len, av, fmt, ap);
        va_end(ap);
        if (n < 0) return -1;
        if ((size_t)n < av) { s->len += (size_t)n; return 0; }
        size_t nc = s->cap * 2 + (size_t)n + 4096;
        char *nb = realloc(s->buf, nc);
        if (!nb) return -1;
        s->buf = nb; s->cap = nc;
    }
}

/* ── dynamic binary buffer ─────────────────────────────────────── */

typedef struct { uint8_t *data; size_t len, cap; } binbuf;

static int bb_init(binbuf *b, size_t n) {
    b->data = malloc(n); b->len = 0; b->cap = b->data ? n : 0;
    return b->data ? 0 : -1;
}

static int bb_append(binbuf *b, const void *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap * 2 + n + 65536;
        uint8_t *nd = realloc(b->data, nc);
        if (!nd) return -1;
        b->data = nd; b->cap = nc;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static void bb_pad4(binbuf *b) {
    static const uint8_t z[4] = {0};
    size_t pad = (4 - (b->len & 3)) & 3;
    if (pad) bb_append(b, z, pad);
}

/* ── per-axis-aligned bounding box (needed by glTF spec for POSITION) */

typedef struct { float mn[3], mx[3]; } aabb;

static void aabb_init(aabb *a) {
    for (int i = 0; i < 3; i++) { a->mn[i] = 1e30f; a->mx[i] = -1e30f; }
}
static void aabb_update(aabb *a, float x, float y, float z) {
    float v[3] = {x, y, z};
    for (int i = 0; i < 3; i++) {
        if (v[i] < a->mn[i]) a->mn[i] = v[i];
        if (v[i] > a->mx[i]) a->mx[i] = v[i];
    }
}

/* ── find the vertex mapping entry for a given index bloc ───────────
 *
 * Raw index values in each bloc are 0-based relative to the draw call's
 * vertex base (section_1[j].items_offset).  To identify the correct
 * section_1[j] we scan the raw indices for max_raw_idx, then pick the
 * section_1 entry with:
 *   - same packed_texel_density
 *   - tightest items_count that is still > max_raw_idx
 *
 * Returns items_offset (vertex base) and sets *out_vtype to the
 * merged_buffer_index of the matched entry.
 */
static uint32_t find_vertex_base(const wows_geometry *geometry,
                                 uint32_t ibloc_idx,
                                 uint16_t *out_vtype) {
    const wows_geometry_info *s2 = &geometry->section_2[ibloc_idx];
    uint16_t ptd  = s2->packed_texel_density;
    uint16_t ibuf = s2->merged_buffer_index;
    uint32_t ioff = s2->items_offset;
    uint32_t icnt = s2->items_count;
    uint32_t n_vbloc = geometry->header->n_vertex_bloc;

    *out_vtype = 0;

    if (ibuf >= geometry->header->n_index_type ||
        !geometry->indexes || !geometry->indexes[ibuf] ||
        !geometry->indexes[ibuf]->raw_data) return 0;

    uint16_t is = geometry->indexes[ibuf]->index_size;
    if (ioff + icnt > geometry->indexes[ibuf]->index_count) return 0;

    /* scan raw indices for max value */
    const uint8_t *raw = geometry->indexes[ibuf]->raw_data + (size_t)ioff * is;
    uint32_t max_raw = 0;
    for (uint32_t j = 0; j < icnt; j++) {
        uint32_t v;
        if (is == 2) { uint16_t x; memcpy(&x, raw + j*2, 2); v = x; }
        else         { memcpy(&v, raw + j*4, 4); }
        if (v > max_raw) max_raw = v;
    }

    /* find section_1 entry: same ptd, tightest items_count > max_raw */
    uint32_t best_cnt = UINT32_MAX;
    uint32_t best_off = 0;
    uint16_t best_vt  = 0;
    for (uint32_t j = 0; j < n_vbloc; j++) {
        const wows_geometry_info *s1 = &geometry->section_1[j];
        if (s1->packed_texel_density != ptd) continue;
        if (s1->items_count > max_raw && s1->items_count < best_cnt) {
            best_cnt = s1->items_count;
            best_off = s1->items_offset;
            best_vt  = s1->merged_buffer_index;
        }
    }
    *out_vtype = best_vt;
    return best_off;
}

/* ─────────────────────────────────────────────────────────────────
 * Export to binary glTF (.glb).
 *
 * Raw index values decoded from the ENCD blocs are 0-based within each
 * draw call's private vertex range (section_1[j].items_offset ..
 * items_offset + items_count).  Before writing to the GLB we add
 * items_offset so the indices become absolute into the shared merged
 * vertex buffer that is exported once per vertex type.
 *
 * The pairing of an index bloc with its vertex mapping entry is found by
 * scanning raw indices for max_raw_idx, then picking the section_1 entry
 * with the same packed_texel_density whose items_count is the tightest
 * fit above max_raw_idx.
 *
 * glTF bufferView layout in BIN chunk:
 *   [pos[0], norm[0], uv[0],  pos[1], norm[1], uv[1], ... ]  ← vertex types
 *   [idx[0], idx[1], ... idx[n_ibloc-1] ]                     ← index slices
 *
 * Accessor indices:
 *   3*k+0  → POSITION    for vertex type k
 *   3*k+1  → NORMAL      for vertex type k
 *   3*k+2  → TEXCOORD_0  for vertex type k
 *   3*n_vtypes + i → indices for prim i
 * ───────────────────────────────────────────────────────────────── */
int wows_geometry_to_glb(wows_geometry *geometry, const char *output_path) {
    uint32_t n_vtypes = geometry->header->n_vertex_type;
    uint32_t n_itypes = geometry->header->n_index_type;
    uint32_t n_ibloc  = geometry->header->n_index_bloc;

    // Per-vertex-type state
    size_t   *pos_off  = calloc(n_vtypes, sizeof(size_t));
    size_t   *norm_off = calloc(n_vtypes, sizeof(size_t));
    size_t   *uv_off   = calloc(n_vtypes, sizeof(size_t));
    uint32_t *vt_cnt   = calloc(n_vtypes, sizeof(uint32_t));
    aabb     *vt_bb    = calloc(n_vtypes, sizeof(aabb));

    // Per-index-bloc state
    size_t   *idx_off  = calloc(n_ibloc, sizeof(size_t));
    uint32_t *idx_cnt  = calloc(n_ibloc, sizeof(uint32_t));
    uint16_t *idx_sz   = calloc(n_ibloc, sizeof(uint16_t));
    uint16_t *prim_vt  = calloc(n_ibloc, sizeof(uint16_t)); // vertex type per prim

    strbuf json; binbuf bin;
    if (!pos_off || !norm_off || !uv_off || !vt_cnt || !vt_bb ||
        !idx_off || !idx_cnt  || !idx_sz || !prim_vt ||
        sb_init(&json, 65536) || bb_init(&bin, 1 << 21)) {
        goto oom;
    }

    /* ── Phase 1: unpack each full merged vertex buffer ─────────── */
    for (uint32_t k = 0; k < n_vtypes; k++) {
        if (!geometry->vertexes || !geometry->vertexes[k] ||
            !geometry->vertexes[k]->raw_data) continue;

        uint32_t       total  = geometry->vertexes[k]->vertex_count;
        uint16_t       stride = geometry->vertex_meta_sections[k].s_vertex_size;
        const uint8_t *raw    = geometry->vertexes[k]->raw_data;

        float *pos  = malloc(total * 12);
        float *norm = malloc(total * 12);
        float *uv   = malloc(total *  8);
        if (!pos || !norm || !uv) { free(pos); free(norm); free(uv); goto oom; }

        aabb_init(&vt_bb[k]);
        for (uint32_t j = 0; j < total; j++) {
            const uint8_t *v = raw + (size_t)j * stride;
            float x, y, z;
            memcpy(&x, v + 0, 4); memcpy(&y, v + 4, 4); memcpy(&z, v + 8, 4);
            pos[j*3+0] = x; pos[j*3+1] = y; pos[j*3+2] = z;
            aabb_update(&vt_bb[k], x, y, z);

            uint32_t pn, puv;
            memcpy(&pn,  v + 12, 4);
            memcpy(&puv, v + 16, 4);
            wows_unpack_normal(pn,  &norm[j*3+0], &norm[j*3+1], &norm[j*3+2]);
            wows_unpack_uv(puv, &uv[j*2+0], &uv[j*2+1]);
        }

        bb_pad4(&bin); pos_off[k]  = bin.len; bb_append(&bin, pos,  total * 12);
        bb_pad4(&bin); norm_off[k] = bin.len; bb_append(&bin, norm, total * 12);
        bb_pad4(&bin); uv_off[k]   = bin.len; bb_append(&bin, uv,   total *  8);
        vt_cnt[k] = total;

        free(pos); free(norm); free(uv);
    }

    /* ── Phase 2: write index slices with absolute vertex offsets ── */
    for (uint32_t i = 0; i < n_ibloc; i++) {
        uint16_t ibuf = geometry->section_2[i].merged_buffer_index;
        uint32_t ioff = geometry->section_2[i].items_offset;
        uint32_t icnt = geometry->section_2[i].items_count;

        if (ibuf >= n_itypes || !geometry->indexes || !geometry->indexes[ibuf] ||
            !geometry->indexes[ibuf]->raw_data ||
            ioff + icnt > geometry->indexes[ibuf]->index_count) continue;

        uint16_t is = geometry->indexes[ibuf]->index_size;
        const uint8_t *raw = geometry->indexes[ibuf]->raw_data + (size_t)ioff * is;

        /* find the vertex base offset for this draw call */
        uint16_t vt;
        uint32_t vbase = find_vertex_base(geometry, i, &vt);
        prim_vt[i] = vt;

        bb_pad4(&bin);
        idx_off[i] = bin.len;

        if (vbase == 0) {
            /* no offset needed — write raw slice directly */
            bb_append(&bin, raw, (size_t)icnt * is);
        } else {
            /* add vertex base to each index value */
            for (uint32_t j = 0; j < icnt; j++) {
                if (is == 2) {
                    uint16_t v; memcpy(&v, raw + j*2, 2);
                    uint16_t vo = (uint16_t)(v + vbase);
                    bb_append(&bin, &vo, 2);
                } else {
                    uint32_t v; memcpy(&v, raw + j*4, 4);
                    uint32_t vo = v + vbase;
                    bb_append(&bin, &vo, 4);
                }
            }
        }

        idx_cnt[i] = icnt;
        idx_sz[i]  = is;
    }

    bb_pad4(&bin);
    size_t bin_len = bin.len;

    /* ── Phase 3: build glTF JSON ──────────────────────────────── */

    sb_printf(&json, "{");
    sb_printf(&json, "\"asset\":{\"version\":\"2.0\",\"generator\":\"wows-geometry\"},");
    sb_printf(&json, "\"scene\":0,");
    sb_printf(&json, "\"scenes\":[{\"nodes\":[0]}],");
    sb_printf(&json, "\"nodes\":[{\"mesh\":0}],");

    /* primitives */
    sb_printf(&json, "\"meshes\":[{\"primitives\":[");
    for (uint32_t i = 0; i < n_ibloc; i++) {
        uint16_t k = prim_vt[i];
        if (i > 0) sb_printf(&json, ",");
        sb_printf(&json,
            "{\"attributes\":{\"POSITION\":%u,\"NORMAL\":%u,\"TEXCOORD_0\":%u},"
            "\"indices\":%u}",
            3*k, 3*k+1, 3*k+2, 3*n_vtypes + i);
    }
    sb_printf(&json, "]}],");

    /* bufferViews: 3 per vertex type, then 1 per index bloc */
    sb_printf(&json, "\"bufferViews\":[");
    for (uint32_t k = 0; k < n_vtypes; k++) {
        uint32_t vc = vt_cnt[k];
        if (k > 0) sb_printf(&json, ",");
        sb_printf(&json,
            "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%u,\"target\":34962},"
            "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%u,\"target\":34962},"
            "{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%u,\"target\":34962}",
            pos_off[k],  vc * 12,
            norm_off[k], vc * 12,
            uv_off[k],   vc *  8);
    }
    for (uint32_t i = 0; i < n_ibloc; i++) {
        sb_printf(&json, ",{\"buffer\":0,\"byteOffset\":%zu,\"byteLength\":%u,\"target\":34963}",
                  idx_off[i], idx_cnt[i] * idx_sz[i]);
    }
    sb_printf(&json, "],");

    /* accessors: 3 per vertex type, then 1 per index bloc */
    sb_printf(&json, "\"accessors\":[");
    for (uint32_t k = 0; k < n_vtypes; k++) {
        uint32_t vc = vt_cnt[k];
        aabb *b = &vt_bb[k];
        if (k > 0) sb_printf(&json, ",");
        /* POSITION (requires min/max) */
        sb_printf(&json,
            "{\"bufferView\":%u,\"byteOffset\":0,\"componentType\":5126,"
            "\"count\":%u,\"type\":\"VEC3\","
            "\"min\":[%.6f,%.6f,%.6f],\"max\":[%.6f,%.6f,%.6f]},",
            3*k, vc, b->mn[0], b->mn[1], b->mn[2], b->mx[0], b->mx[1], b->mx[2]);
        /* NORMAL */
        sb_printf(&json,
            "{\"bufferView\":%u,\"byteOffset\":0,\"componentType\":5126,"
            "\"count\":%u,\"type\":\"VEC3\"},", 3*k+1, vc);
        /* TEXCOORD_0 */
        sb_printf(&json,
            "{\"bufferView\":%u,\"byteOffset\":0,\"componentType\":5126,"
            "\"count\":%u,\"type\":\"VEC2\"}", 3*k+2, vc);
    }
    for (uint32_t i = 0; i < n_ibloc; i++) {
        uint32_t icomp = (idx_sz[i] == 2) ? 5123u : 5125u;
        sb_printf(&json,
            ",{\"bufferView\":%u,\"byteOffset\":0,\"componentType\":%u,"
            "\"count\":%u,\"type\":\"SCALAR\"}",
            3*n_vtypes + i, icomp, idx_cnt[i]);
    }
    sb_printf(&json, "],");

    sb_printf(&json, "\"buffers\":[{\"byteLength\":%zu}]", bin_len);
    sb_printf(&json, "}");

    /* pad JSON to 4-byte boundary with spaces */
    while (json.len & 3) json.buf[json.len++] = ' ';

    /* ── Write GLB ───────────────────────────────────────────────── */
    FILE *f = fopen(output_path, "wb");
    if (!f) goto oom;

    uint32_t jlen = (uint32_t)json.len;
    uint32_t total = 12u + 8u + jlen + 8u + (uint32_t)bin_len;

    uint32_t hdr[3]  = {GLB_MAGIC, GLB_VERSION, total};
    uint32_t jhdr[2] = {jlen, CHUNK_JSON};
    uint32_t bhdr[2] = {(uint32_t)bin_len, CHUNK_BIN};

    fwrite(hdr,        4, 3, f);
    fwrite(jhdr,       4, 2, f);
    fwrite(json.buf,   1, jlen, f);
    fwrite(bhdr,       4, 2, f);
    fwrite(bin.data,   1, bin_len, f);
    fclose(f);

    free(pos_off); free(norm_off); free(uv_off); free(vt_cnt); free(vt_bb);
    free(idx_off); free(idx_cnt);  free(idx_sz);  free(prim_vt);
    free(json.buf); free(bin.data);
    return 0;

oom:
    free(pos_off); free(norm_off); free(uv_off); free(vt_cnt); free(vt_bb);
    free(idx_off); free(idx_cnt);  free(idx_sz);  free(prim_vt);
    free(json.buf); free(bin.data);
    return WOWS_ERROR_UNKNOWN;
}
