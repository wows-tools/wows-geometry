#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── HP hardpoint transforms ─────────────────────────────────────── */

typedef struct {
    char name[256];
    float mat[16]; /* column-major 4×4 */
} assets_bin_hp_t;

typedef struct {
    assets_bin_hp_t *entries;
    size_t count;
} assets_bin_hp_list_t;

/* ── BlendBone corrections ───────────────────────────────────────── */

typedef struct {
    char model_path[512];
    float correction[16]; /* column-major 4×4 inverse(BlendBone) */
} assets_bin_bb_t;

typedef struct {
    assets_bin_bb_t *entries;
    size_t count;
} assets_bin_bb_list_t;

/* ── Opaque PDB handle (avoids repeated file loads) ─────────────── */

typedef struct assets_bin_pdb_s assets_bin_pdb_t;

assets_bin_pdb_t *assets_bin_pdb_open(const char *path);
void assets_bin_pdb_free(assets_bin_pdb_t *pdb);

/* ── Visual render-set and LOD info ──────────────────────────────── */

typedef struct {
    unsigned int indices_mapping_id;
    char mfm_path[512];  /* full MFM path in game tree */
    char node_name[256]; /* render-set node name */
    int is_damage;       /* 1 if this is damage/cross-section geometry */
} assets_bin_rs_t;

typedef struct {
    unsigned int *mapping_ids;
    size_t count;
} assets_bin_lod_t;

typedef struct {
    assets_bin_rs_t *render_sets;
    size_t rs_count;
    assets_bin_lod_t *lods;
    size_t lod_count;
} assets_bin_visual_info_t;

/* Query visual info for a suffix like "ShipDir/ShipDir.visual".
 * pdb must have been opened with assets_bin_pdb_open().
 * Returns NULL if the visual is not found. */
assets_bin_visual_info_t *assets_bin_get_visual_info(assets_bin_pdb_t *pdb, const char *visual_suffix);
void assets_bin_visual_info_free(assets_bin_visual_info_t *info);

/* ── Original one-shot helpers (load PDB internally) ─────────────── */

assets_bin_hp_list_t *assets_bin_get_hp_transforms(const char *path, const char *visual_suffix);
void assets_bin_hp_list_free(assets_bin_hp_list_t *list);

assets_bin_bb_list_t *assets_bin_get_blendbone_corrections(const char *path, const char **model_paths, size_t n_paths);
void assets_bin_bb_list_free(assets_bin_bb_list_t *list);

#ifdef __cplusplus
}
#endif
