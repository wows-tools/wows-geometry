#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize2.h>

#include "stitch.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

/* ── DDS BC decoder ─────────────────────────────────────────────── */

static void rgb565(uint16_t c, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t rv = (c >> 11) & 0x1f;
    *r = (rv << 3) | (rv >> 2);
    uint8_t gv = (c >> 5) & 0x3f;
    *g = (gv << 2) | (gv >> 4);
    uint8_t bv = c & 0x1f;
    *b = (bv << 3) | (bv >> 2);
}

static void bc1_block(const uint8_t *src, uint8_t tmp[64]) {
    uint16_t c0, c1;
    memcpy(&c0, src, 2);
    memcpy(&c1, src + 2, 2);
    uint32_t ix;
    memcpy(&ix, src + 4, 4);
    uint8_t r[4], g[4], b[4];
    r[3] = g[3] = b[3] = 0;
    rgb565(c0, &r[0], &g[0], &b[0]);
    rgb565(c1, &r[1], &g[1], &b[1]);
    if (c0 > c1) {
        r[2] = (2 * r[0] + r[1] + 1) / 3;
        g[2] = (2 * g[0] + g[1] + 1) / 3;
        b[2] = (2 * b[0] + b[1] + 1) / 3;
        r[3] = (r[0] + 2 * r[1] + 1) / 3;
        g[3] = (g[0] + 2 * g[1] + 1) / 3;
        b[3] = (b[0] + 2 * b[1] + 1) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2;
        g[2] = (g[0] + g[1]) / 2;
        b[2] = (b[0] + b[1]) / 2;
    }
    for (int i = 0; i < 16; ++i) {
        int k = (ix >> (i * 2)) & 3;
        tmp[i * 4] = r[k];
        tmp[i * 4 + 1] = g[k];
        tmp[i * 4 + 2] = b[k];
        tmp[i * 4 + 3] = 255;
    }
}

static void bc4_block(const uint8_t *src, uint8_t av[16]) {
    uint8_t a0 = src[0], a1 = src[1];
    uint8_t t[8];
    t[0] = a0;
    t[1] = a1;
    if (a0 > a1) {
        t[2] = (6 * a0 + a1 + 3) / 7;
        t[3] = (5 * a0 + 2 * a1 + 3) / 7;
        t[4] = (4 * a0 + 3 * a1 + 3) / 7;
        t[5] = (3 * a0 + 4 * a1 + 3) / 7;
        t[6] = (2 * a0 + 5 * a1 + 3) / 7;
        t[7] = (a0 + 6 * a1 + 3) / 7;
    } else {
        t[2] = (4 * a0 + a1 + 2) / 5;
        t[3] = (3 * a0 + 2 * a1 + 2) / 5;
        t[4] = (2 * a0 + 3 * a1 + 2) / 5;
        t[5] = (a0 + 4 * a1 + 2) / 5;
        t[6] = 0;
        t[7] = 255;
    }
    uint64_t bits = 0;
    memcpy(&bits, src + 2, 6);
    for (int i = 0; i < 16; ++i)
        av[i] = t[(bits >> (i * 3)) & 7];
}

enum DdsFmt { DDS_NONE, DDS_BC1, DDS_BC2, DDS_BC3, DDS_BC4, DDS_BC5 };

std::vector<uint8_t> stitch_decode_dds(const uint8_t *d, size_t sz, int *W, int *H) {
    if (sz < 128 || memcmp(d, "DDS ", 4) != 0)
        return {};
    uint32_t h, w, fcc;
    memcpy(&h, d + 12, 4);
    memcpy(&w, d + 16, 4);
    memcpy(&fcc, d + 84, 4);
    size_t off = 128;
    DdsFmt fmt = DDS_NONE;
    static const uint32_t DXT1 = 0x31545844, DXT3 = 0x33545844, DXT5 = 0x35545844, DXT2 = 0x32545844, DXT4 = 0x34545844,
                          ATI1 = 0x31495441, BC4U = 0x55344342, ATI2 = 0x32495441, BC5U = 0x55354342, DX10 = 0x30315844;
    if (fcc == DX10) {
        if (sz < 148)
            return {};
        uint32_t dxgi;
        memcpy(&dxgi, d + 128, 4);
        off = 148;
        switch (dxgi) {
        case 71:
        case 72:
            fmt = DDS_BC1;
            break;
        case 74:
            fmt = DDS_BC2;
            break;
        case 77:
        case 78:
            fmt = DDS_BC3;
            break;
        case 80:
        case 81:
            fmt = DDS_BC4;
            break;
        case 83:
        case 84:
            fmt = DDS_BC5;
            break;
        default:
            return {};
        }
    } else {
        if (fcc == DXT1)
            fmt = DDS_BC1;
        else if (fcc == DXT2 || fcc == DXT3)
            fmt = DDS_BC2;
        else if (fcc == DXT4 || fcc == DXT5)
            fmt = DDS_BC3;
        else if (fcc == ATI1 || fcc == BC4U)
            fmt = DDS_BC4;
        else if (fcc == ATI2 || fcc == BC5U)
            fmt = DDS_BC5;
        else
            return {};
    }
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int bs = (fmt == DDS_BC1 || fmt == DDS_BC4) ? 8 : 16;
    if (sz < off + (size_t)bw * bh * bs)
        return {};
    *W = (int)w;
    *H = (int)h;
    std::vector<uint8_t> rgba(w * h * 4, 255);
    const uint8_t *src = d + off;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            int px = bx * 4, py = by * 4;
            int cx = std::min(4, (int)w - px), cy = std::min(4, (int)h - py);
            uint8_t tmp[64] = {};
            switch (fmt) {
            case DDS_BC1:
                bc1_block(src, tmp);
                break;
            case DDS_BC2: {
                uint8_t cb[16];
                bc1_block(src + 8, tmp);
                for (int i = 0; i < 8; ++i) {
                    cb[i * 2] = (src[i] & 0xF) * 17;
                    cb[i * 2 + 1] = (src[i] >> 4) * 17;
                }
                for (int i = 0; i < 16; ++i)
                    tmp[i * 4 + 3] = 255;
                break;
            }
            case DDS_BC3: {
                uint8_t ab[16];
                bc4_block(src, ab);
                bc1_block(src + 8, tmp);
                for (int i = 0; i < 16; ++i)
                    tmp[i * 4 + 3] = 255;
                break;
            }
            case DDS_BC4: {
                uint8_t ab[16];
                bc4_block(src, ab);
                for (int i = 0; i < 16; ++i) {
                    tmp[i * 4] = tmp[i * 4 + 1] = tmp[i * 4 + 2] = ab[i];
                    tmp[i * 4 + 3] = 255;
                }
                break;
            }
            case DDS_BC5: {
                uint8_t rb[16], gb[16];
                bc4_block(src, rb);
                bc4_block(src + 8, gb);
                for (int i = 0; i < 16; ++i) {
                    tmp[i * 4] = rb[i];
                    tmp[i * 4 + 1] = gb[i];
                    tmp[i * 4 + 2] = 0;
                    tmp[i * 4 + 3] = 255;
                }
                break;
            }
            default:
                break;
            }
            for (int ty = 0; ty < cy; ++ty)
                memcpy(rgba.data() + (py + ty) * (int)w * 4 + px * 4, tmp + ty * 16, cx * 4);
            src += bs;
        }
    return rgba;
}

std::vector<uint8_t> stitch_dds_to_png(const std::string &path, int max_sz) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(f);
        return {};
    }
    std::vector<uint8_t> raw((size_t)sz);
    bool ok = fread(raw.data(), 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    if (!ok)
        return {};
    int w, h;
    std::vector<uint8_t> rgba = stitch_decode_dds(raw.data(), raw.size(), &w, &h);
    if (rgba.empty())
        return {};
    if (max_sz > 0 && (w > max_sz || h > max_sz)) {
        float s = (float)max_sz / std::max(w, h);
        int nw = std::max(1, (int)(w * s)), nh = std::max(1, (int)(h * s));
        std::vector<uint8_t> rs((size_t)nw * nh * 4);
        stbir_resize_uint8_srgb(rgba.data(), w, h, 0, rs.data(), nw, nh, 0, STBIR_RGBA);
        rgba = std::move(rs);
        w = nw;
        h = nh;
    }
    bool has_alpha = false;
    for (int i = 3, n = w * h * 4; i < n; i += 4)
        if (rgba[i] != 255) {
            has_alpha = true;
            break;
        }
    std::vector<uint8_t> rgb3;
    const uint8_t *pix = rgba.data();
    int channels = 4, stride = w * 4;
    if (!has_alpha) {
        rgb3.resize((size_t)w * h * 3);
        for (int i = 0; i < w * h; i++) {
            rgb3[i * 3 + 0] = rgba[i * 4 + 0];
            rgb3[i * 3 + 1] = rgba[i * 4 + 1];
            rgb3[i * 3 + 2] = rgba[i * 4 + 2];
        }
        pix = rgb3.data();
        channels = 3;
        stride = w * 3;
    }
    std::vector<uint8_t> png;
    stbi_write_png_to_func(
        [](void *ctx, void *data, int n) {
            auto *v = (std::vector<uint8_t> *)ctx;
            const auto *p = (const uint8_t *)data;
            v->insert(v->end(), p, p + n);
        },
        &png, w, h, channels, pix, stride);
    return png;
}
