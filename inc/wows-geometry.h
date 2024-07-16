#include <stdint.h>
#include <stdbool.h>

// Garbage from the .splash file

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

#define VER_TYPE_00 "unknown"
#define ID_TYPE_00 0
#define VER_TYPE_01 "set3/xyznuv2iiiwwtbpc"
#define ID_TYPE_01 1
#define VER_TYPE_02 "set3/xyznuv2tbipc"
#define ID_TYPE_02 2
#define VER_TYPE_03 "set3/xyznuv2tbpc"
#define ID_TYPE_03 3
#define VER_TYPE_04 "set3/xyznuviiiwwpc"
#define ID_TYPE_04 4
#define VER_TYPE_05 "set3/xyznuviiiwwr"
#define ID_TYPE_05 5
#define VER_TYPE_06 "set3/xyznuviiiwwtbpc"
#define ID_TYPE_06 6
#define VER_TYPE_07 "set3/xyznuvpc"
#define ID_TYPE_07 7
#define VER_TYPE_08 "set3/xyznuvrpc"
#define ID_TYPE_08 8
#define VER_TYPE_09 "set3/xyznuvtbipc"
#define ID_TYPE_09 9
#define VER_TYPE_10 "set3/xyznuvtboi"
#define ID_TYPE_10 10
#define VER_TYPE_11 "set3/xyznuvtbpc"
#define ID_TYPE_11 11

typedef struct {
    float x;
    float y;
    float z;
    float dx;
    float dy;
    float dz;
} wows_geometry_vertex_entry;

#define WOWS_HEADER_SIZE 72
typedef struct {
    uint32_t n_ver_type; // number of vertice type
    uint32_t n_ind_type; // number of index type
    uint32_t n_ver_bloc; // number of vertice blocs
    uint32_t n_ind_bloc; // number of index blocs

    uint32_t n_col_bloc; // number of collision blocs
    uint32_t n_arm_bloc; // number of armor blocs
    uint64_t off_sec_1;  // offset to data beginning (always 72/0x48)

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
    // TODO
} wows_geometry_unk_2;

typedef struct {
    wows_geometry_header *header;
    wows_geometry_info *section_1;
    wows_geometry_info *section_2;
    wows_geometry_unk_1 *unk_1;
    wows_geometry_vertex_entry **entries; // array of entries
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
void wows_geometry_print(wows_geometry *geometry_content);
int wows_geometry_free(wows_geometry *geometry_content);
