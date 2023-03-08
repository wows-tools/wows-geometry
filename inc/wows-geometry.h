#include <stdint.h>

typedef struct {
    uint32_t name_len;
    char *name;
    // 192 bits of data, maybe 6 x 32 bits fields (int or float) describing an (hit)box (x y z dx dy dz);
    // Order of coordinates is unknown (don't trust the current names)
    float x;
    float y;
    float z;
    float dx;
    float dy;
    float dz;
} wows_geometry_entry;

typedef struct {
    uint32_t entry_count;
    void *entry_map;             // struct *hashmap
    wows_geometry_entry **entries; // array of entries
} wows_geometry;

int wows_parse_geometry(char *input, wows_geometry **geometry_content);
int wows_parse_geometry_fp(FILE *input, wows_geometry **geometry_content);
int wows_geometry_print(wows_geometry *geometry_content);
int wows_geometry_free(wows_geometry *geometry_content);

// TODO
int wows_geometry_get_entry_by_name(wows_geometry *geometry_content, char *entry_name, wows_geometry_entry **entry);
