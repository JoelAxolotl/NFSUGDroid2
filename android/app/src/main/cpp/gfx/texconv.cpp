#include "gfx/texconv.h"

#include <cstring>

#include "gfx/d3d9_types.h"

namespace texconv {

using namespace d3d;

bool describe(uint32_t f, GLFormat& o) {
    o = GLFormat{};
    switch (f) {
        case FMT_A8R8G8B8: case FMT_X8R8G8B8: case FMT_A8B8G8R8: case FMT_X8B8G8R8:
        case FMT_X1R5G5B5: case FMT_A1R5G5B5: case FMT_A4R4G4B4: case FMT_A2R10G10B10:
        case FMT_R8G8B8: case FMT_G16R16:
        case FMT_DXT1: case FMT_DXT2: case FMT_DXT3: case FMT_DXT4: case FMT_DXT5:
            return true;  // converted to RGBA8
        case FMT_R5G6B5:
            o.internal = GL_RGB565;
            o.format = GL_RGB;
            o.type = GL_UNSIGNED_SHORT_5_6_5;
            o.direct = true;
            o.out_bpp = 2;
            return true;
        case FMT_L8:
            o.internal = GL_R8;
            o.format = GL_RED;
            o.direct = true;
            o.out_bpp = 1;
            o.swizzle[1] = o.swizzle[2] = GL_RED;
            o.swizzle[3] = GL_ONE;
            return true;
        case FMT_A8:
            o.internal = GL_R8;
            o.format = GL_RED;
            o.direct = true;
            o.out_bpp = 1;
            o.swizzle[0] = o.swizzle[1] = o.swizzle[2] = GL_ZERO;
            o.swizzle[3] = GL_RED;
            return true;
        case FMT_A8L8:
            o.internal = GL_RG8;
            o.format = GL_RG;
            o.direct = true;
            o.out_bpp = 2;
            o.swizzle[1] = o.swizzle[2] = GL_RED;
            o.swizzle[3] = GL_GREEN;
            return true;
        case FMT_V8U8:
            o.internal = GL_RG8_SNORM;
            o.format = GL_RG;
            o.type = GL_BYTE;
            o.direct = true;
            o.out_bpp = 2;
            o.swizzle[2] = o.swizzle[3] = GL_ONE;
            return true;
        case FMT_Q8W8V8U8:
            o.internal = GL_RGBA8_SNORM;
            o.type = GL_BYTE;
            o.direct = true;
            return true;
        case FMT_A16B16G16R16F:
            o.internal = GL_RGBA16F;
            o.type = GL_HALF_FLOAT;
            o.direct = true;
            o.out_bpp = 8;
            return true;
        case FMT_R32F:
            o.internal = GL_R32F;
            o.format = GL_RED;
            o.type = GL_FLOAT;
            o.direct = true;
            o.swizzle[1] = o.swizzle[2] = o.swizzle[3] = GL_ONE;
            return true;
        default:
            return false;
    }
}

namespace {
inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | (g << 8) | (b << 16) | (a << 24); }

void rgb565(uint16_t c, uint32_t& r, uint32_t& g, uint32_t& b) {
    r = ((c >> 11) & 31) * 255 / 31;
    g = ((c >> 5) & 63) * 255 / 63;
    b = (c & 31) * 255 / 31;
}

// Decodes one DXT colour block into 16 RGBA pixels. `dxt1` enables the
// 3-colour + transparent mode.
void color_block(const uint8_t* p, uint32_t out[16], bool dxt1) {
    uint16_t c0, c1;
    memcpy(&c0, p, 2);
    memcpy(&c1, p + 2, 2);
    uint32_t r[4], g[4], b[4], a[4] = {255, 255, 255, 255};
    rgb565(c0, r[0], g[0], b[0]);
    rgb565(c1, r[1], g[1], b[1]);
    if (!dxt1 || c0 > c1) {
        r[2] = (2 * r[0] + r[1]) / 3; g[2] = (2 * g[0] + g[1]) / 3; b[2] = (2 * b[0] + b[1]) / 3;
        r[3] = (r[0] + 2 * r[1]) / 3; g[3] = (g[0] + 2 * g[1]) / 3; b[3] = (b[0] + 2 * b[1]) / 3;
    } else {
        r[2] = (r[0] + r[1]) / 2; g[2] = (g[0] + g[1]) / 2; b[2] = (b[0] + b[1]) / 2;
        r[3] = g[3] = b[3] = 0;
        a[3] = 0;
    }
    uint32_t bits;
    memcpy(&bits, p + 4, 4);
    for (int i = 0; i < 16; ++i) {
        uint32_t k = (bits >> (i * 2)) & 3;
        out[i] = rgba(r[k], g[k], b[k], a[k]);
    }
}

void decode_dxt(uint32_t f, const uint8_t* src, uint32_t w, uint32_t h, uint32_t* dst) {
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    const bool dxt1 = f == FMT_DXT1;
    const uint32_t bsize = dxt1 ? 8 : 16;
    uint32_t px[16];
    for (uint32_t by = 0; by < bh; ++by) {
        for (uint32_t bx = 0; bx < bw; ++bx) {
            const uint8_t* blk = src + (by * bw + bx) * bsize;
            if (dxt1) {
                color_block(blk, px, true);
            } else {
                color_block(blk + 8, px, false);
                if (f == FMT_DXT2 || f == FMT_DXT3) {
                    for (int i = 0; i < 16; ++i) {
                        uint32_t a4 = (blk[i / 2] >> ((i & 1) * 4)) & 15;
                        px[i] = (px[i] & 0x00FFFFFF) | ((a4 * 17) << 24);
                    }
                } else {
                    uint32_t a[8] = {blk[0], blk[1]};
                    if (a[0] > a[1]) {
                        for (int i = 1; i < 7; ++i) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7;
                    } else {
                        for (int i = 1; i < 5; ++i) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5;
                        a[6] = 0;
                        a[7] = 255;
                    }
                    uint64_t bits = 0;
                    memcpy(&bits, blk + 2, 6);
                    for (int i = 0; i < 16; ++i)
                        px[i] = (px[i] & 0x00FFFFFF) | (a[(bits >> (i * 3)) & 7] << 24);
                }
            }
            for (uint32_t y = 0; y < 4; ++y) {
                uint32_t yy = by * 4 + y;
                if (yy >= h) break;
                for (uint32_t x = 0; x < 4; ++x) {
                    uint32_t xx = bx * 4 + x;
                    if (xx < w) dst[yy * w + xx] = px[y * 4 + x];
                }
            }
        }
    }
}
}  // namespace

void convert(uint32_t f, const uint8_t* src, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    GLFormat gf;
    describe(f, gf);
    const size_t n = (size_t)w * h;
    if (gf.direct) {
        out.assign(src, src + n * gf.out_bpp);
        return;
    }
    out.resize(n * 4);
    uint32_t* d = (uint32_t*)out.data();
    if (is_dxt(f)) {
        decode_dxt(f, src, w, h, d);
        return;
    }
    for (size_t i = 0; i < n; ++i) {
        uint32_t v, r, g, b, a;
        switch (f) {
            case FMT_A8R8G8B8:
            case FMT_X8R8G8B8:
                memcpy(&v, src + i * 4, 4);
                a = f == FMT_X8R8G8B8 ? 255 : v >> 24;
                d[i] = rgba((v >> 16) & 255, (v >> 8) & 255, v & 255, a);
                break;
            case FMT_A8B8G8R8:
            case FMT_X8B8G8R8:
                memcpy(&v, src + i * 4, 4);
                d[i] = f == FMT_X8B8G8R8 ? (v | 0xFF000000u) : v;
                break;
            case FMT_R8G8B8:
                d[i] = rgba(src[i * 3 + 2], src[i * 3 + 1], src[i * 3], 255);
                break;
            case FMT_X1R5G5B5:
            case FMT_A1R5G5B5: {
                uint16_t c;
                memcpy(&c, src + i * 2, 2);
                r = ((c >> 10) & 31) * 255 / 31;
                g = ((c >> 5) & 31) * 255 / 31;
                b = (c & 31) * 255 / 31;
                a = f == FMT_X1R5G5B5 || (c & 0x8000) ? 255 : 0;
                d[i] = rgba(r, g, b, a);
                break;
            }
            case FMT_A4R4G4B4: {
                uint16_t c;
                memcpy(&c, src + i * 2, 2);
                d[i] = rgba(((c >> 8) & 15) * 17, ((c >> 4) & 15) * 17, (c & 15) * 17, ((c >> 12) & 15) * 17);
                break;
            }
            case FMT_A2R10G10B10:
                memcpy(&v, src + i * 4, 4);
                d[i] = rgba(((v >> 20) & 1023) >> 2, ((v >> 10) & 1023) >> 2, (v & 1023) >> 2, (v >> 30) * 85);
                break;
            case FMT_G16R16:
                memcpy(&v, src + i * 4, 4);
                d[i] = rgba((v >> 8) & 255, (v >> 24) & 255, 255, 255);
                break;
            default:
                d[i] = 0xFFFF00FFu;  // magenta: unsupported
                break;
        }
    }
}

}  // namespace texconv
