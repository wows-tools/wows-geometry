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
    if (strcmp(vertex_type, WOWS_VER_UNKNOWN) == 0)
        return WOWS_ID_UNKNOWN;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUV2IIIWWTBPC) == 0)
        return WOWS_ID_SET3_XYNUV2IIIWWTBPC;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUV2TBIPC) == 0)
        return WOWS_ID_SET3_XYNUV2TBIPC;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUV2TBPC) == 0)
        return WOWS_ID_SET3_XYNUV2TBPC;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUVIIIWWPC) == 0)
        return WOWS_ID_SET3_XYNUVIIIWWPC;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUVIIIWWR) == 0)
        return WOWS_ID_SET3_XYNUVIIIWWR;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUVIIIWWTBPC) == 0)
        return WOWS_ID_SET3_XYNUVIIIWWTBPC;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUVPC) == 0)
        return WOWS_ID_SET3_XYNUVPC;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUVRPC) == 0)
        return WOWS_ID_SET3_XYNUVRPC;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUVTBIPC) == 0)
        return WOWS_ID_SET3_XYNUVTBIPC;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUVTBOI) == 0)
        return WOWS_ID_SET3_XYNUVTBOI;
    if (strcmp(vertex_type, WOWS_VER_SET3_XYNUVTBPC) == 0)
        return WOWS_ID_SET3_XYNUVTBPC;
    return -1; // Invalid version string
}

const char *id2vertex(int id) {
    switch (id) {
    case WOWS_ID_SET3_XYNUV2IIIWWTBPC:
        return WOWS_VER_SET3_XYNUV2IIIWWTBPC;
    case WOWS_ID_SET3_XYNUV2TBIPC:
        return WOWS_VER_SET3_XYNUV2TBIPC;
    case WOWS_ID_SET3_XYNUV2TBPC:
        return WOWS_VER_SET3_XYNUV2TBPC;
    case WOWS_ID_SET3_XYNUVIIIWWPC:
        return WOWS_VER_SET3_XYNUVIIIWWPC;
    case WOWS_ID_SET3_XYNUVIIIWWR:
        return WOWS_VER_SET3_XYNUVIIIWWR;
    case WOWS_ID_SET3_XYNUVIIIWWTBPC:
        return WOWS_VER_SET3_XYNUVIIIWWTBPC;
    case WOWS_ID_SET3_XYNUVPC:
        return WOWS_VER_SET3_XYNUVPC;
    case WOWS_ID_SET3_XYNUVRPC:
        return WOWS_VER_SET3_XYNUVRPC;
    case WOWS_ID_SET3_XYNUVTBIPC:
        return WOWS_VER_SET3_XYNUVTBIPC;
    case WOWS_ID_SET3_XYNUVTBOI:
        return WOWS_VER_SET3_XYNUVTBOI;
    case WOWS_ID_SET3_XYNUVTBPC:
        return WOWS_VER_SET3_XYNUVTBPC;
    default:
        return WOWS_VER_UNKNOWN;
    }
}

uint8_t geom_datatoh8(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context) {
    // Endianness doesn't matter for 8-bit values, simply return the byte at the given offset
    return (uint8_t)data[offset];
}

uint16_t geom_datatoh16(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context) {
    uint16_t *ret = (uint16_t *)(data + offset);
    if (context->is_le) {
        return le16toh(*ret);
    } else {
        return be16toh(*ret);
    }
}

uint32_t geom_datatoh32(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context) {
    uint32_t *ret = (uint32_t *)(data + offset);
    if (context->is_le) {
        return le32toh(*ret);
    } else {
        return be32toh(*ret);
    }
}

uint64_t geom_datatoh64(char *data, size_t offset, WOWS_GEOMETRY_CONTEXT *context) {
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

// Convert IEEE 754 float16 to float32.
float f16_to_f32(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31;
    uint32_t e = (h >> 10) & 0x1fu;
    uint32_t m = h & 0x3ffu;
    uint32_t f;

    if (e == 0) {
        if (m == 0) {
            f = s;
        } else {
            e = 1;
            while (!(m & 0x400u)) {
                m <<= 1;
                e--;
            }
            m &= 0x3ffu;
            f = s | ((e + 112u) << 23) | (m << 13);
        }
    } else if (e == 31) {
        f = s | 0x7f800000u | (m << 13);
    } else {
        f = s | ((e + 112u) << 23) | (m << 13);
    }

    float result;
    memcpy(&result, &f, sizeof(f));
    return result;
}

// Unpack 4-byte packed normal: 4 signed bytes each mapped [-127,127] -> [-1,1].
void wows_unpack_normal(uint32_t packed, float *nx, float *ny, float *nz) {
    int8_t bytes[4];
    memcpy(bytes, &packed, 4);
    *nx = bytes[0] / 127.0f;
    *ny = bytes[1] / 127.0f;
    *nz = bytes[2] / 127.0f;
}

// Unpack 4-byte packed UV: 2 x float16 stored as actual_uv - 0.5.
void wows_unpack_uv(uint32_t packed, float *u, float *v) {
    uint16_t u_bits, v_bits;
    memcpy(&u_bits, (uint8_t *)&packed + 0, 2);
    memcpy(&v_bits, (uint8_t *)&packed + 2, 2);
    *u = f16_to_f32(u_bits) + 0.5f;
    *v = f16_to_f32(v_bits) + 0.5f;
}
