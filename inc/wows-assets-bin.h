/**
 * @file assets_bin.h
 * @brief API for extracting model metadata from a BigWorld `assets.bin` packed database.
 *
 * `assets.bin` is a packed key-value store (PDB) that ships alongside the game
 * client.  It contains, among other things:
 *  - hardpoint (HP_) transform matrices used to position turrets and modules,
 *  - BlendBone correction matrices needed to align part geometries,
 *  - render-set / LOD listings that map draw-call IDs to material (.mfm) paths.
 *
 * The preferred usage pattern is to open a PDB handle once with
 * ::wows_assets_bin_pdb_open and then call query helpers as needed, rather than
 * using the one-shot helpers that reload the PDB on every call.
 */

#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern bool wows_assets_bin_verbose; /**< Set to `true` before any call to enable verbose logging. */
extern "C" {
#endif

/** @defgroup assets_hp Hardpoint transforms
 *  HP_ hardpoint world-space transform matrices extracted from `assets.bin`.
 *  Each hardpoint names a socket on the model (e.g. `HP_MainGun_1`).
 *  @{
 */

/**
 * @brief A single hardpoint transform entry.
 */
typedef struct {
    char name[256]; /**< Hardpoint name as stored in the PDB (e.g. `"HP_MainGun_1"`). */
    float mat[16];  /**< Column-major 4×4 world-space transform matrix. */
} wows_assets_bin_hp_t;

/**
 * @brief List of all hardpoint transforms for one visual.
 */
typedef struct {
    wows_assets_bin_hp_t *entries; /**< Array of hardpoint entries. */
    size_t count;                  /**< Number of entries in @p entries. */
} wows_assets_bin_hp_list_t;

/** @} */

/** @defgroup assets_bb BlendBone corrections
 *  Inverse BlendBone matrices used to correct part-model alignment when
 *  assembling a multi-part ship.
 *  @{
 */

/**
 * @brief A single BlendBone correction entry.
 */
typedef struct {
    char model_path[512]; /**< Path of the model this correction applies to, relative to the game tree. */
    float correction[16]; /**< Column-major 4×4 matrix equal to inverse(BlendBone transform). */
} wows_assets_bin_bb_t;

/**
 * @brief List of BlendBone corrections for a set of model paths.
 */
typedef struct {
    wows_assets_bin_bb_t *entries; /**< Array of BlendBone correction entries. */
    size_t count;                  /**< Number of entries in @p entries. */
} wows_assets_bin_bb_list_t;

/** @} */

/** @defgroup assets_pdb PDB handle
 *  Opaque handle for an open `assets.bin` packed database.
 *  Opening the PDB is relatively expensive (full file parse); keep the handle
 *  alive and reuse it across multiple queries.
 *  @{
 */

/**
 * @brief Opaque handle to an open `assets.bin` database.
 */
typedef struct wows_assets_bin_pdb_s wows_assets_bin_pdb_t;

/**
 * @brief Open and parse an `assets.bin` file.
 *
 * @param path  Filesystem path to the `assets.bin` file.
 * @return A heap-allocated PDB handle, or `NULL` on failure.
 */
wows_assets_bin_pdb_t *wows_assets_bin_pdb_open(const char *path);

/**
 * @brief Open and parse `assets.bin` from a memory buffer (contents are copied).
 *
 * @param data  Raw bytes of a valid `assets.bin` file.
 * @param size  Length of @p data in bytes.
 * @return A heap-allocated PDB handle, or `NULL` on failure.
 */
wows_assets_bin_pdb_t *wows_assets_bin_pdb_open_memory(const uint8_t *data, size_t size);

/**
 * @brief Close an `assets.bin` handle and release all associated memory.
 *
 * @param pdb  Handle previously returned by ::wows_assets_bin_pdb_open or ::wows_assets_bin_pdb_open_memory.
 */
void wows_assets_bin_pdb_free(wows_assets_bin_pdb_t *pdb);

/**
 * @brief Like ::wows_assets_bin_get_hp_transforms but uses an already-open PDB handle.
 */
wows_assets_bin_hp_list_t *wows_assets_bin_get_hp_transforms_pdb(wows_assets_bin_pdb_t *pdb, const char *visual_suffix);

/**
 * @brief Like ::wows_assets_bin_get_blendbone_corrections but uses an already-open PDB handle.
 */
wows_assets_bin_bb_list_t *wows_assets_bin_get_blendbone_corrections_pdb(wows_assets_bin_pdb_t *pdb,
                                                                         const char **model_paths, size_t n_paths);

/** @} */

/** @defgroup assets_visual Visual render-set and LOD info
 *  Render-set listings and LOD index tables for a given `.visual` resource.
 *  @{
 */

/**
 * @brief One render-set entry within a visual.
 *
 * A render set maps a draw-call `indices_mapping_id` to a material file
 * and a named mesh section, making it possible to look up which texture
 * belongs to each part of the exported geometry.
 */
typedef struct {
    unsigned int indices_mapping_id; /**< Draw-call ID; matches ::wows_geometry_info::mapping_id. */
    char mfm_path[512];              /**< Full MFM material path within the game's asset tree. */
    char section_name[256];          /**< Render-set section name (from the `name_id` field at offset 0). */
    char node_name[256];             /**< Node name (from the `node_name_ids` array; may be shared). */
    int is_damage;                   /**< Non-zero if this section represents damage/cross-section geometry. */
} wows_assets_bin_rs_t;

/**
 * @brief List of draw-call IDs belonging to one LOD level.
 */
typedef struct {
    unsigned int *mapping_ids; /**< Array of draw-call IDs active in this LOD. */
    size_t count;              /**< Number of IDs in @p mapping_ids. */
} wows_assets_bin_lod_t;

/**
 * @brief Complete render-set and LOD description for a visual resource.
 *
 * Returned by ::wows_assets_bin_get_visual_info; must be freed with
 * ::wows_assets_bin_visual_info_free.
 */
typedef struct {
    wows_assets_bin_rs_t *render_sets; /**< Array of render-set entries. */
    size_t rs_count;                   /**< Number of entries in @p render_sets. */
    wows_assets_bin_lod_t *lods;       /**< Array of LOD descriptors, index 0 = highest detail. */
    size_t lod_count;                  /**< Number of LOD levels in @p lods. */
} wows_assets_bin_visual_info_t;

/**
 * @brief Query render-set and LOD information for a `.visual` resource.
 *
 * @p visual_suffix must be a suffix path such as `"ShipDir/ShipDir.visual"`.
 * The @p pdb handle must have been obtained from ::wows_assets_bin_pdb_open.
 *
 * @param pdb            Open PDB handle.
 * @param visual_suffix  Suffix path of the visual resource to look up.
 * @return Heap-allocated ::wows_assets_bin_visual_info_t, or `NULL` if not found.
 */
wows_assets_bin_visual_info_t *wows_assets_bin_get_visual_info(wows_assets_bin_pdb_t *pdb, const char *visual_suffix);

/**
 * @brief Free memory allocated by ::wows_assets_bin_get_visual_info.
 *
 * @param info  Pointer returned by ::wows_assets_bin_get_visual_info.
 */
void wows_assets_bin_visual_info_free(wows_assets_bin_visual_info_t *info);

/** @} */

/** @defgroup assets_prop Propeller positions
 *  Propeller shaft positions and geometry paths derived from `assets.bin`.
 *
 *  Positions come from skeleton extension records (skel_ext, blob 2).
 *  The propeller model is found via the ship's compound model record (blob 3).
 *  @{
 */

/**
 * @brief One propeller placement entry.
 */
typedef struct {
    float mat[16];       /**< Column-major 4×4 translation-only transform (shaft centre). */
    char geom_path[512]; /**< Geometry path within the game asset tree. */
    char name[64];       /**< "Propeller_L_1", "Propeller_R_1", etc. */
} wows_assets_bin_propeller_t;

/**
 * @brief List of propeller placements for a ship.
 */
typedef struct {
    wows_assets_bin_propeller_t *entries; /**< Array of propeller entries. */
    size_t count;                         /**< Number of entries. */
} wows_assets_bin_propeller_list_t;

/**
 * @brief Query propeller geometry and shaft positions from an open PDB handle.
 *
 * @param pdb             Open PDB handle.
 * @param hull_model_path Full or relative path to the ship's main `.model` file
 *                        (as returned by GameParams; used to find the compound record).
 * @param hull_geom_paths Array of full paths to hull `.geometry` files
 *                        (used to locate the matching `.skel_ext` skeletons).
 * @param n_geom_paths    Number of entries in @p hull_geom_paths.
 * @return Heap-allocated list, or `NULL` when no propeller data is available.
 *         Free with ::wows_assets_bin_propeller_list_free.
 */
wows_assets_bin_propeller_list_t *wows_assets_bin_get_propellers_pdb(wows_assets_bin_pdb_t *pdb,
                                                                     const char *hull_model_path,
                                                                     const char **hull_geom_paths, size_t n_geom_paths);

/**
 * @brief Free memory allocated by ::wows_assets_bin_get_propellers_pdb.
 *
 * @param list  Pointer returned by ::wows_assets_bin_get_propellers_pdb.
 */
void wows_assets_bin_propeller_list_free(wows_assets_bin_propeller_list_t *list);

/** @} */

/** @defgroup assets_oneshot One-shot helpers
 *  Convenience wrappers that open the PDB internally for a single query.
 *  Use the PDB-handle API instead when making multiple queries on the same file.
 *  @{
 */

/**
 * @brief Extract hardpoint transforms for a visual, opening the PDB internally.
 *
 * @param path           Filesystem path to `assets.bin`.
 * @param visual_suffix  Suffix path of the target visual resource.
 * @return Heap-allocated list, or `NULL` on failure.  Free with ::wows_assets_bin_hp_list_free.
 */
wows_assets_bin_hp_list_t *wows_assets_bin_get_hp_transforms(const char *path, const char *visual_suffix);

/**
 * @brief Free memory allocated by ::wows_assets_bin_get_hp_transforms.
 *
 * @param list  List to free.
 */
void wows_assets_bin_hp_list_free(wows_assets_bin_hp_list_t *list);

/**
 * @brief Extract BlendBone correction matrices for a set of model paths.
 *
 * @param path        Filesystem path to `assets.bin`.
 * @param model_paths Array of model path strings to look up.
 * @param n_paths     Length of @p model_paths.
 * @return Heap-allocated list, or `NULL` on failure.  Free with ::wows_assets_bin_bb_list_free.
 */
wows_assets_bin_bb_list_t *wows_assets_bin_get_blendbone_corrections(const char *path, const char **model_paths,
                                                                     size_t n_paths);

/**
 * @brief Free memory allocated by ::wows_assets_bin_get_blendbone_corrections.
 *
 * @param list  List to free.
 */
void wows_assets_bin_bb_list_free(wows_assets_bin_bb_list_t *list);

/** @} */

#ifdef __cplusplus
}
#endif
