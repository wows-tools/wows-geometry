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
    contents += header->n_ver_bloc * WOWS_BLOC_INFO_SIZE;
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
    contents += header->n_ver_bloc * WOWS_BLOC_INFO_SIZE;

    wows_geometry_unk_1 *unk_1 = calloc(sizeof(wows_geometry_unk_1), header->n_ver_type);
    geometry->unk_1 = unk_1;
    for (int i = 0; i < header->n_ver_type; i++) {
        unk_1[i].off_ver_bloc_start = datatoh64(contents, i * WOWS_UNK_1_SIZE + 0, context);
        unk_1[i].n_size_type_str = datatoh64(contents, i * WOWS_UNK_1_SIZE + 8, context);
        unk_1[i].off_ver_bloc_end = datatoh64(contents, i * WOWS_UNK_1_SIZE + 16, context);
        unk_1[i].s_ver_bloc_size = datatoh32(contents, i * WOWS_UNK_1_SIZE + 24, context);
        unk_1[i].n_unk_5 = datatoh32(contents, i * WOWS_UNK_1_SIZE + 28, context);

        // Record absolute offset for conviniance
        unk_1[i]._abs_start = contents + i * WOWS_UNK_1_SIZE - start + unk_1[i].off_ver_bloc_start;
        unk_1[i]._abs_end = contents + i * WOWS_UNK_1_SIZE - start + unk_1[i].off_ver_bloc_end + 8;
        unk_1[i]._vertex_type = vertex2id(start + unk_1[i]._abs_end);
    }

    contents += header->n_ver_type * WOWS_UNK_1_SIZE;

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

int wows_geometry_free(wows_geometry *geometry) {
    free(geometry->header);
    free(geometry->section_1);
    free(geometry->section_2);
    free(geometry->unk_1);
    free(geometry);
    return 0;
}
