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
    uint32_t n_vertex_type; // number of vertice type
    uint32_t n_index_type;  // number of index type
    uint32_t n_vertex_bloc;
    ;                      // number of vertice blocs
    uint32_t n_index_bloc; // number of index blocs

    uint32_t n_collision_bloc; // number of collision blocs
    uint32_t n_armor_bloc;     // number of armor blocs
    uint64_t off_sec_1;        // offset to data beginning (always 72/0x48)

    uint64_t off_unk_1;
    uint64_t off_unk_2;

    uint64_t n_unk_3;
    uint64_t n_col_unk_4;

    uint64_t n_arm_unk_5;
} wows_geometry_header;

#define WOWS_BLOC_INFO_SIZE 16
typedef struct {
    uint32_t id_unk_6;
    uint16_t type_unk_7;
    uint16_t id_unk_8;
    uint32_t n_unk_9;
    uint32_t n_unk_10;
} wows_geometry_info;

#define WOWS_UNK_1_SIZE 32
typedef struct {
    uint64_t
        off_ver_bloc_start; // Seems to be the offset to the corresponding vertice section relative to this current bloc
    uint64_t n_size_type_str; // Seems to be the size of vertice type string size (ex: set3/xyznuviiiwwtbpc)
    uint64_t off_ver_bloc_end;
    uint32_t s_ver_bloc_size;
    uint32_t n_unk_5;

    // parsing internals, not part of the format
    size_t _abs_start;
    size_t _abs_end;
    uint8_t _vertex_type;
} wows_geometry_unk_1;

#define WOWS_UNK_2_SIZE 32
typedef struct {
    char magic[4];
    uint32_t n_unk_6;
    wows_vertex *vertices;
    uint8_t _vertex_type;
    uint32_t _vertex_count;
} wows_geometry_vertex_section;

typedef struct {
    wows_geometry_header *header;
    wows_geometry_info *section_1;
    wows_geometry_info *section_2;
    wows_geometry_unk_1 *unk_1;
    wows_vert_xyznuvtbpc **entries; // array of entries
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
int wows_geometry_print(wows_geometry *geometry_content);
int wows_geometry_free(wows_geometry *geometry_content);
