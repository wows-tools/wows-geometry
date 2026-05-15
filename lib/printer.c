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
#include <linux/limits.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <endian.h>
#include <math.h>

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#include <fcntl.h>
#include <io.h>
#define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#define SET_BINARY_MODE(file)
#endif

#include "wows-geometry.h"
#include "internal.h"

int wows_geometry_header_print(const wows_geometry_header *header) {
    if (header == NULL) {
        printf("Invalid header: NULL pointer.\n");
        return WOWS_ERROR_UNKNOWN; // TODO
    }

    printf("---------------- Header ------------------\n");
    printf("n_vertex_types:    %10u (0x%08x)\n", header->n_vertex_type, header->n_vertex_type);
    printf("n_index_types:     %10u (0x%08x)\n", header->n_index_type, header->n_index_type);
    printf("n_vertex_blocs:    %10u (0x%08x)\n", header->n_vertex_bloc, header->n_vertex_bloc);
    printf("n_index_blocs:     %10u (0x%08x)\n", header->n_index_bloc, header->n_index_bloc);
    printf("n_collision_blocs: %10u (0x%08x)\n", header->n_collision_bloc, header->n_collision_bloc);
    printf("n_armor_blocs:     %10u (0x%08x)\n", header->n_armor_bloc, header->n_armor_bloc);
    printf("off_vertices_mapping: %10lu (0x%08lx)\n", header->off_vertices_mapping, header->off_vertices_mapping);
    printf("off_indices_mapping:  %10lu (0x%08lx)\n", header->off_indices_mapping, header->off_indices_mapping);
    printf("off_merged_vertices:  %10lu (0x%08lx)\n", header->off_merged_vertices, header->off_merged_vertices);
    printf("off_merged_indices:   %10lu (0x%08lx)\n", header->off_merged_indices, header->off_merged_indices);
    printf("off_collision_models: %10lu (0x%08lx)\n", header->off_collision_models, header->off_collision_models);
    printf("off_armor_models:     %10lu (0x%08lx)\n", header->off_armor_models, header->off_armor_models);
    return 0;
}

int wows_geometry_info_print(const wows_geometry_info *section, uint32_t count, const char *section_name) {
    for (uint32_t i = 0; i < count; i++) {
        printf("--------- %s - Entry %02u -----------\n", section_name, i);
        printf("mapping_id:           %10u (0x%08x)\n", section[i].mapping_id, section[i].mapping_id);
        printf("merged_buffer_index:  %10u (0x%08x)\n", section[i].merged_buffer_index, section[i].merged_buffer_index);
        printf("packed_texel_density: %10u (0x%08x)\n", section[i].packed_texel_density,
               section[i].packed_texel_density);
        printf("items_offset:         %10u (0x%08x)\n", section[i].items_offset, section[i].items_offset);
        printf("items_count:          %10u (0x%08x)\n", section[i].items_count, section[i].items_count);
    }
    return 0;
}

int wows_geometry_vertex_section_metadata_print(const wows_geometry_vertex_section_metadata *section, uint32_t count,
                                                const char *section_name) {
    for (uint32_t i = 0; i < count; i++) {
        printf("--------- %s - Entry %02u -----------\n", section_name, i);
        printf("off_ver_bloc_start:%10lu (0x%08lx)\n", section[i].off_ver_bloc_start, section[i].off_ver_bloc_start);
        printf("n_size_type_str:   %10lu (0x%08lx)\n", section[i].n_size_type_str, section[i].n_size_type_str);
        printf("off_ver_bloc_end:  %10lu (0x%08lx)\n", section[i].off_ver_bloc_end, section[i].off_ver_bloc_end);
        printf("s_ver_bloc_size:   %10u (0x%08x)\n", section[i].s_ver_bloc_size, section[i].s_ver_bloc_size);
        printf("s_vertex_size:     %10u (0x%04x)\n", section[i].s_vertex_size, section[i].s_vertex_size);
        printf("b_flag_1:          %10u (0x%02x)\n", section[i].b_flag_1, section[i].b_flag_1);
        printf("b_flag_2:          %10u (0x%02x)\n", section[i].b_flag_2, section[i].b_flag_2);
        printf("_abs_start:        %10lu (0x%08lx)\n", section[i]._abs_start, section[i]._abs_start);
        printf("_abs_end:          %10lu (0x%08lx)\n", section[i]._abs_end, section[i]._abs_end);
        printf("_vertex_type:      %23s\n", id2vertex(section[i]._vertex_type));
    }
    return 0;
}

int wows_geometry_index_section_metadata_print(const wows_geometry_index_section_metadata *section, uint32_t count,
                                               const char *section_name) {
    for (uint32_t i = 0; i < count; i++) {
        printf("--------- %s - Entry %02u -----------\n", section_name, i);
        printf("data_relptr:       %10ld (0x%08lx)\n", section[i].data_relptr, (uint64_t)section[i].data_relptr);
        printf("s_idx_bloc_size:   %10u (0x%08x)\n", section[i].s_idx_bloc_size, section[i].s_idx_bloc_size);
        printf("s_index_size:      %10u (0x%04x)\n", section[i].s_index_size, section[i].s_index_size);
        printf("_abs_start:        %10lu (0x%08lx)\n", section[i]._abs_start, section[i]._abs_start);
    }
    return 0;
}

int wows_geometry_vertex_sections_print(wows_geometry_vertex_section **vertexes, uint32_t count,
                                        const wows_geometry_vertex_section_metadata *meta, bool verbose) {
    for (uint32_t i = 0; i < count; i++) {
        const wows_geometry_vertex_section *vs = vertexes[i];
        printf("--------- Vertex Section %02u -----------\n", i);
        printf("_vertex_type:      %23s\n", id2vertex(vs->_vertex_type));
        printf("vertex_count:      %10u\n", vs->vertex_count);
        if (vs->raw_data == NULL || vs->vertex_count == 0) {
            printf("(no decoded data)\n");
            continue;
        }

        uint16_t stride = meta[i].s_vertex_size;
        uint32_t print_count = verbose ? vs->vertex_count : (vs->vertex_count < 4 ? vs->vertex_count : 4);
        for (uint32_t j = 0; j < print_count; j++) {
            uint8_t *v = vs->raw_data + (size_t)j * stride;
            float x, y, z;
            memcpy(&x, v + 0, 4);
            memcpy(&y, v + 4, 4);
            memcpy(&z, v + 8, 4);
            uint32_t packed_n;
            memcpy(&packed_n, v + 12, 4);
            float nx, ny, nz;
            wows_unpack_normal(packed_n, &nx, &ny, &nz);
            uint32_t packed_uv;
            memcpy(&packed_uv, v + 16, 4);
            float u, vcoord;
            wows_unpack_uv(packed_uv, &u, &vcoord);
            printf("  v[%u]: pos=(%.4f, %.4f, %.4f)  n=(%.3f, %.3f, %.3f)  uv=(%.4f, %.4f)\n", j, x, y, z, nx, ny, nz,
                   u, vcoord);
        }
        if (!verbose && vs->vertex_count > print_count) {
            printf("  ... (%u more vertices)\n", vs->vertex_count - print_count);
        }
    }
    return 0;
}

int wows_geometry_index_sections_print(wows_geometry_index_section **indexes, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        const wows_geometry_index_section *is = indexes[i];
        printf("--------- Index Section %02u -----------\n", i);
        printf("index_count:       %10u\n", is->index_count);
        printf("index_size:        %10u bytes\n", is->index_size);
        if (is->raw_data == NULL || is->index_count == 0) {
            printf("(no decoded data)\n");
            continue;
        }
        uint32_t print_count = is->index_count < 12 ? is->index_count : 12;
        printf("  first %u indices:", print_count);
        for (uint32_t j = 0; j < print_count; j++) {
            uint32_t idx = 0;
            if (is->index_size == 2) {
                uint16_t v;
                memcpy(&v, is->raw_data + j * 2, 2);
                idx = v;
            } else {
                memcpy(&idx, is->raw_data + j * 4, 4);
            }
            printf(" %u", idx);
        }
        printf("\n");
    }
    return 0;
}

int wows_geometry_print(wows_geometry *geometry, bool verbose) {
    if (geometry == NULL) {
        printf("Invalid geometry: NULL pointer.\n");
        return WOWS_ERROR_UNKNOWN;
    }
    wows_geometry_header_print(geometry->header);
    wows_geometry_info_print(geometry->vertex_bloc_map, geometry->header->n_vertex_bloc, "Vertex Bloc Mapping");
    wows_geometry_info_print(geometry->index_bloc_map, geometry->header->n_index_bloc, "Index Bloc Mapping");
    wows_geometry_vertex_section_metadata_print(geometry->vertex_meta_sections, geometry->header->n_vertex_type,
                                                "Vertex Section Metadata");
    wows_geometry_index_section_metadata_print(geometry->index_meta_sections, geometry->header->n_index_type,
                                               "Index Section Metadata");
    if (geometry->vertexes) {
        wows_geometry_vertex_sections_print(geometry->vertexes, geometry->header->n_vertex_type,
                                            geometry->vertex_meta_sections, verbose);
    }
    if (geometry->indexes) {
        wows_geometry_index_sections_print(geometry->indexes, geometry->header->n_index_type);
    }
    return 0;
}
