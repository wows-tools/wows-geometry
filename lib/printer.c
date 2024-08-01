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
    printf("n_vertex_types:    %10u (0x%08x)\n", header->n_ver_type, header->n_ver_type);
    printf("n_index_types:     %10u (0x%08x)\n", header->n_ind_type, header->n_ind_type);
    printf("n_vertex_blocs:    %10u (0x%08x)\n", header->n_ver_bloc, header->n_ver_bloc);
    printf("n_index_blocs:     %10u (0x%08x)\n", header->n_ind_bloc, header->n_ind_bloc);
    printf("n_collision_blocs: %10u (0x%08x)\n", header->n_col_bloc, header->n_col_bloc);
    printf("n_armor_blocs:     %10u (0x%08x)\n", header->n_arm_bloc, header->n_arm_bloc);
    printf("off_sec_1:         %10lu (0x%08lx)\n", header->off_sec_1, header->off_sec_1);
    printf("off_unk_1:         %10lu (0x%08lx)\n", header->off_unk_1, header->off_unk_1);
    printf("off_unk_2:         %10lu (0x%08lx)\n", header->off_unk_2, header->off_unk_2);
    printf("n_unk_3:           %10lu (0x%08lx)\n", header->n_unk_3, header->n_unk_3);
    printf("n_col_unk_4:       %10lu (0x%08lx)\n", header->n_col_unk_4, header->n_col_unk_4);
    printf("n_arm_unk_5:       %10lu (0x%08lx)\n", header->n_arm_unk_5, header->n_arm_unk_5);
    return 0;
}

int wows_geometry_info_print(const wows_geometry_info *section, uint32_t count, const char *section_name) {
    for (uint32_t i = 0; i < count; i++) {
        printf("--------- %s - Entry %02u -----------\n", section_name, i);
        printf("id_unk_6:          %10u (0x%08x)\n", section[i].id_unk_6, section[i].id_unk_6);
        printf("type_unk_7:        %10u (0x%08x)\n", section[i].type_unk_7, section[i].type_unk_7);
        printf("id_unk_8:          %10u (0x%08x)\n", section[i].id_unk_8, section[i].id_unk_8);
        printf("n_unk_9:           %10u (0x%08x)\n", section[i].n_unk_9, section[i].n_unk_9);
        printf("n_unk_10:          %10u (0x%08x)\n", section[i].n_unk_10, section[i].n_unk_10);
    }
    return 0;
}

int wows_geometry_unk_1_print(const wows_geometry_unk_1 *section, uint32_t count, const char *section_name) {
    for (uint32_t i = 0; i < count; i++) {
        printf("--------- %s - Entry %02u -----------\n", section_name, i);
        printf("off_ver_bloc_start:%10lu (0x%08lx)\n", section[i].off_ver_bloc_start, section[i].off_ver_bloc_start);
        printf("n_size_type_str:   %10lu (0x%08lx)\n", section[i].n_size_type_str, section[i].n_size_type_str);
        printf("off_ver_bloc_end:  %10lu (0x%08lx)\n", section[i].off_ver_bloc_end, section[i].off_ver_bloc_end);
        printf("s_ver_bloc_size:   %10u (0x%08x)\n", section[i].s_ver_bloc_size, section[i].s_ver_bloc_size);
        printf("n_unk_5:           %10u (0x%08x)\n", section[i].n_unk_5, section[i].n_unk_5);
        printf("_abs_start:        %10lu (0x%08lx)\n", section[i]._abs_start, section[i]._abs_start);
        printf("_abs_end:          %10lu (0x%08lx)\n", section[i]._abs_end, section[i]._abs_end);
        printf("_vertex_type:      %23s\n", id2vertex(section[i]._vertex_type));
    }
    return 0;
}

int wows_geometry_print(wows_geometry *geometry) {
    if (geometry == NULL) {
        printf("Invalid geometry: NULL pointer.\n");
        return WOWS_ERROR_UNKNOWN;
    }
    wows_geometry_header_print(geometry->header);
    wows_geometry_info_print(geometry->section_1, geometry->header->n_ver_bloc, "Section 1");
    wows_geometry_info_print(geometry->section_2, geometry->header->n_ind_bloc, "Section 2");
    wows_geometry_unk_1_print(geometry->unk_1, geometry->header->n_ver_type, "Unknown 1");
    return 0;
}
