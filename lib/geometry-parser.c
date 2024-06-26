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
    printf("wows_geometry_header values:\n");
    printf("n_vertex_types:    %u\n", header->n_ver_type);
    printf("n_index_types:     %u\n", header->n_ind_type);
    printf("n_vertex_blocs:    %u\n", header->n_ver_bloc);
    printf("n_index_blocs:     %u\n", header->n_ind_bloc);
    printf("n_collision_blocs: %u\n", header->n_col_bloc);
    printf("n_armor_blocs:     %u\n", header->n_arm_bloc);
}

void wows_geometry_print(wows_geometry *geometry) {
    if (geometry == NULL) {
        printf("Invalid geometry: NULL pointer.\n");
        return;
    }
    wows_geometry_header_print(geometry->header);
}

// Context init function
WOWS_GEOMETRY_CONTEXT *wows_init_geometry_context(uint8_t debug_level) {
    WOWS_GEOMETRY_CONTEXT *context = calloc(sizeof(WOWS_GEOMETRY_CONTEXT), 1);
    context->debug_level = debug_level;
    context->is_le = true;
    return context;
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
    // TODO error handling in case the file is sketchy

    WOWS_GEOMETRY_CONTEXT *context = wows_init_geometry_context(10);
    wows_geometry *geometry = calloc(sizeof(wows_geometry), 1);
    wows_geometry_header *header = calloc(sizeof(wows_geometry_header), 1);
    header->n_ver_type = datatoh32(contents, 0, context);
    header->n_ind_type = datatoh32(contents, 4, context);
    header->n_ver_bloc = datatoh32(contents, 8, context);
    header->n_ind_bloc = datatoh32(contents, 12, context);
    header->n_col_bloc = datatoh32(contents, 16, context);
    header->n_arm_bloc = datatoh32(contents, 20, context);

    geometry->header = header;
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
    free(geometry);
    return 0;
}
