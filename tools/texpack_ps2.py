"""Nomes das texturas do God of War de PS2 no padrão de substituição do PCSX2.

    python tools/texpack_ps2.py <iso> <saída.tsv> [lista de arquivos do pack]

Percorre os .WAD da ISO (tools/ps2_audio.toc), acha cada TXR_ (tipo 7: nome do GFX e da paleta) e
calcula o nome que o PCSX2 dá ao arquivo: TEX0Hash-CLUTHash-bits (hexadecimal sem zeros à esquerda;
bits = PSM | TW<<6 | TH<<10). Como o PCSX2 (GSTextureCache::HashCacheKey::Create):
  - TEX0Hash = XXH3-64 dos blocos de 256 bytes da memória do GS na ordem dos blocos do retângulo
    (PSMT8: blocos 16x16, PSMT4: 32x16, PSMCT32: 8x8, cada um no arranjo das columnTable do GS); se a
    textura é menor que um bloco ou o formato não usa os 32 bits (PSMT8H), XXH3 das linhas expandidas;
  - CLUTHash = XXH3-64 da paleta em 32 bits na ordem lógica (16 ou 256 cores).
Formato dos WAD e dos GFX: god_of_war_browser (mogaika). Saída: nome_do_wad, TXR, GFX, PAL, nome PCSX2.
"""
import os
import struct
import sys

import xxhash

sys.path.insert(0, os.path.dirname(__file__))
import ps2_audio  # noqa: E402

COL32 = [
    [0, 1, 4, 5, 8, 9, 12, 13], [2, 3, 6, 7, 10, 11, 14, 15], [16, 17, 20, 21, 24, 25, 28, 29],
    [18, 19, 22, 23, 26, 27, 30, 31], [32, 33, 36, 37, 40, 41, 44, 45], [34, 35, 38, 39, 42, 43, 46, 47],
    [48, 49, 52, 53, 56, 57, 60, 61], [50, 51, 54, 55, 58, 59, 62, 63]]
COL8 = [
    [0, 4, 16, 20, 32, 36, 48, 52, 2, 6, 18, 22, 34, 38, 50, 54],
    [8, 12, 24, 28, 40, 44, 56, 60, 10, 14, 26, 30, 42, 46, 58, 62],
    [33, 37, 49, 53, 1, 5, 17, 21, 35, 39, 51, 55, 3, 7, 19, 23],
    [41, 45, 57, 61, 9, 13, 25, 29, 43, 47, 59, 63, 11, 15, 27, 31],
    [96, 100, 112, 116, 64, 68, 80, 84, 98, 102, 114, 118, 66, 70, 82, 86],
    [104, 108, 120, 124, 72, 76, 88, 92, 106, 110, 122, 126, 74, 78, 90, 94],
    [65, 69, 81, 85, 97, 101, 113, 117, 67, 71, 83, 87, 99, 103, 115, 119],
    [73, 77, 89, 93, 105, 109, 121, 125, 75, 79, 91, 95, 107, 111, 123, 127],
    [128, 132, 144, 148, 160, 164, 176, 180, 130, 134, 146, 150, 162, 166, 178, 182],
    [136, 140, 152, 156, 168, 172, 184, 188, 138, 142, 154, 158, 170, 174, 186, 190],
    [161, 165, 177, 181, 129, 133, 145, 149, 163, 167, 179, 183, 131, 135, 147, 151],
    [169, 173, 185, 189, 137, 141, 153, 157, 171, 175, 187, 191, 139, 143, 155, 159],
    [224, 228, 240, 244, 192, 196, 208, 212, 226, 230, 242, 246, 194, 198, 210, 214],
    [232, 236, 248, 252, 200, 204, 216, 220, 234, 238, 250, 254, 202, 206, 218, 222],
    [193, 197, 209, 213, 225, 229, 241, 245, 195, 199, 211, 215, 227, 231, 243, 247],
    [201, 205, 217, 221, 233, 237, 249, 253, 203, 207, 219, 223, 235, 239, 251, 255]]


# columnTable4 do GS (16 linhas x 32 nibbles por bloco), copiada do PCSX2 (GSTables.cpp).
COL4 = [[0, 8, 32, 40, 64, 72, 96, 104, 2, 10, 34, 42, 66, 74, 98, 106, 4, 12, 36, 44, 68, 76, 100, 108, 6, 14, 38, 46, 70, 78, 102, 110], [16, 24, 48, 56, 80, 88, 112, 120, 18, 26, 50, 58, 82, 90, 114, 122, 20, 28, 52, 60, 84, 92, 116, 124, 22, 30, 54, 62, 86, 94, 118, 126], [65, 73, 97, 105, 1, 9, 33, 41, 67, 75, 99, 107, 3, 11, 35, 43, 69, 77, 101, 109, 5, 13, 37, 45, 71, 79, 103, 111, 7, 15, 39, 47], [81, 89, 113, 121, 17, 25, 49, 57, 83, 91, 115, 123, 19, 27, 51, 59, 85, 93, 117, 125, 21, 29, 53, 61, 87, 95, 119, 127, 23, 31, 55, 63], [192, 200, 224, 232, 128, 136, 160, 168, 194, 202, 226, 234, 130, 138, 162, 170, 196, 204, 228, 236, 132, 140, 164, 172, 198, 206, 230, 238, 134, 142, 166, 174], [208, 216, 240, 248, 144, 152, 176, 184, 210, 218, 242, 250, 146, 154, 178, 186, 212, 220, 244, 252, 148, 156, 180, 188, 214, 222, 246, 254, 150, 158, 182, 190], [129, 137, 161, 169, 193, 201, 225, 233, 131, 139, 163, 171, 195, 203, 227, 235, 133, 141, 165, 173, 197, 205, 229, 237, 135, 143, 167, 175, 199, 207, 231, 239], [145, 153, 177, 185, 209, 217, 241, 249, 147, 155, 179, 187, 211, 219, 243, 251, 149, 157, 181, 189, 213, 221, 245, 253, 151, 159, 183, 191, 215, 223, 247, 255], [256, 264, 288, 296, 320, 328, 352, 360, 258, 266, 290, 298, 322, 330, 354, 362, 260, 268, 292, 300, 324, 332, 356, 364, 262, 270, 294, 302, 326, 334, 358, 366], [272, 280, 304, 312, 336, 344, 368, 376, 274, 282, 306, 314, 338, 346, 370, 378, 276, 284, 308, 316, 340, 348, 372, 380, 278, 286, 310, 318, 342, 350, 374, 382], [321, 329, 353, 361, 257, 265, 289, 297, 323, 331, 355, 363, 259, 267, 291, 299, 325, 333, 357, 365, 261, 269, 293, 301, 327, 335, 359, 367, 263, 271, 295, 303], [337, 345, 369, 377, 273, 281, 305, 313, 339, 347, 371, 379, 275, 283, 307, 315, 341, 349, 373, 381, 277, 285, 309, 317, 343, 351, 375, 383, 279, 287, 311, 319], [448, 456, 480, 488, 384, 392, 416, 424, 450, 458, 482, 490, 386, 394, 418, 426, 452, 460, 484, 492, 388, 396, 420, 428, 454, 462, 486, 494, 390, 398, 422, 430], [464, 472, 496, 504, 400, 408, 432, 440, 466, 474, 498, 506, 402, 410, 434, 442, 468, 476, 500, 508, 404, 412, 436, 444, 470, 478, 502, 510, 406, 414, 438, 446], [385, 393, 417, 425, 449, 457, 481, 489, 387, 395, 419, 427, 451, 459, 483, 491, 389, 397, 421, 429, 453, 461, 485, 493, 391, 399, 423, 431, 455, 463, 487, 495], [401, 409, 433, 441, 465, 473, 497, 505, 403, 411, 435, 443, 467, 475, 499, 507, 405, 413, 437, 445, 469, 477, 501, 509, 407, 415, 439, 447, 471, 479, 503, 511]]


def walk(d):
    """Registros do WAD do GoW1 (PS2): u16 tag, u16 flags, u32 tamanho, nome[24]; a tag 24 não tem corpo."""
    o, out = 0, []
    while o + 32 <= len(d):
        tag, flags, size = struct.unpack_from('<HHI', d, o)
        name = d[o + 8:o + 32].split(b'\0')[0].decode('latin1')
        body = b'' if tag == 24 else d[o + 32:o + 32 + size]
        out.append((tag, name, body))
        o = (o + 32 + (0 if tag == 24 else size) + 15) & ~15
    return out


def unswizzle8(x, y, w):
    block = (y & ~0xF) * w + (x & ~0xF) * 2
    swap = (((y + 2) >> 2) & 1) * 4
    posy = (((y & ~3) >> 1) + (y & 1)) & 7
    col = posy * w * 2 + ((x + swap) & 7) * 4
    return block + col + ((y >> 1) & 1) + ((x >> 2) & 2)


def gfx_parse(b):
    magic, w, h, enc, bpi, n = struct.unpack_from('<6I', b, 0)
    if magic != 0xC or not n:
        return None
    rh = h // n
    size = w * rh * bpi // 8
    datas = [b[24 + i * size:24 + (i + 1) * size] for i in range(n)]
    return dict(w=w, h=h, rh=rh, enc=enc, bpi=bpi, data=datas)


def palette(g, i):
    raw = g['data'][i]
    n = g['w'] * g['rh']
    cols = list(struct.unpack_from('<%dI' % n, raw, 0))
    if g['h'] in (16, 32):  # paleta de 256 em CSM1: blocos de 8 trocados (0,2,1,3)
        out = [0] * n
        for k in range(n):
            blk, pos = k // 8, k % 8
            out[pos + ([0, 2, 1, 3][blk % 4] + (blk // 4) * 4) * 8] = cols[k]
        cols = out
    return struct.pack('<%dI' % n, *cols)


def log2up(v):
    r = 0
    while (1 << r) < v:
        r += 1
    return r


def indexes(g, i):
    w, h, d = g['w'], g['rh'], g['data'][i]
    if g['bpi'] == 8:
        if g['enc'] & 2 == 0:
            return [[d[unswizzle8(x, y, w)] if unswizzle8(x, y, w) < len(d) else 0 for x in range(w)] for y in range(h)]
        return [[d[x + y * w] for x in range(w)] for y in range(h)]
    if g['bpi'] == 4:
        return [[(d[(x + y * w) // 2] >> (4 * (x & 1))) & 15 for x in range(w)] for y in range(h)]
    return None


def tex0hash(g, i):
    """(TEX0Hash, PSM, TW, TH) ou None."""
    w, h, bpi = g['w'], g['rh'], g['bpi']
    tw, th = log2up(w), log2up(h)
    W, H = 1 << tw, 1 << th
    if bpi in (8, 4):
        idx = indexes(g, i)
        px = lambda x, y: idx[y][x] if x < w and y < h else 0  # noqa: E731
        psm = 0x13 if bpi == 8 and g['enc'] & 2 == 0 else 0x1B if bpi == 8 else 0x14
        bx, by = (16, 16) if bpi == 8 else (32, 16)
        if W < bx or H < by or psm == 0x1B:
            raw = bytes(px(x, y) for y in range(H) for x in range(W))
        else:
            out = bytearray()
            for b_y in range(0, H, by):
                for b_x in range(0, W, bx):
                    blk = bytearray(256)
                    if bpi == 8:
                        for y in range(16):
                            for x in range(16):
                                blk[COL8[y][x]] = px(b_x + x, b_y + y)
                    else:
                        for y in range(16):
                            for x in range(32):
                                o = COL4[y][x]
                                blk[o >> 1] |= px(b_x + x, b_y + y) << (4 * (o & 1))
                    out += blk
            raw = bytes(out)
        return xxhash.xxh3_64_intdigest(raw), psm, tw, th
    if bpi == 32:
        d = g['data'][i]
        px = lambda x, y: d[(x + y * w) * 4:(x + y * w) * 4 + 4] if x < w and y < h else b'\0\0\0\0'  # noqa: E731
        if W < 8 or H < 8:
            raw = b''.join(px(x, y) for y in range(H) for x in range(W))
        else:
            out = bytearray()
            for b_y in range(0, H, 8):
                for b_x in range(0, W, 8):
                    blk = bytearray(256)
                    for y in range(8):
                        for x in range(8):
                            o = COL32[y][x] * 4
                            blk[o:o + 4] = px(b_x + x, b_y + y)
                    out += blk
            raw = bytes(out)
        return xxhash.xxh3_64_intdigest(raw), 0, tw, th
    return None


def main():
    iso = open(sys.argv[1], 'rb')
    toc = ps2_audio.toc(iso)
    pack = set()
    if len(sys.argv) > 3:
        for line in open(sys.argv[3], encoding='utf-8', errors='replace'):
            base = line.strip().replace('\\', '/').rsplit('/', 1)[-1]
            if base.lower().endswith('.png'):
                parts = base[:-4].split('-')
                if len(parts) >= 2:
                    pack.add('-'.join(parts[:2]) if len(parts) >= 3 else parts[0])
    rows, hits, total = [], set(), 0
    # GFX/PAL podem estar em outro WAD (o permanente, o da fase): índice global, primeiro que aparece.
    wads = {}
    glob_by = {}
    for wad in sorted(n for n in toc if n.endswith('.WAD')):
        size, lba = toc[wad]
        iso.seek(lba * 2048)
        by = {name: body for tag, name, body in walk(iso.read(size)) if tag == 30 and len(body) >= 4}
        wads[wad] = by
        for name, body in by.items():
            glob_by.setdefault(name, body)
    for wad, by_name in wads.items():
        for name, body in by_name.items():
            if struct.unpack_from('<I', body, 0)[0] != 7 or len(body) < 88:
                continue
            gname = body[4:28].split(b'\0')[0].decode('latin1')
            pname = body[28:52].split(b'\0')[0].decode('latin1')
            g = gfx_parse(by_name.get(gname) or glob_by.get(gname, b'\0' * 24))
            if not g:
                continue
            pb = by_name.get(pname) or glob_by.get(pname)
            p = gfx_parse(pb) if pb else None
            for gi in range(len(g['data'])):
                th = tex0hash(g, gi)
                if not th:
                    continue
                h0, psm, tw, thh = th
                bits = psm | tw << 6 | thh << 10
                pals = range(len(p['data'])) if (p and g['bpi'] in (8, 4)) else [None]
                for pi in pals:
                    if pi is None:
                        key = '%x' % h0
                        full = '%x-%08x' % (h0, bits)
                    else:
                        ch = xxhash.xxh3_64_intdigest(palette(p, pi))
                        key = '%x-%x' % (h0, ch)
                        full = '%x-%x-%08x' % (h0, ch, bits)
                    total += 1
                    if key in pack:
                        hits.add(key)
                    rows.append((wad, name, gname, pname, gi, pi, full))
    with open(sys.argv[2], 'w', encoding='utf-8') as o:
        for r in rows:
            o.write('\t'.join(str(x) for x in r) + '\n')
    print(f'{total} texturas (combinações gfx x paleta) em {len({r[0] for r in rows})} WADs')
    if pack:
        print(f'pack: {len(pack)} nomes; encontrados entre as texturas do PS2: {len(hits)} ({100.0 * len(hits) / len(pack):.1f}%)')


if __name__ == '__main__':
    main()
