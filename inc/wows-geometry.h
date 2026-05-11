#include <stdint.h>
#include <stdbool.h>

#define WOWS_ERROR_NOT_A_FILE 12 /**< path is not a file */
#define WOWS_ERROR_NOT_A_DIR 13  /**< path is not a directory */
#define WOWS_ERROR_UNKNOWN 7     /**< An unknown error occurred. */

/* Counts:
      6 set3/xyznuv2iiiwwtbpc
      2 set3/xyznuv2tbipc
     88 set3/xyznuv2tbpc
    250 set3/xyznuviiiwwpc
     77 set3/xyznuviiiwwr
   2189 set3/xyznuviiiwwtbpc
   2885 set3/xyznuvpc
   5224 set3/xyznuvrpc
     71 set3/xyznuvtbipc
      1 set3/xyznuvtboi
   7720 set3/xyznuvtbpc
*/

#define VER_UNKNOWN "unknown"
#define ID_UNKNOWN 0
#define VER_SET3_XYNUV2IIIWWTBPC "set3/xyznuv2iiiwwtbpc"
#define ID_SET3_XYNUV2IIIWWTBPC 1
#define VER_SET3_XYNUV2TBIPC "set3/xyznuv2tbipc"
#define ID_SET3_XYNUV2TBIPC 2
#define VER_SET3_XYNUV2TBPC "set3/xyznuv2tbpc"
#define ID_SET3_XYNUV2TBPC 3
#define VER_SET3_XYNUVIIIWWPC "set3/xyznuviiiwwpc"
#define ID_SET3_XYNUVIIIWWPC 4
#define VER_SET3_XYNUVIIIWWR "set3/xyznuviiiwwr"
#define ID_SET3_XYNUVIIIWWR 5
#define VER_SET3_XYNUVIIIWWTBPC "set3/xyznuviiiwwtbpc"
#define ID_SET3_XYNUVIIIWWTBPC 6
#define VER_SET3_XYNUVPC "set3/xyznuvpc"
#define ID_SET3_XYNUVPC 7
#define VER_SET3_XYNUVRPC "set3/xyznuvrpc"
#define ID_SET3_XYNUVRPC 8
#define SIZE_XYNUVRPC 28
#define VER_SET3_XYNUVTBIPC "set3/xyznuvtbipc"
#define ID_SET3_XYNUVTBIPC 9
#define VER_SET3_XYNUVTBOI "set3/xyznuvtboi"
#define ID_SET3_XYNUVTBOI 10
#define VER_SET3_XYNUVTBPC "set3/xyznuvtbpc"
#define ID_SET3_XYNUVTBPC 11

#define WOWS_VERTEX_FIELDS                                                                                             \
    float x;    /* Position x */                                                                                       \
    float y;    /* Position y */                                                                                       \
    float z;    /* Position z */                                                                                       \
    uint32_t n; /* Vertex Normal for shading */                                                                        \
    float _nx;  /* Unpacked normal.x */                                                                                \
    float _ny;  /* Unpacked normal.y */                                                                                \
    float _nz;  /* Unpacked normal.z */                                                                                \
    float u;    /* U of UV texture mapping */                                                                          \
    float v;    /* V of UV texture mapping */

typedef struct {
    WOWS_VERTEX_FIELDS
} wows_vertex;

typedef struct {
    WOWS_VERTEX_FIELDS
    uint32_t t; // Vertex Tagent for shading
    float _tx;  // Unpacked tagent.x
    float _ty;  // Unpacked tagent.y
    float _tz;  // Unpacked tagent.z
    uint32_t b; // Vertex Binormal for shading
    float _bx;  // Unpacked binormal.x
    float _by;  // Unpacked binormal.y
    float _bz;  // Unpacked binormal.z
} wows_vert_xyznuvtbpc;

typedef struct {
    WOWS_VERTEX_FIELDS
    uint32_t r;
} wows_vert_xyznuvrpc;

#define WOWS_HEADER_SIZE 72
typedef struct {
    uint32_t n_vertex_type;         // number of merged vertex buffers
    uint32_t n_index_type;          // number of merged index buffers
    uint32_t n_vertex_bloc;         // number of vertex mapping entries
    uint32_t n_index_bloc;          // number of index mapping entries
    uint32_t n_collision_bloc;      // number of collision models
    uint32_t n_armor_bloc;          // number of armor models
    uint64_t off_vertices_mapping;  // offset to vertices mapping table (always 72/0x48)
    uint64_t off_indices_mapping;   // offset to indices mapping table
    uint64_t off_merged_vertices;   // offset to merged vertex buffers array
    uint64_t off_merged_indices;    // offset to merged index buffers array
    uint64_t off_collision_models;  // offset to collision models array
    uint64_t off_armor_models;      // offset to armor models array
} wows_geometry_header;

#define WOWS_BLOC_INFO_SIZE 16
typedef struct {
    uint32_t mapping_id;            // draw call identifier
    uint16_t merged_buffer_index;   // index into merged vertex or index buffer array
    uint16_t packed_texel_density;  // matches vertex and index mapping entries together
    uint32_t items_offset;          // first element within the merged buffer
    uint32_t items_count;           // number of elements for this draw call
} wows_geometry_info;

#define WOWS_VERTEX_META_SIZE 32
typedef struct {
    uint64_t
        off_ver_bloc_start; // Seems to be the offset to the corresponding vertice section relative to this current bloc
    uint64_t n_size_type_str; // Seems to be the size of vertice type string size (ex: set3/xyznuviiiwwtbpc)
    uint64_t off_ver_bloc_end;
    uint32_t s_ver_bloc_size;
    uint16_t s_vertex_size;
    uint8_t b_flag_1;
    uint8_t b_flag_2;

    // parsing internals, not part of the format
    size_t _abs_start;
    size_t _abs_end;
    uint8_t _vertex_type;
} wows_geometry_vertex_section_metadata;

typedef struct {
    uint8_t *raw_data;     // decoded vertex bytes (vertex_count * stride bytes)
    uint32_t vertex_count; // number of vertices (element_count from ENCD header)
    uint8_t _vertex_type;  // vertex type ID
} wows_geometry_vertex_section;

#define WOWS_INDEX_META_SIZE 16
typedef struct {
    int64_t data_relptr;       // relative pointer from struct base to ENCD block
    uint32_t s_idx_bloc_size;  // total ENCD block size (includes 8-byte ENCD header)
    uint16_t _reserved;
    uint16_t s_index_size;     // bytes per index: 2 (uint16) or 4 (uint32)
    size_t _abs_start;         // absolute file offset of ENCD block
} wows_geometry_index_section_metadata;

typedef struct {
    uint8_t *raw_data;    // decoded index bytes (index_count * index_size bytes)
    uint32_t index_count; // number of indices (element_count from ENCD header)
    uint16_t index_size;  // bytes per index: 2 or 4
} wows_geometry_index_section;

typedef struct {
    wows_geometry_header *header;
    wows_geometry_info *section_1;
    wows_geometry_info *section_2;
    wows_geometry_vertex_section_metadata *vertex_meta_sections;
    wows_geometry_index_section_metadata *index_meta_sections;
    wows_geometry_vertex_section **vertexes;
    wows_geometry_index_section **indexes;
} wows_geometry;

/*
 * @brief WoWs resource extractor context.
 *
 * This structure is used to hold the context for the WoWs extractor.
 *
 * This structure is not meant to be manipulated directly.
 * Internal fields are private and could be subject to changes.
 */
typedef struct {
    uint8_t debug_level; // Debug level for logging
    bool is_le;          // Flag for endianess (true if LE, false if BE)
    char *err_msg;       // Last error message
} WOWS_GEOMETRY_CONTEXT;

int wows_parse_geometry(char *input, wows_geometry **geometry_content);
int wows_parse_geometry_fp(FILE *input, wows_geometry **geometry_content);
int wows_geometry_print(wows_geometry *geometry_content, bool verbose);
int wows_geometry_free(wows_geometry *geometry_content);
int wows_geometry_to_glb(wows_geometry *geometry, const char *output_path);
