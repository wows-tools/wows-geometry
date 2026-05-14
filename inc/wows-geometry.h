/**
 * @file wows-geometry.h
 * @brief Public API for parsing and exporting World of Warships `.geometry` files.
 *
 * A `.geometry` file stores meshoptimizer-compressed (ENCD) vertex and index
 * buffers for one or more parts of a ship model.  This library decompresses
 * those buffers and can export the result as a binary glTF (GLB) file.
 *
 * Typical usage:
 * @code
 * wows_geometry *geo = NULL;
 * int rc = wows_parse_geometry("/path/to/ship.geometry", &geo);
 * if (rc == 0) {
 *     wows_geometry_to_glb(geo, "/path/to/out.glb");
 *     wows_geometry_free(geo);
 * }
 * @endcode
 */

#include <stdint.h>
#include <stdbool.h>

/** @defgroup errors Error codes
 *  Return values indicating parse or I/O failures.
 *  @{
 */
#define WOWS_ERROR_NOT_A_FILE 12 /**< Path argument is not a regular file. */
#define WOWS_ERROR_NOT_A_DIR 13  /**< Path argument is not a directory. */
#define WOWS_ERROR_UNKNOWN 7     /**< An unrecoverable internal error occurred. */
/** @} */

/** @defgroup vertex_types Vertex format identifiers
 *  Each `.geometry` file declares the vertex layout used by its vertex
 *  buffers.  The string name (e.g. `"set3/xyznuvtbpc"`) is stored in the
 *  file; the corresponding `WOWS_ID_*` constant is used internally to select the
 *  decode path.
 *  @{
 */
#define WOWS_VER_UNKNOWN "unknown"             /**< Unknown / unrecognised format string. */
#define WOWS_ID_UNKNOWN 0                      /**< Numeric ID for an unknown format. */

#define WOWS_VER_SET3_XYNUV2IIIWWTBPC "set3/xyznuv2iiiwwtbpc" /**< Format name: pos + 2×UV + IIIww + TB + PC. */
#define WOWS_ID_SET3_XYNUV2IIIWWTBPC 1

#define WOWS_VER_SET3_XYNUV2TBIPC "set3/xyznuv2tbipc"         /**< Format name: pos + 2×UV + TB + IPC. */
#define WOWS_ID_SET3_XYNUV2TBIPC 2

#define WOWS_VER_SET3_XYNUV2TBPC "set3/xyznuv2tbpc"           /**< Format name: pos + 2×UV + TB + PC. */
#define WOWS_ID_SET3_XYNUV2TBPC 3

#define WOWS_VER_SET3_XYNUVIIIWWPC "set3/xyznuviiiwwpc"       /**< Format name: pos + UV + IIIww + PC. */
#define WOWS_ID_SET3_XYNUVIIIWWPC 4

#define WOWS_VER_SET3_XYNUVIIIWWR "set3/xyznuviiiwwr"         /**< Format name: pos + UV + IIIww + R. */
#define WOWS_ID_SET3_XYNUVIIIWWR 5

#define WOWS_VER_SET3_XYNUVIIIWWTBPC "set3/xyznuviiiwwtbpc"   /**< Format name: pos + UV + IIIww + TB + PC. */
#define WOWS_ID_SET3_XYNUVIIIWWTBPC 6

#define WOWS_VER_SET3_XYNUVPC "set3/xyznuvpc"                 /**< Format name: pos + UV + PC. */
#define WOWS_ID_SET3_XYNUVPC 7

#define WOWS_VER_SET3_XYNUVRPC "set3/xyznuvrpc"               /**< Format name: pos + UV + R + PC. */
#define WOWS_ID_SET3_XYNUVRPC 8
#define WOWS_SIZE_XYNUVRPC 28                                  /**< Stride in bytes for the XYNUVRPC layout. */

#define WOWS_VER_SET3_XYNUVTBIPC "set3/xyznuvtbipc"           /**< Format name: pos + UV + TB + IPC. */
#define WOWS_ID_SET3_XYNUVTBIPC 9

#define WOWS_VER_SET3_XYNUVTBOI "set3/xyznuvtboi"             /**< Format name: pos + UV + TB + OI. */
#define WOWS_ID_SET3_XYNUVTBOI 10

#define WOWS_VER_SET3_XYNUVTBPC "set3/xyznuvtbpc"             /**< Format name: pos + UV + TB + PC. */
#define WOWS_ID_SET3_XYNUVTBPC 11
/** @} */

/**
 * @brief Common vertex fields shared by every vertex layout.
 *
 * This macro expands to the position, packed normal, unpacked normal
 * components, and texture coordinates that appear in every format variant.
 * It is used as the first member block of every vertex struct.
 */
#define WOWS_VERTEX_FIELDS        \
    float x;    /**< World-space position X. */  \
    float y;    /**< World-space position Y. */  \
    float z;    /**< World-space position Z. */  \
    uint32_t n; /**< Packed vertex normal (decoded to `_nx/_ny/_nz`). */ \
    float _nx;  /**< Unpacked normal X component. */  \
    float _ny;  /**< Unpacked normal Y component. */  \
    float _nz;  /**< Unpacked normal Z component. */  \
    float u;    /**< Texture coordinate U. */  \
    float v;    /**< Texture coordinate V. */

/**
 * @brief Minimal vertex containing only position, normal, and UV.
 *
 * Used when the on-disk format carries no tangent or other extra attributes.
 */
typedef struct {
    WOWS_VERTEX_FIELDS
} wows_vertex;

/**
 * @brief Vertex with tangent and binormal vectors (TB layouts).
 *
 * Extends the base vertex with packed tangent and binormal data needed for
 * normal-mapped rendering.
 */
typedef struct {
    WOWS_VERTEX_FIELDS
    uint32_t t; /**< Packed tangent vector. */
    float _tx;  /**< Unpacked tangent X. */
    float _ty;  /**< Unpacked tangent Y. */
    float _tz;  /**< Unpacked tangent Z. */
    uint32_t b; /**< Packed binormal vector. */
    float _bx;  /**< Unpacked binormal X. */
    float _by;  /**< Unpacked binormal Y. */
    float _bz;  /**< Unpacked binormal Z. */
} wows_vert_xyznuvtbpc;

/**
 * @brief Vertex with an extra 32-bit R attribute (R layouts).
 */
typedef struct {
    WOWS_VERTEX_FIELDS
    uint32_t r; /**< Extra R attribute (purpose varies by draw call). */
} wows_vert_xyznuvrpc;

/** @brief Size of a serialised ::wows_geometry_header in bytes. */
#define WOWS_HEADER_SIZE 72

/**
 * @brief Top-level `.geometry` file header at offset 0.
 *
 * Records element counts and absolute file offsets for every section.
 * All offsets are from the start of the file.
 */
typedef struct {
    uint32_t n_vertex_type;        /**< Number of merged vertex buffer descriptors. */
    uint32_t n_index_type;         /**< Number of merged index buffer descriptors. */
    uint32_t n_vertex_bloc;        /**< Number of vertex mapping entries (draw calls). */
    uint32_t n_index_bloc;         /**< Number of index mapping entries (draw calls). */
    uint32_t n_collision_bloc;     /**< Number of collision model entries. */
    uint32_t n_armor_bloc;         /**< Number of armor model entries. */
    uint64_t off_vertices_mapping; /**< Offset to the vertex mapping table (always 72 / 0x48). */
    uint64_t off_indices_mapping;  /**< Offset to the index mapping table. */
    uint64_t off_merged_vertices;  /**< Offset to the array of merged vertex buffers. */
    uint64_t off_merged_indices;   /**< Offset to the array of merged index buffers. */
    uint64_t off_collision_models; /**< Offset to the collision model array. */
    uint64_t off_armor_models;     /**< Offset to the armor model array. */
} wows_geometry_header;

/** @brief Size of a serialised ::wows_geometry_info entry in bytes. */
#define WOWS_BLOC_INFO_SIZE 16

/**
 * @brief Draw-call descriptor mapping a render set to a slice of a merged buffer.
 *
 * Each entry describes one draw call: which merged buffer to use and which
 * contiguous range of elements within it belongs to this draw call.
 */
typedef struct {
    uint32_t mapping_id;           /**< Draw-call identifier (links vertex and index entries). */
    uint16_t merged_buffer_index;  /**< Index into the merged vertex or index buffer array. */
    uint16_t packed_texel_density; /**< Packed texel-density value; pairs vertex and index entries. */
    uint32_t items_offset;         /**< First element index within the merged buffer. */
    uint32_t items_count;          /**< Number of elements for this draw call. */
} wows_geometry_info;

/** @brief Size of a serialised ::wows_geometry_vertex_section_metadata in bytes. */
#define WOWS_VERTEX_META_SIZE 32

/**
 * @brief Metadata for one merged vertex buffer (ENCD block descriptor).
 *
 * Describes the location and layout of a single meshoptimizer-compressed
 * vertex buffer inside the file.  Fields prefixed with `_` are set by the
 * parser and are not part of the on-disk format.
 */
typedef struct {
    uint64_t off_ver_bloc_start; /**< Relative offset from this struct to the start of the ENCD block. */
    uint64_t n_size_type_str;    /**< Byte length of the vertex-format type string (e.g. "set3/xyznuvtbpc"). */
    uint64_t off_ver_bloc_end;   /**< Relative offset to the end of the ENCD block. */
    uint32_t s_ver_bloc_size;    /**< Size of the ENCD block in bytes. */
    uint16_t s_vertex_size;      /**< Stride (bytes per vertex) as recorded in the file. */
    uint8_t b_flag_1;            /**< Reserved flag byte 1. */
    uint8_t b_flag_2;            /**< Reserved flag byte 2. */

    /* Parser-internal fields — not part of the on-disk format. */
    size_t _abs_start;   /**< Absolute file offset of the ENCD block start. */
    size_t _abs_end;     /**< Absolute file offset of the ENCD block end. */
    uint8_t _vertex_type; /**< Resolved vertex-type ID (one of the `WOWS_ID_*` constants). */
} wows_geometry_vertex_section_metadata;

/**
 * @brief Decoded vertex buffer for one merged vertex stream.
 *
 * Holds the raw bytes after meshoptimizer decompression.  The byte layout
 * depends on `_vertex_type`; cast `raw_data` to the appropriate struct
 * pointer to access individual vertices.
 */
typedef struct {
    uint8_t *raw_data;     /**< Decompressed vertex data (`vertex_count × stride` bytes). */
    uint32_t vertex_count; /**< Number of vertices in this buffer. */
    uint8_t _vertex_type;  /**< Vertex-type ID; determines how to interpret `raw_data`. */
} wows_geometry_vertex_section;

/** @brief Size of a serialised ::wows_geometry_index_section_metadata in bytes. */
#define WOWS_INDEX_META_SIZE 16

/**
 * @brief Metadata for one merged index buffer (ENCD block descriptor).
 *
 * Fields prefixed with `_` are set by the parser and are not part of the
 * on-disk format.
 */
typedef struct {
    int64_t data_relptr;      /**< Signed relative pointer from this struct's base to the ENCD block. */
    uint32_t s_idx_bloc_size; /**< Total ENCD block size in bytes (includes the 8-byte ENCD header). */
    uint16_t _reserved;       /**< Reserved / padding bytes. */
    uint16_t s_index_size;    /**< Bytes per index: 2 for `uint16_t`, 4 for `uint32_t`. */
    size_t _abs_start;        /**< Absolute file offset of the ENCD block. */
} wows_geometry_index_section_metadata;

/**
 * @brief Decoded index buffer for one merged index stream.
 */
typedef struct {
    uint8_t *raw_data;    /**< Decompressed index data (`index_count × index_size` bytes). */
    uint32_t index_count; /**< Number of indices in this buffer. */
    uint16_t index_size;  /**< Bytes per index: 2 or 4. */
} wows_geometry_index_section;

/**
 * @brief Top-level container for a fully parsed `.geometry` file.
 *
 * All pointer members are heap-allocated by ::wows_parse_geometry and must
 * be released with ::wows_geometry_free.
 */
typedef struct {
    wows_geometry_header                   *header;               /**< File header. */
    wows_geometry_info                     *section_1;            /**< Vertex draw-call mapping table. */
    wows_geometry_info                     *section_2;            /**< Index draw-call mapping table. */
    wows_geometry_vertex_section_metadata  *vertex_meta_sections; /**< Array of `header->n_vertex_type` vertex metadata entries. */
    wows_geometry_index_section_metadata   *index_meta_sections;  /**< Array of `header->n_index_type` index metadata entries. */
    wows_geometry_vertex_section          **vertexes;             /**< Array of decoded vertex buffers. */
    wows_geometry_index_section           **indexes;              /**< Array of decoded index buffers. */
} wows_geometry;

/**
 * @brief Library context holding global parse settings.
 *
 * Obtain one via the `wows_geometry_context_*` helpers (not yet public).
 * This structure is opaque by intent; do not access its fields directly.
 */
typedef struct {
    uint8_t debug_level; /**< Verbosity level for diagnostic output. */
    bool is_le;          /**< `true` if the host is little-endian. */
    char *err_msg;       /**< Last error message string (heap-allocated). */
} WOWS_GEOMETRY_CONTEXT;

/** @defgroup api Parse and export functions
 *  @{
 */

/**
 * @brief Parse a `.geometry` file from a memory-mapped path.
 *
 * Opens, maps, and fully decodes the file at @p input.  On success @p *geometry_content
 * points to a heap-allocated ::wows_geometry that must be freed with
 * ::wows_geometry_free.
 *
 * @param input             Path to the `.geometry` file.
 * @param geometry_content  Output pointer; set to the parsed geometry on success.
 * @return 0 on success, a non-zero error code on failure.
 */
int wows_parse_geometry(char *input, wows_geometry **geometry_content);

/**
 * @brief Parse a `.geometry` file from an already-open FILE stream.
 *
 * Reads from the current position of @p input to EOF.  Ownership of the
 * FILE handle is not transferred; the caller must close it.
 *
 * @param input             Open readable FILE stream positioned at the start of the data.
 * @param geometry_content  Output pointer; set to the parsed geometry on success.
 * @return 0 on success, a non-zero error code on failure.
 */
int wows_parse_geometry_fp(FILE *input, wows_geometry **geometry_content);

/**
 * @brief Print a human-readable summary of a parsed geometry to stdout.
 *
 * @param geometry_content  Geometry to inspect.
 * @param verbose           If `true`, print per-vertex data in addition to header fields.
 * @return 0 on success.
 */
int wows_geometry_print(wows_geometry *geometry_content, bool verbose);

/**
 * @brief Free all memory associated with a parsed geometry.
 *
 * Releases all buffers allocated during ::wows_parse_geometry or
 * ::wows_parse_geometry_fp and sets the pointer to `NULL` conceptually.
 *
 * @param geometry_content  Geometry to free.
 * @return 0 on success.
 */
int wows_geometry_free(wows_geometry *geometry_content);

/**
 * @brief Export a full geometry as a binary glTF (GLB) file.
 *
 * All vertex and index sections are merged into a single GLB scene.
 *
 * @param geometry     Parsed geometry to export.
 * @param output_path  Destination path for the `.glb` file.
 * @return 0 on success, non-zero on failure.
 */
int wows_geometry_to_glb(wows_geometry *geometry, const char *output_path);

/**
 * @brief Export a subset of draw-call sections as a GLB file.
 *
 * Only the draw calls whose `mapping_id` appears in @p sections are written.
 * Useful for isolating a specific LOD level or ship part.
 *
 * @param geometry     Parsed geometry to export.
 * @param output_path  Destination path for the `.glb` file.
 * @param sections     Array of `mapping_id` values to include.
 * @param n_sections   Length of the @p sections array.
 * @return 0 on success, non-zero on failure.
 */
int wows_geometry_to_glb_sections(wows_geometry *geometry, const char *output_path, const uint32_t *sections,
                                  uint32_t n_sections);

/**
 * @brief Parse a `.geometry` file from a raw memory buffer.
 *
 * @param contents          Pointer to the raw geometry file data.
 * @param length            Length of the buffer in bytes.
 * @param geometry_content  Output pointer; set to the parsed geometry on success.
 * @return 0 on success, a non-zero error code on failure.
 */
int wows_parse_geometry_buffer(char *contents, size_t length, wows_geometry **geometry_content);
/** @} */
