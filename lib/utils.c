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

int vertex2id(const char *vertex_type) {
    if (strcmp(vertex_type, VER_UNKNOWN) == 0)
        return ID_UNKNOWN;
    if (strcmp(vertex_type, VER_SET3_XYNUV2IIIWWTBPC) == 0)
        return ID_SET3_XYNUV2IIIWWTBPC;
    if (strcmp(vertex_type, VER_SET3_XYNUV2TBIPC) == 0)
        return ID_SET3_XYNUV2TBIPC;
    if (strcmp(vertex_type, VER_SET3_XYNUV2TBPC) == 0)
        return ID_SET3_XYNUV2TBPC;
    if (strcmp(vertex_type, VER_SET3_XYNUVIIIWWPC) == 0)
        return ID_SET3_XYNUVIIIWWPC;
    if (strcmp(vertex_type, VER_SET3_XYNUVIIIWWR) == 0)
        return ID_SET3_XYNUVIIIWWR;
    if (strcmp(vertex_type, VER_SET3_XYNUVIIIWWTBPC) == 0)
        return ID_SET3_XYNUVIIIWWTBPC;
    if (strcmp(vertex_type, VER_SET3_XYNUVPC) == 0)
        return ID_SET3_XYNUVPC;
    if (strcmp(vertex_type, VER_SET3_XYNUVRPC) == 0)
        return ID_SET3_XYNUVRPC;
    if (strcmp(vertex_type, VER_SET3_XYNUVTBIPC) == 0)
        return ID_SET3_XYNUVTBIPC;
    if (strcmp(vertex_type, VER_SET3_XYNUVTBOI) == 0)
        return ID_SET3_XYNUVTBOI;
    if (strcmp(vertex_type, VER_SET3_XYNUVTBPC) == 0)
        return ID_SET3_XYNUVTBPC;
    return -1; // Invalid version string
}

const char *id2vertex(int id) {
    switch (id) {
    case ID_SET3_XYNUV2IIIWWTBPC:
        return VER_SET3_XYNUV2IIIWWTBPC;
    case ID_SET3_XYNUV2TBIPC:
        return VER_SET3_XYNUV2TBIPC;
    case ID_SET3_XYNUV2TBPC:
        return VER_SET3_XYNUV2TBPC;
    case ID_SET3_XYNUVIIIWWPC:
        return VER_SET3_XYNUVIIIWWPC;
    case ID_SET3_XYNUVIIIWWR:
        return VER_SET3_XYNUVIIIWWR;
    case ID_SET3_XYNUVIIIWWTBPC:
        return VER_SET3_XYNUVIIIWWTBPC;
    case ID_SET3_XYNUVPC:
        return VER_SET3_XYNUVPC;
    case ID_SET3_XYNUVRPC:
        return VER_SET3_XYNUVRPC;
    case ID_SET3_XYNUVTBIPC:
        return VER_SET3_XYNUVTBIPC;
    case ID_SET3_XYNUVTBOI:
        return VER_SET3_XYNUVTBOI;
    case ID_SET3_XYNUVTBPC:
        return VER_SET3_XYNUVTBPC;
    default:
        return VER_UNKNOWN;
    }
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

void normalise(float *x, float *y, float *z) {
    float length = sqrt((*x) * (*x) + (*y) * (*y) + (*z) * (*z));
    if (length > 0.0f) {
        *x /= length;
        *y /= length;
        *z /= length;
    }
}

float clamp(float min, float value, float max) {
    if (value < min)
        return min;
    if (value > max)
        return max;
    return value;
}

// Normals are weirldly packed in a single uint32
// X 11 bits, Y 11 bits, Z 10 bits.
// unpacked values should be between [-1, 1]
int wows_unpack_normal_old(wows_vertex *vertex_packed) {
    int32_t z = (int32_t)(vertex_packed->n) >> 22;
    int32_t y = (int32_t)(vertex_packed->n << 10) >> 21;
    int32_t x = (int32_t)(vertex_packed->n << 21) >> 21;

    vertex_packed->_nx = (float)(x) / 1023.f;
    vertex_packed->_ny = (float)(y) / 1023.f;
    vertex_packed->_nz = (float)(z) / 511.f;

    return 0;
}

int wows_pack_normal_old(wows_vertex *vertex_packed) {
    float nx = vertex_packed->_nx;
    float ny = vertex_packed->_ny;
    float nz = vertex_packed->_nz;

    normalise(&nx, &ny, &nz);

    nx = clamp(-1.0f, nx, 1.0f);
    ny = clamp(-1.0f, ny, 1.0f);
    nz = clamp(-1.0f, nz, 1.0f);

    vertex_packed->n = (((uint32_t)(nz * 511.0f) & 0x3ff) << 22) | (((uint32_t)(ny * 1023.0f) & 0x7ff) << 11) |
                       (((uint32_t)(nx * 1023.0f) & 0x7ff) << 0);
    return 0;
}
