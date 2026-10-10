"""Decodifica as texturas de uma captura do vitart (VITART_DUMP_FRAME) para PNG.

    python tools/gxm_tex_dump.py <dir da captura> [saída]

A captura tem textures.txt (as 4 palavras de cada SceGxmTexture usada nos desenhos do quadro e
quantos desenhos a usaram) e mem_<endereço>.bin com a memória do jogo. Layouts como no hardware:
swizzled = ordem Z (Morton) com o lado maior em linhas de quadrados; BCn swizzled = blocos 4x4 em
ordem Z; tiled = ladrilhos 32x32; linear = linhas alinhadas a 8 texels.
"""
import os
import sys

import numpy as np
from PIL import Image

TYPES = {0: "swizzled", 1: "cube", 2: "linear_old", 3: "linear", 4: "tiled", 5: "swizzled_arb", 6: "linear_strided", 7: "cube_arb"}


class Memory:
    def __init__(self, d):
        self.regions = []
        for line in open(os.path.join(d, "regions.txt"), encoding="utf-8"):
            p = line.split(None, 2)
            if len(p) < 2:
                continue
            start, size = int(p[0], 16), int(p[1], 16)
            self.regions.append((start, size, os.path.join(d, "mem_%08x.bin" % start)))
        self.cache = {}

    def read(self, addr, n):
        for start, size, path in self.regions:
            if start <= addr < start + size:
                if path not in self.cache:
                    self.cache[path] = np.fromfile(path, dtype=np.uint8)
                buf = self.cache[path]
                off = addr - start
                out = np.zeros(n, dtype=np.uint8)
                avail = max(0, min(n, len(buf) - off))
                out[:avail] = buf[off:off + avail]
                return out
        raise KeyError("endereço 0x%08x fora da captura" % addr)


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def describe(w):
    ty = bits(w[1], 29, 3)
    base = (bits(w[0], 31, 1) << 31) | (bits(w[1], 24, 5) << 24)
    swz = bits(w[3], 28, 3)
    if ty in (0, 1):  # dimensões em log2
        width, height = 1 << bits(w[1], 16, 4), 1 << bits(w[1], 0, 4)
    else:
        width, height = bits(w[1], 12, 12) + 1, bits(w[1], 0, 12) + 1
    stride = None
    if ty == 6:
        sc = bits(w[0], 9, 1) | (bits(w[0], 10, 2) << 1) | (bits(w[0], 17, 4) << 3) | (bits(w[0], 21, 6) << 7)
        stride = (sc + 1) * 4
    return dict(type=ty, base=base, swizzle=swz, width=width, height=height, stride=stride,
                data=w[2] & ~3, palette=bits(w[3], 0, 26) << 6)


# ------------------------------------------------------------ ordem Z
def part1by1(x):
    x &= 0x0000FFFF
    x = (x | (x << 8)) & 0x00FF00FF
    x = (x | (x << 4)) & 0x0F0F0F0F
    x = (x | (x << 2)) & 0x33333333
    x = (x | (x << 1)) & 0x55555555
    return x


def zorder_index(w, h):
    """Índice na memória de cada (y, x) de uma grade w x h (potências de 2) em ordem Z do Vita."""
    y, x = np.mgrid[0:h, 0:w].astype(np.uint32)
    m = min(w, h)
    k = m.bit_length() - 1
    idx = ((x >> k) | (y >> k)) << (2 * k)
    idx |= part1by1(x & (m - 1)) << 1
    idx |= part1by1(y & (m - 1))
    return idx


def pow2(n):
    return 1 << (n - 1).bit_length()


# ------------------------------------------------------------ BCn
def rgb565(c):
    r = ((c >> 11) & 31) * 255 // 31
    g = ((c >> 5) & 63) * 255 // 63
    b = (c & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def bc1_colors(blk, alpha_mode):
    c0 = blk[:, 0].astype(np.int32) | (blk[:, 1].astype(np.int32) << 8)
    c1 = blk[:, 2].astype(np.int32) | (blk[:, 3].astype(np.int32) << 8)
    p0, p1 = rgb565(c0), rgb565(c1)
    four = (c0 > c1) | (not alpha_mode)
    p2 = np.where(four[:, None], (2 * p0 + p1) // 3, (p0 + p1) // 2)
    p3 = np.where(four[:, None], (p0 + 2 * p1) // 3, 0)
    pal = np.stack([p0, p1, p2, p3], 1)  # (n, 4, 3)
    a = np.full((len(blk), 4), 255, np.int32)
    if alpha_mode:
        a[:, 3] = np.where(four, 255, 0)
    idx = blk[:, 4:8].astype(np.uint32)
    sel = np.zeros((len(blk), 16), np.int32)
    for i in range(16):
        sel[:, i] = (idx[:, i // 4] >> (2 * (i % 4))) & 3
    rgb = np.take_along_axis(pal, sel[:, :, None].repeat(3, 2), 1)
    alpha = np.take_along_axis(a, sel, 1)
    return rgb, alpha


def bc3_alpha(blk):
    a0 = blk[:, 0].astype(np.int32)
    a1 = blk[:, 1].astype(np.int32)
    pal = np.zeros((len(blk), 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    big = a0 > a1
    for i in range(1, 7):
        pal[:, i + 1] = np.where(big, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, i + 1] = np.where(big, pal[:, i + 1], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(big, pal[:, 6], 0)
    pal[:, 7] = np.where(big, pal[:, 7], 255)
    bitsv = np.zeros(len(blk), np.uint64)
    for i in range(6):
        bitsv |= blk[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    sel = np.zeros((len(blk), 16), np.int32)
    for i in range(16):
        sel[:, i] = ((bitsv >> np.uint64(3 * i)) & np.uint64(7)).astype(np.int32)
    return np.take_along_axis(pal, sel, 1)


def decode_bc(raw, fmt, bw, bh):
    bsize = 8 if fmt == 0x85000000 else 16
    blk = raw[:bw * bh * bsize].reshape(-1, bsize)
    if fmt == 0x85000000:
        rgb, a = bc1_colors(blk, True)
    elif fmt == 0x86000000:
        rgb, _ = bc1_colors(blk[:, 8:], False)
        a4 = np.zeros((len(blk), 16), np.int32)
        for i in range(16):
            a4[:, i] = ((blk[:, i // 2] >> (4 * (i % 2))) & 15) * 17
        a = a4
    else:
        rgb, _ = bc1_colors(blk[:, 8:], False)
        a = bc3_alpha(blk)
    px = np.concatenate([rgb, a[:, :, None]], 2).reshape(bh, bw, 4, 4, 4)
    return px.transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4).astype(np.uint8)


def apply_swizzle4(px, swz):
    # SceGxmTextureSwizzle4Mode: 0 ABGR, 1 ARGB, 2 RGBA, 3 BGRA, 4 1BGR, 5 1RGB, 6 RGB1, 7 BGR1
    # (componentes do MSB ao LSB de um u32 little-endian: ABGR = bytes R,G,B,A).
    order = {0: (0, 1, 2, 3), 1: (2, 1, 0, 3), 2: (3, 2, 1, 0), 3: (1, 2, 3, 0),
             4: (0, 1, 2, None), 5: (2, 1, 0, None), 6: (3, 2, 1, None), 7: (1, 2, 3, None)}[swz]
    out = np.empty_like(px)
    for c in range(3):
        out[..., c] = px[..., order[c]]
    out[..., 3] = 255 if order[3] is None else px[..., order[3]]
    return out


def decode(mem, t):
    w, h, ty, fmt = t["width"], t["height"], t["type"], t["base"]
    bc = fmt in (0x85000000, 0x86000000, 0x87000000)
    if bc:
        bw, bh = (w + 3) // 4, (h + 3) // 4
        if ty in (0, 1, 5, 7):
            pw, ph = pow2(bw), pow2(bh)
            bsize = 8 if fmt == 0x85000000 else 16
            raw = mem.read(t["data"], pw * ph * bsize).reshape(-1, bsize)
            idx = zorder_index(pw, ph)[:bh, :bw].reshape(-1)
            raw = raw[idx].reshape(-1)
        else:
            raw = mem.read(t["data"], bw * bh * (8 if fmt == 0x85000000 else 16))
        return decode_bc(raw, fmt, bw, bh)[:h, :w]
    if fmt in (0x0C000000, 0x95000000, 0x00000000, 0x07000000):
        bpp = {0x0C000000: 4, 0x95000000: 1, 0x00000000: 1, 0x07000000: 2}[fmt]
        if ty in (0, 1, 5, 7):
            pw, ph = pow2(w), pow2(h)
            raw = mem.read(t["data"], pw * ph * bpp).reshape(-1, bpp)
            raw = raw[zorder_index(pw, ph)[:h, :w].reshape(-1)].reshape(h, w, bpp)
        elif ty == 4:
            tw = (w + 31) // 32
            raw = mem.read(t["data"], tw * ((h + 31) // 32) * 1024 * bpp).reshape(-1, bpp)
            y, x = np.mgrid[0:h, 0:w]
            off = (((x >> 5) + tw * (y >> 5)) << 10) | (x & 31) | ((y & 31) << 5)
            raw = raw[off.reshape(-1)].reshape(h, w, bpp)
        else:
            stride = t["stride"] if ty == 6 else ((w + 7) & ~7) * bpp
            raw = mem.read(t["data"], stride * h).reshape(h, stride)[:, :w * bpp].reshape(h, w, bpp)
        if fmt == 0x0C000000:
            return apply_swizzle4(raw, t["swizzle"])
        if fmt == 0x95000000:
            pal = mem.read(t["palette"], 1024).reshape(256, 4)
            return apply_swizzle4(pal[raw[..., 0]], t["swizzle"])
        if fmt == 0x00000000:
            g = raw[..., 0]
            return np.stack([g, g, g, np.full_like(g, 255)], -1)
        g, a = raw[..., 0], raw[..., 1]
        return np.stack([g, g, g, a], -1)
    return None


def main():
    d = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(d, "png")
    os.makedirs(out, exist_ok=True)
    mem = Memory(d)
    done = []
    for line in open(os.path.join(d, "textures.txt"), encoding="utf-8"):
        p = line.split()
        if len(p) < 5:
            continue
        w = [int(x, 16) for x in p[:4]]
        t = describe(w)
        try:
            img = decode(mem, t)
        except KeyError as e:
            img = None
            print("  ", e)
        tag = "%08x_%dx%d_%s_%08x" % (t["data"], t["width"], t["height"], TYPES[t["type"]], t["base"])
        if img is None:
            print("sem decodificador: %s (%s desenhos)" % (tag, p[4]))
            continue
        path = os.path.join(out, tag + ".png")
        Image.fromarray(img, "RGBA").save(path)
        done.append((path, int(p[4]), t))
        print("%s  %s desenhos" % (tag, p[4]))
    print("%d de texturas decodificadas em %s" % (len(done), out))


if __name__ == "__main__":
    main()
