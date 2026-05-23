#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 500
// TODO clean-up this mess
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <endian.h>
#include <math.h>
#include <meshoptimizer.h>

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#include <fcntl.h>
#include <io.h>
#define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#define SET_BINARY_MODE(file)
#endif

#include "wows-geometry.h"
#include "internal.h"

#define ENCD_MAGIC 0x44434E45u

// Context init function
WOWS_GEOMETRY_CONTEXT *wows_init_geometry_context(uint8_t debug_level) {
    WOWS_GEOMETRY_CONTEXT *context = calloc(sizeof(WOWS_GEOMETRY_CONTEXT), 1);
    context->debug_level = debug_level;
    context->is_le = true;
    return context;
}

int wows_parse_geometry_buffer(char *contents, size_t length, wows_geometry **geometry_content) {
    // TODO FIXME add size control

    char *start = contents;
    WOWS_GEOMETRY_CONTEXT *context = wows_init_geometry_context(10);
    wows_geometry *geometry = calloc(sizeof(wows_geometry), 1);

    // parsing the header
    wows_geometry_header *header = calloc(sizeof(wows_geometry_header), 1);
    header->n_vertex_type = geom_datatoh32(contents, 0, context);
    header->n_index_type = geom_datatoh32(contents, 4, context);
    header->n_vertex_bloc = geom_datatoh32(contents, 8, context);
    header->n_index_bloc = geom_datatoh32(contents, 12, context);
    header->n_collision_bloc = geom_datatoh32(contents, 16, context);
    header->n_armor_bloc = geom_datatoh32(contents, 20, context);
    header->off_vertices_mapping = geom_datatoh64(contents, 24, context);
    header->off_indices_mapping = geom_datatoh64(contents, 32, context);
    header->off_merged_vertices = geom_datatoh64(contents, 40, context);
    header->off_merged_indices = geom_datatoh64(contents, 48, context);
    header->off_collision_models = geom_datatoh64(contents, 56, context);
    header->off_armor_models = geom_datatoh64(contents, 64, context);
    geometry->header = header;

    // Parsing the vertex bloc mapping table
    wows_geometry_info *vertex_bloc_map = calloc(sizeof(wows_geometry_info), header->n_vertex_bloc);
    geometry->vertex_bloc_map = vertex_bloc_map;
    contents += header->off_vertices_mapping;

    for (int i = 0; i < (int)header->n_vertex_bloc; i++) {
        vertex_bloc_map[i].mapping_id = geom_datatoh32(contents, i * WOWS_BLOC_INFO_SIZE, context);
        vertex_bloc_map[i].merged_buffer_index = geom_datatoh16(contents, i * WOWS_BLOC_INFO_SIZE + 4, context);
        vertex_bloc_map[i].packed_texel_density = geom_datatoh16(contents, i * WOWS_BLOC_INFO_SIZE + 6, context);
        vertex_bloc_map[i].items_offset = geom_datatoh32(contents, i * WOWS_BLOC_INFO_SIZE + 8, context);
        vertex_bloc_map[i].items_count = geom_datatoh32(contents, i * WOWS_BLOC_INFO_SIZE + 12, context);
    }

    // Parsing the index bloc mapping table
    contents += header->n_vertex_bloc * WOWS_BLOC_INFO_SIZE;
    wows_geometry_info *index_bloc_map = calloc(sizeof(wows_geometry_info), header->n_index_bloc);
    geometry->index_bloc_map = index_bloc_map;

    for (int i = 0; i < (int)header->n_index_bloc; i++) {
        index_bloc_map[i].mapping_id = geom_datatoh32(contents, i * WOWS_BLOC_INFO_SIZE, context);
        index_bloc_map[i].merged_buffer_index = geom_datatoh16(contents, i * WOWS_BLOC_INFO_SIZE + 4, context);
        index_bloc_map[i].packed_texel_density = geom_datatoh16(contents, i * WOWS_BLOC_INFO_SIZE + 6, context);
        index_bloc_map[i].items_offset = geom_datatoh32(contents, i * WOWS_BLOC_INFO_SIZE + 8, context);
        index_bloc_map[i].items_count = geom_datatoh32(contents, i * WOWS_BLOC_INFO_SIZE + 12, context);
    }

    // Parsing the vertex type metadata (merged_vertices array)
    contents += header->n_index_bloc * WOWS_BLOC_INFO_SIZE;

    wows_geometry_vertex_section_metadata *vertex_meta_sections =
        calloc(sizeof(wows_geometry_vertex_section_metadata), header->n_vertex_type);
    geometry->vertex_meta_sections = vertex_meta_sections;
    for (int i = 0; i < (int)header->n_vertex_type; i++) {
        vertex_meta_sections[i].off_ver_bloc_start = geom_datatoh64(contents, i * WOWS_VERTEX_META_SIZE + 0, context);
        vertex_meta_sections[i].n_size_type_str = geom_datatoh64(contents, i * WOWS_VERTEX_META_SIZE + 8, context);
        vertex_meta_sections[i].off_ver_bloc_end = geom_datatoh64(contents, i * WOWS_VERTEX_META_SIZE + 16, context);
        vertex_meta_sections[i].s_ver_bloc_size = geom_datatoh32(contents, i * WOWS_VERTEX_META_SIZE + 24, context);
        vertex_meta_sections[i].s_vertex_size = geom_datatoh16(contents, i * WOWS_VERTEX_META_SIZE + 28, context);
        vertex_meta_sections[i].b_flag_1 = geom_datatoh8(contents, i * WOWS_VERTEX_META_SIZE + 30, context);
        vertex_meta_sections[i].b_flag_2 = geom_datatoh8(contents, i * WOWS_VERTEX_META_SIZE + 31, context);

        // Record absolute offset of the ENCD block and vertex type string
        vertex_meta_sections[i]._abs_start =
            contents + i * WOWS_VERTEX_META_SIZE - start + vertex_meta_sections[i].off_ver_bloc_start;
        vertex_meta_sections[i]._abs_end =
            contents + i * WOWS_VERTEX_META_SIZE - start + vertex_meta_sections[i].off_ver_bloc_end + 8;
        vertex_meta_sections[i]._vertex_type = vertex2id(start + vertex_meta_sections[i]._abs_end);
    }

    // Parsing the index type metadata (merged_indices array) at header->off_merged_indices
    wows_geometry_index_section_metadata *index_meta_sections =
        calloc(sizeof(wows_geometry_index_section_metadata), header->n_index_type);
    geometry->index_meta_sections = index_meta_sections;
    char *idx_meta_ptr = start + header->off_merged_indices;
    for (int i = 0; i < (int)header->n_index_type; i++) {
        size_t struct_base = (idx_meta_ptr + i * WOWS_INDEX_META_SIZE) - start;
        // data_relptr is a signed 64-bit relative pointer from struct base to ENCD block
        int64_t relptr;
        uint64_t raw = geom_datatoh64(idx_meta_ptr, i * WOWS_INDEX_META_SIZE + 0, context);
        memcpy(&relptr, &raw, sizeof(relptr));
        index_meta_sections[i].data_relptr = relptr;
        index_meta_sections[i].s_idx_bloc_size = geom_datatoh32(idx_meta_ptr, i * WOWS_INDEX_META_SIZE + 8, context);
        index_meta_sections[i]._reserved = geom_datatoh16(idx_meta_ptr, i * WOWS_INDEX_META_SIZE + 12, context);
        index_meta_sections[i].s_index_size = geom_datatoh16(idx_meta_ptr, i * WOWS_INDEX_META_SIZE + 14, context);
        index_meta_sections[i]._abs_start = (size_t)((int64_t)struct_base + relptr);
    }

    // Decode vertex ENCD blocks using meshoptimizer
    geometry->vertexes = calloc(sizeof(wows_geometry_vertex_section *), header->n_vertex_type);
    for (int i = 0; i < (int)header->n_vertex_type; i++) {
        size_t abs_start = vertex_meta_sections[i]._abs_start;
        uint32_t bloc_size = vertex_meta_sections[i].s_ver_bloc_size;
        uint16_t stride = vertex_meta_sections[i].s_vertex_size;

        wows_geometry_vertex_section *vs = calloc(sizeof(wows_geometry_vertex_section), 1);
        vs->_vertex_type = vertex_meta_sections[i]._vertex_type;
        geometry->vertexes[i] = vs;

        if (abs_start + bloc_size > length || stride == 0 || bloc_size < 8) {
            continue;
        }

        uint32_t magic;
        memcpy(&magic, start + abs_start, 4);
        magic = le32toh(magic);
        if (magic != ENCD_MAGIC) {
            // Raw (uncompressed) vertex data
            vs->vertex_count = bloc_size / stride;
            vs->raw_data = malloc(bloc_size);
            memcpy(vs->raw_data, start + abs_start, bloc_size);
            continue;
        }

        uint32_t element_count;
        memcpy(&element_count, start + abs_start + 4, 4);
        element_count = le32toh(element_count);

        const unsigned char *payload = (const unsigned char *)(start + abs_start + 8);
        size_t payload_size = bloc_size - 8;

        vs->vertex_count = element_count;
        vs->raw_data = malloc((size_t)element_count * stride);
        if (meshopt_decodeVertexBuffer(vs->raw_data, element_count, stride, payload, payload_size) != 0) {
            free(vs->raw_data);
            vs->raw_data = NULL;
            vs->vertex_count = 0;
        }
    }

    // Decode index ENCD blocks using meshoptimizer
    geometry->indexes = calloc(sizeof(wows_geometry_index_section *), header->n_index_type);
    for (int i = 0; i < (int)header->n_index_type; i++) {
        size_t abs_start = index_meta_sections[i]._abs_start;
        uint32_t bloc_size = index_meta_sections[i].s_idx_bloc_size;
        uint16_t index_size = index_meta_sections[i].s_index_size;

        wows_geometry_index_section *is = calloc(sizeof(wows_geometry_index_section), 1);
        is->index_size = index_size;
        geometry->indexes[i] = is;

        if (abs_start + bloc_size > length || index_size == 0 || bloc_size < 8) {
            continue;
        }

        uint32_t magic;
        memcpy(&magic, start + abs_start, 4);
        magic = le32toh(magic);
        if (magic != ENCD_MAGIC) {
            // Raw (uncompressed) index data
            is->index_count = bloc_size / index_size;
            is->raw_data = malloc(bloc_size);
            memcpy(is->raw_data, start + abs_start, bloc_size);
            continue;
        }

        uint32_t element_count;
        memcpy(&element_count, start + abs_start + 4, 4);
        element_count = le32toh(element_count);

        const unsigned char *payload = (const unsigned char *)(start + abs_start + 8);
        size_t payload_size = bloc_size - 8;

        // meshopt_decodeIndexBuffer always decodes to uint32; then downcast to index_size if needed
        uint32_t *tmp = malloc((size_t)element_count * sizeof(uint32_t));
        if (meshopt_decodeIndexBuffer(tmp, element_count, sizeof(uint32_t), payload, payload_size) != 0) {
            free(tmp);
            is->raw_data = NULL;
            is->index_count = 0;
            continue;
        }

        is->index_count = element_count;
        if (index_size == 2) {
            is->raw_data = malloc((size_t)element_count * 2);
            uint16_t *dst = (uint16_t *)is->raw_data;
            for (uint32_t j = 0; j < element_count; j++) {
                dst[j] = (uint16_t)tmp[j];
            }
        } else {
            is->raw_data = malloc((size_t)element_count * 4);
            memcpy(is->raw_data, tmp, (size_t)element_count * 4);
        }
        free(tmp);
    }

    free(context);
    contents += header->n_vertex_type * WOWS_VERTEX_META_SIZE;

    *geometry_content = geometry;
    return 0;
}

int wows_parse_geometry(char *input, wows_geometry **geometry_content) {
    int ret = 0;

    // Open the index file
    int fd = open(input, O_RDONLY);
    if (fd <= 0) {
        return WOWS_ERROR_NOT_A_FILE;
    }
    // Recover the file size
    struct stat s;
    fstat(fd, &s);

    /* index content size */
    size_t length = s.st_size;
    char *contents = mmap(0, length, PROT_READ, MAP_PRIVATE, fd, 0);
    ret = wows_parse_geometry_buffer(contents, length, geometry_content);
    close(fd);
    return ret;
}

int wows_parse_geometry_fp(FILE *input, wows_geometry **geometry_content) {
    if (input == NULL) {
        return WOWS_ERROR_NOT_A_FILE;
    }
    fseek(input, 0, SEEK_END);
    long length = ftell(input);
    fseek(input, 0, SEEK_SET);
    if (length <= 0) {
        return WOWS_ERROR_NOT_A_FILE;
    }
    char *contents = malloc((size_t)length);
    if (contents == NULL) {
        return WOWS_ERROR_UNKNOWN;
    }
    if (fread(contents, 1, (size_t)length, input) != (size_t)length) {
        free(contents);
        return WOWS_ERROR_UNKNOWN;
    }
    int ret = wows_parse_geometry_buffer(contents, (size_t)length, geometry_content);
    free(contents);
    return ret;
}

int wows_geometry_free(wows_geometry *geometry) {
    uint32_t n_vertex_type = geometry->header ? geometry->header->n_vertex_type : 0;
    uint32_t n_index_type = geometry->header ? geometry->header->n_index_type : 0;
    free(geometry->header);
    free(geometry->vertex_bloc_map);
    free(geometry->index_bloc_map);
    free(geometry->vertex_meta_sections);
    free(geometry->index_meta_sections);
    if (geometry->vertexes) {
        for (uint32_t i = 0; i < n_vertex_type; i++) {
            if (geometry->vertexes[i]) {
                free(geometry->vertexes[i]->raw_data);
                free(geometry->vertexes[i]);
            }
        }
        free(geometry->vertexes);
    }
    if (geometry->indexes) {
        for (uint32_t i = 0; i < n_index_type; i++) {
            if (geometry->indexes[i]) {
                free(geometry->indexes[i]->raw_data);
                free(geometry->indexes[i]);
            }
        }
        free(geometry->indexes);
    }
    free(geometry);
    return 0;
}
