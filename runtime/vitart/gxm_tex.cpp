// Decodificação de SceGxmTexture para RGBA8. Porte direto de tools/gxm_tex_dump.py (a referência
// conferida nas texturas do jogo): swizzled = ordem Z com o lado maior em linhas de quadrados; BCn
// swizzled = blocos 4x4 em ordem Z; tiled = ladrilhos 32x32; linear = linhas alinhadas a 8 texels.
#include "gxm_tex.h"

#include <cstring>

namespace gxmtex {
namespace {

uint32_t bits(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }

uint32_t pow2(uint32_t n) {
    uint32_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

uint32_t part1by1(uint32_t x) {
    x &= 0x0000FFFF;
    x = (x | (x << 8)) & 0x00FF00FF;
    x = (x | (x << 4)) & 0x0F0F0F0F;
    x = (x | (x << 2)) & 0x33333333;
    x = (x | (x << 1)) & 0x55555555;
    return x;
}

// Índice na memória de (x, y) numa grade w x h (potências de 2) em ordem Z do Vita.
uint32_t zorder(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    const uint32_t m = w < h ? w : h;
    uint32_t k = 0;
    while ((1u << (k + 1)) <= m) k++;
    return (((x >> k) | (y >> k)) << (2 * k)) | (part1by1(x & (m - 1)) << 1) | part1by1(y & (m - 1));
}

void rgb565(uint32_t c, int out[3]) {
    out[0] = (int)((c >> 11) & 31) * 255 / 31;
    out[1] = (int)((c >> 5) & 63) * 255 / 63;
    out[2] = (int)(c & 31) * 255 / 31;
}

// 16 texels RGBA de um bloco BC1 (8 bytes); alpha_mode = BC1 com alfa de 1 bit.
void bc1(const uint8_t *b, bool alpha_mode, uint8_t px[16][4]) {
    const uint32_t c0 = b[0] | (b[1] << 8), c1 = b[2] | (b[3] << 8);
    int p[4][3];
    rgb565(c0, p[0]);
    rgb565(c1, p[1]);
    const bool four = (c0 > c1) || !alpha_mode;
    for (int c = 0; c < 3; c++) {
        p[2][c] = four ? (2 * p[0][c] + p[1][c]) / 3 : (p[0][c] + p[1][c]) / 2;
        p[3][c] = four ? (p[0][c] + 2 * p[1][c]) / 3 : 0;
    }
    for (int i = 0; i < 16; i++) {
        const int s = (b[4 + i / 4] >> (2 * (i % 4))) & 3;
        px[i][0] = (uint8_t)p[s][0], px[i][1] = (uint8_t)p[s][1], px[i][2] = (uint8_t)p[s][2];
        px[i][3] = (alpha_mode && !four && s == 3) ? 0 : 255;
    }
}

void bc3_alpha(const uint8_t *b, uint8_t px[16][4]) {
    const int a0 = b[0], a1 = b[1];
    int pal[8] = {a0, a1};
    if (a0 > a1) {
        for (int i = 1; i < 7; i++) pal[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (int i = 1; i < 5; i++) pal[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        pal[6] = 0, pal[7] = 255;
    }
    uint64_t v = 0;
    for (int i = 0; i < 6; i++) v |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)pal[(v >> (3 * i)) & 7];
}

void swizzle4(const uint8_t *in, uint32_t swz, uint8_t *out) {
    // SceGxmTextureSwizzle4Mode: 0 ABGR, 1 ARGB, 2 RGBA, 3 BGRA, 4 1BGR, 5 1RGB, 6 RGB1, 7 BGR1.
    static const int order[8][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {1, 2, 3, 0},
                                    {0, 1, 2, -1}, {2, 1, 0, -1}, {3, 2, 1, -1}, {1, 2, 3, -1}};
    const int *o = order[swz & 7];
    out[0] = in[o[0]], out[1] = in[o[1]], out[2] = in[o[2]];
    out[3] = o[3] < 0 ? 255 : in[o[3]];
}

} // namespace

Info describe(const uint32_t w[4]) {
    Info t;
    t.type = bits(w[1], 29, 3);
    t.base = (bits(w[0], 31, 1) << 31) | (bits(w[1], 24, 5) << 24);
    t.swizzle = bits(w[3], 28, 3);
    if (t.type == 0 || t.type == 1) {
        t.width = 1u << bits(w[1], 16, 4), t.height = 1u << bits(w[1], 0, 4);
    } else {
        t.width = bits(w[1], 12, 12) + 1, t.height = bits(w[1], 0, 12) + 1;
    }
    if (t.type == 6) {
        const uint32_t sc = bits(w[0], 9, 1) | (bits(w[0], 10, 2) << 1) | (bits(w[0], 17, 4) << 3) | (bits(w[0], 21, 6) << 7);
        t.stride = (sc + 1) * 4;
    }
    t.data = w[2] & ~3u;
    t.palette = bits(w[3], 0, 26) << 6;
    return t;
}

size_t data_size(const Info &t) {
    const uint32_t w = t.width, h = t.height, ty = t.type, fmt = t.base;
    if (!w || !h || w > 4096 || h > 4096) return 0;
    const bool swizzled = ty == 0 || ty == 1 || ty == 5 || ty == 7;
    if (fmt == 0x85000000u || fmt == 0x86000000u || fmt == 0x87000000u) {
        const size_t bw = (w + 3) / 4, bh = (h + 3) / 4, bsize = fmt == 0x85000000u ? 8 : 16;
        return (swizzled ? (size_t)pow2((uint32_t)bw) * pow2((uint32_t)bh) : bw * bh) * bsize;
    }
    size_t bpp;
    switch (fmt) {
    case 0x0C000000u: bpp = 4; break;
    case 0x95000000u: case 0x00000000u: bpp = 1; break;
    case 0x07000000u: bpp = 2; break;
    default: return 0;
    }
    if (swizzled) return (size_t)pow2(w) * pow2(h) * bpp;
    if (ty == 4) return (size_t)((w + 31) / 32) * ((h + 31) / 32) * 1024 * bpp;
    return (size_t)(ty == 6 ? t.stride : ((w + 7) & ~7u) * bpp) * (h - 1) + (size_t)w * bpp;
}

bool decode(const uint8_t *guest, const Info &t, std::vector<uint8_t> &rgba) {
    const uint32_t w = t.width, h = t.height, ty = t.type, fmt = t.base;
    if (!w || !h || w > 4096 || h > 4096) return false;
    const bool swizzled = ty == 0 || ty == 1 || ty == 5 || ty == 7;
    rgba.assign((size_t)w * h * 4, 255);
    const uint8_t *src = guest + t.data;
    if (fmt == 0x85000000u || fmt == 0x86000000u || fmt == 0x87000000u) {
        const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bsize = fmt == 0x85000000u ? 8 : 16;
        const uint32_t pw = pow2(bw), ph = pow2(bh);
        uint8_t px[16][4];
        for (uint32_t by = 0; by < bh; by++)
            for (uint32_t bx = 0; bx < bw; bx++) {
                const uint8_t *b = src + (size_t)(swizzled ? zorder(bx, by, pw, ph) : by * bw + bx) * bsize;
                if (fmt == 0x85000000u) {
                    bc1(b, true, px);
                } else {
                    bc1(b + 8, false, px);
                    if (fmt == 0x86000000u)
                        for (int i = 0; i < 16; i++) px[i][3] = (uint8_t)(((b[i / 2] >> (4 * (i % 2))) & 15) * 17);
                    else
                        bc3_alpha(b, px);
                }
                for (int i = 0; i < 16; i++) {
                    const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x < w && y < h) std::memcpy(&rgba[((size_t)y * w + x) * 4], px[i], 4);
                }
            }
        return true;
    }
    uint32_t bpp;
    switch (fmt) {
    case 0x0C000000u: bpp = 4; break; // U8U8U8U8
    case 0x95000000u: bpp = 1; break; // P8
    case 0x00000000u: bpp = 1; break; // U8
    case 0x07000000u: bpp = 2; break; // U8U8
    default: return false;
    }
    const uint32_t pw = pow2(w), ph = pow2(h), tw = (w + 31) / 32;
    const uint32_t stride = ty == 6 ? t.stride : ((w + 7) & ~7u) * bpp;
    const uint8_t *pal = fmt == 0x95000000u ? guest + t.palette : nullptr;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            size_t off;
            if (swizzled)
                off = (size_t)zorder(x, y, pw, ph) * bpp;
            else if (ty == 4)
                off = (size_t)(((((x >> 5) + tw * (y >> 5)) << 10) | (x & 31) | ((y & 31) << 5))) * bpp;
            else
                off = (size_t)y * stride + (size_t)x * bpp;
            const uint8_t *s = src + off;
            uint8_t *d = &rgba[((size_t)y * w + x) * 4];
            if (fmt == 0x0C000000u)
                swizzle4(s, t.swizzle, d);
            else if (fmt == 0x95000000u)
                swizzle4(pal + 4 * s[0], t.swizzle, d);
            else if (fmt == 0x00000000u)
                d[0] = d[1] = d[2] = s[0], d[3] = 255;
            else
                d[0] = d[1] = d[2] = s[0], d[3] = s[1];
        }
    return true;
}

} // namespace gxmtex
