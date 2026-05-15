/**
 * @file stitch.h
 * @brief High-level ship assembly and GLB export API.
 *
 * This header provides the top-level `wows_stitch_export_ship()` function as well
 * as the lower-level helpers used internally to locate geometry files, decode
 * textures, and merge multiple GLB parts into a single scene.
 *
 * A typical caller only needs ::wows_stitch_export_ship; the remaining symbols are
 * exposed for unit-testing and for tools that need finer-grained control.
 */

#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <functional>
#include <tiny_gltf.h>
#include "wows-assets-bin.h"

/** @brief Set to `true` before calling any stitch function to enable verbose logging to stderr. */
extern bool wows_stitch_verbose;

/**
 * @brief Enable or disable verbose diagnostic output.
 *
 * @param v  `true` to enable, `false` to disable.
 */
void wows_stitch_set_verbose(bool v);

/**
 * @brief Conditionally print a formatted message to stderr when verbose mode is active.
 *
 * Usage mirrors `fprintf(stderr, ...)`.
 */
#define wows_stitch_vlog(...)                                                                                          \
    do {                                                                                                               \
        if (wows_stitch_verbose)                                                                                       \
            fprintf(stderr, __VA_ARGS__);                                                                              \
    } while (0)

/** @brief Column-major 4×4 transform matrix stored as a flat `double` vector of 16 elements. */
using wows_mat16d = std::vector<double>;

/** @defgroup wows_stitch_types Data types
 *  Structures used to describe ship parts and export options.
 *  @{
 */

/**
 * @brief Options controlling a full-ship GLB export.
 *
 * All fields have sensible defaults; only @p game_dir, @p ship_name, and
 * @p output_path must be specified.
 */
struct wows_ship_export_options {
    std::string gameparams_path;      /**< Path to `GameParams.data`; auto-detected from @p game_dir if empty. */
    std::string wows_assets_bin_path; /**< Path to `assets.bin`; auto-detected from @p game_dir if empty. */
    /** If non-empty, GameParams is loaded from this buffer instead of @p gameparams_path (archive / in-memory
     * workflows). */
    std::vector<uint8_t> gameparams_data;
    /** If non-empty, `assets.bin` is parsed from this buffer instead of @p wows_assets_bin_path. */
    std::vector<uint8_t> assets_bin_data;
    std::string hull_upgrade;    /**< Upgrade name substring used to select a hull; empty = latest hull. */
    bool with_turrets = true;    /**< Include turret and module meshes. */
    bool with_propellers = true; /**< Include propeller/screw meshes (discovered by name pattern). */
    bool with_textures = true;   /**< Embed textures in the output GLB. */
    int max_tex_size = 2048;     /**< Maximum texture dimension in pixels; larger textures are downscaled. */
    int lod_level = -1;          /**< LOD level to export; -1 = auto-select the highest detail level. */
    bool exclude_damage = true;  /**< Exclude damage and cross-section geometry sections. */
    /** Optional progress callback. Called with values 0–100 during export:
     *  0–20  geometry loading, 20–40  visual-info reading, 40–100  texture conversion. */
    std::function<void(int)> progress_cb;
};

/**
 * @brief A turret or module mount point, pairing a hardpoint name with a model path.
 */
struct wows_mount_entry {
    std::string hp_name;    /**< Hardpoint socket name (e.g. `"HP_MainGun_1"`). */
    std::string model_path; /**< Path to the mount's `.visual` model within the game tree. */
};

/**
 * @brief Aggregated hull information for one ship hull variant.
 */
struct wows_hull_info {
    std::string hull_model;               /**< Path to the primary hull `.visual` model. */
    std::vector<wows_mount_entry> mounts; /**< All mount points attached to this hull. */
};

/**
 * @brief One resolved geometry part ready to be merged into a GLB scene.
 */
struct wows_glb_part {
    tinygltf::Model model; /**< Parsed glTF model for this part. */
    std::string mesh_name; /**< Display name used for the mesh node in the merged scene. */
    std::string geom_path; /**< Filesystem path to the source `.geometry` file. */
    wows_mat16d matrix;    /**< World-space transform; empty vector means identity. */
};

/** @} */

/** @defgroup wows_stitch_export High-level export
 *  @{
 */

/**
 * @brief Export a complete ship model to a binary glTF (GLB) file.
 *
 * Locates all hull and turret geometry files, decodes vertices and indices,
 * applies textures, and writes the result to @p output_path.
 *
 * Python **must not** be initialised by the caller; this function manages
 * `Py_Initialize` / `Py_Finalize` internally for `GameParams.data` access.
 *
 * @param game_dir     Path to the game's root resource directory.
 * @param ship_name    Ship identifier as it appears in `GameParams.data` (e.g. `"PJSB009"`).
 * @param output_path  Destination path for the `.glb` output file.
 * @param opts         Export options; defaults are applied if not specified.
 * @return `true` on success; errors are printed to stderr.
 */
bool wows_stitch_export_ship(const std::string &game_dir, const std::string &ship_name, const std::string &output_path,
                             const wows_ship_export_options &opts = {});

/**
 * @brief Callback type for reading a file from an arbitrary storage backend.
 *
 * Takes a virtual file path (relative to the game root) and returns the
 * file bytes. Returns an empty vector on failure.
 */
using wows_file_provider_t = std::function<std::vector<uint8_t>(const std::string &)>;

/**
 * @brief Export a complete ship model to a binary glTF (GLB) buffer in memory.
 *
 * Like ::wows_stitch_export_ship but writes the result to an in-memory buffer
 * instead of a file. If @p file_provider is non-null, it is used to read
 * geometry files instead of the filesystem (allowing archive-backed storage).
 * GameParams.data and assets.bin may be supplied either as filesystem paths
 * (including auto-detection under @p game_dir) or via @p opts.gameparams_data /
 * @p opts.assets_bin_data when reading from archives or other non-file sources.
 *
 * @param game_dir      Path to the game's root resource directory (used as base
 *                      for paths when @p file_provider is null).
 * @param ship_name     Ship identifier in `GameParams.data`.
 * @param glb_out       Output: filled with the GLB bytes on success.
 * @param opts          Export options.
 * @param file_provider Optional: callback to read geometry files from memory.
 * @return `true` on success.
 */
bool wows_stitch_export_ship_to_glb_mem(const std::string &game_dir, const std::string &ship_name,
                                        std::vector<uint8_t> &glb_out, const wows_ship_export_options &opts = {},
                                        wows_file_provider_t file_provider = nullptr);

/** @} */

/** @defgroup wows_stitch_path Path and file utilities
 *  @{
 */

/** @brief Return the basename component of a path (everything after the last `/`). */
std::string wows_stitch_path_basename(const std::string &p);

/** @brief Return the directory component of a path (everything up to and including the last `/`). */
std::string wows_stitch_path_dirname(const std::string &p);

/** @brief Return the stem of a filename (basename without the final extension). */
std::string wows_stitch_stem(const std::string &filename);

/** @brief Replace all backslashes in @p s with forward slashes and return the result. */
std::string wows_stitch_normalize_slashes(std::string s);

/** @brief Return `true` if the file at @p p exists and is readable. */
bool wows_stitch_file_exists(const std::string &p);

/**
 * @brief Resolve a `.visual` model path to the corresponding `.geometry` file path.
 *
 * @param model    Path to the `.visual` model within the game tree.
 * @param game_dir Root resource directory.
 * @return Filesystem path to the `.geometry` file, or empty string if not found.
 */
std::string wows_stitch_model_to_geom_path(const std::string &model, const std::string &game_dir);

/**
 * @brief Convert a `.geometry` filesystem path to its `.visual` suffix form.
 *
 * The suffix is suitable as the @p visual_suffix argument to
 * ::wows_assets_bin_get_visual_info.
 *
 * @param geom_path  Filesystem path to a `.geometry` file.
 * @return Visual suffix string (e.g. `"ShipDir/ShipDir.visual"`).
 */
std::string wows_stitch_geom_to_visual_suffix(const std::string &geom_path);

/**
 * @brief Find all `.geometry` files belonging to a hull model.
 *
 * A single hull is often split into multiple part files (`_Bow`, `_MidFront`,
 * etc.); this function enumerates them all.
 *
 * @param hull_model  Path to the hull `.visual` model.
 * @param game_dir    Root resource directory.
 * @return Sorted list of filesystem paths to the part `.geometry` files.
 */
std::vector<std::string> wows_stitch_find_hull_geoms(const std::string &hull_model, const std::string &game_dir);

/**
 * @brief Find propeller/screw `.geometry` files for a ship.
 *
 * Scans the same directory as the hull geometry for files whose names contain
 * "prop" or "screw" (case-insensitive) and end in `.geometry`.  These are the
 * propeller meshes that the game animates at runtime.
 *
 * @param hull_model  Path to the hull `.visual` or `.model` file.
 * @param game_dir    Root resource directory.
 * @return Sorted list of filesystem paths to propeller `.geometry` files.
 */
std::vector<std::string> wows_stitch_find_propeller_geoms(const std::string &hull_model, const std::string &game_dir);

/**
 * @brief Search for a game asset file by name under @p game_dir.
 *
 * @param game_dir  Root resource directory.
 * @param filename  Filename (not a path) to search for.
 * @return Full filesystem path to the first match, or empty string if not found.
 */
std::string wows_stitch_find_game_file(const std::string &game_dir, const std::string &filename);

/** @} */

/** @defgroup wows_stitch_math Matrix math helpers
 *  @{
 */

/**
 * @brief Multiply two column-major 4×4 double matrices.
 *
 * @param a  Left-hand matrix (16 elements).
 * @param b  Right-hand matrix (16 elements).
 * @return Product `a × b` as a 16-element vector.
 */
wows_mat16d wows_stitch_mat4_mul_d(const wows_mat16d &a, const wows_mat16d &b);

/**
 * @brief Convert a `float[16]` column-major matrix to a `double` wows_mat16d.
 *
 * @param m  Source float array of 16 elements.
 * @return Double-precision copy as a wows_mat16d.
 */
wows_mat16d wows_stitch_float_to_double_mat(const float m[16]);

/** @} */

/** @defgroup wows_stitch_geom Geometry I/O
 *  @{
 */

/**
 * @brief Load a `.geometry` file and convert it to a tinygltf model.
 *
 * @param geom_path  Filesystem path to the `.geometry` file.
 * @param model_out  Output parameter filled with the parsed glTF model.
 * @return `true` on success.
 */
bool wows_stitch_geom_to_model(const std::string &geom_path, tinygltf::Model &model_out);

/**
 * @brief Load a `.geometry` file from a memory buffer and convert it to a tinygltf model.
 *
 * @param data      Pointer to the raw `.geometry` file data in memory.
 * @param size      Size of the data buffer in bytes.
 * @param model_out Output parameter filled with the parsed glTF model.
 * @return `true` on success.
 */
bool wows_stitch_geom_to_model_from_memory(const uint8_t *data, size_t size, tinygltf::Model &model_out);

/**
 * @brief Merge a list of ::wows_glb_part objects into a single tinygltf scene.
 *
 * Each part's mesh nodes are added under a root node and transformed by the
 * part's @p matrix field.
 *
 * @param parts  Parts to merge; modified in-place (meshes are moved out).
 * @return A merged tinygltf::Model containing all parts in one scene.
 */
tinygltf::Model wows_stitch_merge_parts(std::vector<wows_glb_part> &parts);

/** @} */

/** @defgroup wows_stitch_dds DDS texture decoding
 *  @{
 */

/**
 * @brief Decode a DDS texture buffer to raw RGBA pixels.
 *
 * Supports the BC1/BC3/BC5/BC7 block-compressed formats used in WoWs.
 *
 * @param d   Pointer to the DDS file data in memory.
 * @param sz  Size of the DDS data in bytes.
 * @param W   Output: decoded image width in pixels.
 * @param H   Output: decoded image height in pixels.
 * @return RGBA pixel data (`W × H × 4` bytes), or empty on failure.
 */
std::vector<uint8_t> wows_stitch_decode_dds(const uint8_t *d, size_t sz, int *W, int *H);

/**
 * @brief Load a `.dds` file from disk and encode it as a PNG byte buffer.
 *
 * If either dimension exceeds @p max_sz the image is downscaled to fit.
 *
 * @param path    Filesystem path to the `.dds` file.
 * @param max_sz  Maximum allowed width or height; 0 means no limit.
 * @return PNG-encoded bytes, or empty on failure.
 */
std::vector<uint8_t> wows_stitch_dds_to_png(const std::string &path, int max_sz);

/**
 * @brief Decode a DDS buffer in memory and encode it as a PNG byte buffer.
 *
 * Like ::wows_stitch_dds_to_png but operates on an already-loaded DDS buffer
 * rather than a filesystem path.  Used when textures are archive-backed.
 *
 * @param data    Pointer to DDS file data in memory.
 * @param size    Size of the DDS data in bytes.
 * @param max_sz  Maximum allowed width or height; 0 means no limit.
 * @return PNG-encoded bytes, or empty on failure.
 */
std::vector<uint8_t> wows_stitch_dds_to_png_from_memory(const uint8_t *data, size_t size, int max_sz);

/** @} */

/** @defgroup wows_stitch_tex Texture application
 *  @{
 */

/**
 * @brief Attach textures from `assets.bin` to the materials in a glTF model.
 *
 * Looks up each mesh section's MFM path via the PDB, locates the corresponding
 * `.dds` file, decodes it, and embeds the result as a PNG in the model.
 *
 * @param model       glTF model whose materials will be updated.
 * @param geom_order  Ordered list of `.geometry` file paths that contributed meshes.
 * @param pdb         Open `assets.bin` handle for material lookups.
 * @param game_dir    Root resource directory used to resolve texture paths.
 * @param lod_level   LOD level to use when selecting render sets (-1 = auto).
 * @param excl_damage If `true`, damage-geometry render sets are skipped.
 * @param max_tex     Maximum texture dimension; larger textures are downscaled.
 */
void wows_stitch_apply_textures(tinygltf::Model &model, const std::vector<std::string> &geom_order,
                                wows_assets_bin_pdb_t *pdb, const std::string &game_dir, int lod_level,
                                bool excl_damage, int max_tex, std::function<void(int)> progress_cb = nullptr,
                                wows_file_provider_t file_provider = nullptr);

/**
 * @brief Apply a uniform grey default material to all primitives in a model.
 *
 * Used as a fallback when texture lookup fails or textures are disabled.
 *
 * @param model  glTF model to modify in-place.
 */
void wows_stitch_apply_default_material(tinygltf::Model &model);

/** @} */
