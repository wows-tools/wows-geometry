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

#if defined(MSDOS) || defined(OS2) || defined(WIN32) || defined(__CYGWIN__)
#include <fcntl.h>
#include <io.h>
#define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#define SET_BINARY_MODE(file)
#endif

#include "wows-geometry.h"
#include "hashmap.h"

void wows_geometry_header_print(const wows_geometry_header *header) {
    if (header == NULL) {
        printf("Invalid header: NULL pointer.\n");
        return;
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
}

void wows_geometry_info_print(const wows_geometry_info *section, uint32_t count, const char *section_name) {
    for (uint32_t i = 0; i < count; i++) {
        printf("--------- %s - Entry %02u -----------\n", section_name, i);
        printf("id_unk_6:          %10u (0x%08x)\n", section[i].id_unk_6, section[i].id_unk_6);
        printf("type_unk_7:        %10u (0x%08x)\n", section[i].type_unk_7, section[i].type_unk_7);
        printf("id_unk_8:          %10u (0x%08x)\n", section[i].id_unk_8, section[i].id_unk_8);
        printf("n_unk_9:           %10u (0x%08x)\n", section[i].n_unk_9, section[i].n_unk_9);
        printf("n_unk_10:          %10u (0x%08x)\n", section[i].n_unk_10, section[i].n_unk_10);
    }
}

void wows_geometry_unk_1_print(const wows_geometry_unk_1 *section, uint32_t count, const char *section_name) {
    for (uint32_t i = 0; i < count; i++) {
        printf("--------- %s - Entry %02u -----------\n", section_name, i);
        printf("off_ver_bloc:      %10lu (0x%016lx)\n", section[i].off_ver_bloc, section[i].off_ver_bloc);
        printf("n_size_type_str:   %10lu (0x%016lx)\n", section[i].n_size_type_str, section[i].n_size_type_str);
        printf("n_unk_3:           %10lu (0x%016lx)\n", section[i].n_unk_3, section[i].n_unk_3);
        printf("n_unk_4:           %10u (0x%08x)\n", section[i].n_unk_4, section[i].n_unk_4);
        printf("n_unk_5:           %10u (0x%08x)\n", section[i].n_unk_5, section[i].n_unk_5);
    }
}

void wows_geometry_print(wows_geometry *geometry) {
    if (geometry == NULL) {
        printf("Invalid geometry: NULL pointer.\n");
        return;
    }
    wows_geometry_header_print(geometry->header);
    wows_geometry_info_print(geometry->section_1, geometry->header->n_ver_bloc, "Section 1");
    wows_geometry_info_print(geometry->section_2, geometry->header->n_ind_bloc, "Section 2");
    wows_geometry_unk_1_print(geometry->unk_1, geometry->header->n_ver_type, "Unknown 1");
}

// Context init function
WOWS_GEOMETRY_CONTEXT *wows_init_geometry_context(uint8_t debug_level) {
    WOWS_GEOMETRY_CONTEXT *context = calloc(sizeof(WOWS_GEOMETRY_CONTEXT), 1);
    context->debug_level = debug_level;
    context->is_le = true;
    return context;
}

uint16_t datatoh16(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context) {
    uint16_t *ret = (uint16_t *)(data + offset);
    if (context->is_le) {
        return le16toh(*ret);
    } else {
        return be16toh(*ret);
    }
}

uint32_t datatoh32(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context) {
    uint32_t *ret = (uint32_t *)(data + offset);
    if (context->is_le) {
        return le32toh(*ret);
    } else {
        return be32toh(*ret);
    }
}

uint64_t datatoh64(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context) {
    uint64_t *ret = (uint64_t *)(data + offset);
    if (context->is_le) {
        return le64toh(*ret);
    } else {
        return be64toh(*ret);
    }
}

int wows_parse_geometry_buffer(char *contents, size_t length, wows_geometry **geometry_content) {
    // TODO FIXME add size control

    WOWS_GEOMETRY_CONTEXT *context = wows_init_geometry_context(10);
    wows_geometry *geometry = calloc(sizeof(wows_geometry), 1);

    // parsing the header
    wows_geometry_header *header = calloc(sizeof(wows_geometry_header), 1);
    header->n_ver_type = datatoh32(contents, 0, context);
    header->n_ind_type = datatoh32(contents, 4, context);
    header->n_ver_bloc = datatoh32(contents, 8, context);
    header->n_ind_bloc = datatoh32(contents, 12, context);
    header->n_col_bloc = datatoh32(contents, 16, context);
    header->n_arm_bloc = datatoh32(contents, 20, context);
    header->off_sec_1 = datatoh64(contents, 24, context);
    header->off_unk_1 = datatoh64(contents, 32, context);
    header->off_unk_2 = datatoh64(contents, 40, context);
    header->n_unk_3 = datatoh64(contents, 48, context);
    header->n_col_unk_4 = datatoh64(contents, 56, context);
    header->n_arm_unk_5 = datatoh64(contents, 64, context);
    geometry->header = header;

    // Parsing the Info Section 1
    wows_geometry_info *section_1 = calloc(sizeof(wows_geometry_info), header->n_ver_bloc);
    geometry->section_1 = section_1;
    contents += header->off_sec_1;

    for (int i = 0; i < header->n_ver_bloc; i++) {
        section_1[i].id_unk_6 = datatoh32(contents, i * WOWS_BLOC_INFO_SIZE, context);
        section_1[i].type_unk_7 = datatoh16(contents, i * WOWS_BLOC_INFO_SIZE + 4, context);
        section_1[i].id_unk_8 = datatoh16(contents, i * WOWS_BLOC_INFO_SIZE + 6, context);
        section_1[i].n_unk_9 = datatoh32(contents, i * WOWS_BLOC_INFO_SIZE + 8, context);
        section_1[i].n_unk_10 = datatoh32(contents, i * WOWS_BLOC_INFO_SIZE + 12, context);
    }

    // Parsing the Info Section 1
    contents += header->n_ver_bloc * WOWS_BLOC_INFO_SIZE; // Move to the next section
    wows_geometry_info *section_2 = calloc(sizeof(wows_geometry_info), header->n_ind_bloc);
    geometry->section_2 = section_2;

    for (int i = 0; i < header->n_ind_bloc; i++) {
        section_2[i].id_unk_6 = datatoh32(contents, i * WOWS_BLOC_INFO_SIZE, context);
        section_2[i].type_unk_7 = datatoh16(contents, i * WOWS_BLOC_INFO_SIZE + 4, context);
        section_2[i].id_unk_8 = datatoh16(contents, i * WOWS_BLOC_INFO_SIZE + 6, context);
        section_2[i].n_unk_9 = datatoh32(contents, i * WOWS_BLOC_INFO_SIZE + 8, context);
        section_2[i].n_unk_10 = datatoh32(contents, i * WOWS_BLOC_INFO_SIZE + 12, context);
    }

    // Parsing the Unknown_1 section
    contents += header->n_ver_bloc * WOWS_BLOC_INFO_SIZE; // Move to the next section

    wows_geometry_unk_1 *unk_1 = calloc(sizeof(wows_geometry_unk_1), header->n_ver_type);
    geometry->unk_1 = unk_1;
    for (int i = 0; i < header->n_ver_type; i++) {
        unk_1[i].off_ver_bloc = datatoh64(contents, i * WOWS_UNK_1_SIZE + 0, context);
        unk_1[i].n_size_type_str = datatoh64(contents, i * WOWS_UNK_1_SIZE + 8, context);
        unk_1[i].n_unk_3 = datatoh64(contents, i * WOWS_UNK_1_SIZE + 16, context);
        unk_1[i].n_unk_4 = datatoh32(contents, i * WOWS_UNK_1_SIZE + 24, context);
        unk_1[i].n_unk_5 = datatoh32(contents, i * WOWS_UNK_1_SIZE + 28, context);
    }

    contents += header->n_ver_type * WOWS_UNK_1_SIZE; // Move to the next section

    *geometry_content = geometry;
    return 0;
}

int wows_parse_geometry(char *input, wows_geometry **geometry_content) {
    int ret = 0;

    // Open the index file
    int fd = open(input, O_RDONLY);
    ;
    if (fd <= 0) {
        return -1;
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

int wows_geometry_free(wows_geometry *geometry) {
    free(geometry->header);
    free(geometry->section_1);
    free(geometry->section_2);
    free(geometry->unk_1);
    free(geometry);
    return 0;
}
