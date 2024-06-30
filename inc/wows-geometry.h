#include <stdint.h>
#include <stdbool.h>

// Garbage from the .splash file

typedef struct {
    float x;
    float y;
    float z;
    float dx;
    float dy;
    float dz;
} wows_geometry_vertex_entry;

typedef struct {
    uint32_t n_ver_type; // number of vertice type
    uint32_t n_ind_type; // number of index type
    uint32_t n_ver_bloc; // number of vertice blocs
    uint32_t n_ind_bloc; // number of index blocs

    uint32_t n_col_bloc; // number of collision blocs
    uint32_t n_arm_bloc; // number of armor blocs
    uint64_t offset_1;   // offset to data beginning (always 72/0x48)

    uint64_t unk_1;
    uint64_t unk_2;

    uint64_t unk_3;
    uint64_t unk_4;

    uint64_t unk_5;
} wows_geometry_header;

typedef struct {
    wows_geometry_header *header;
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
