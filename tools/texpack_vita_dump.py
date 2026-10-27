"""Tira as texturas dos WADs do Vita para PNG, com a chave do pack no nome (sem rodar o jogo).

    python tools/texpack_vita_dump.py <psarc> <saída> [filtro no nome do wad] [pack/index.tsv]

Nome: <chave>_<recurso>_<l>x<a>[_nopack].png. A chave é a mesma que o vitart calcula ao vivo (texpack_build.py:
vita_key; nas P8 a paleta entra também). "_nopack" marca as que o pack de texturas ainda não cobre.
"""
import os
import struct
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(__file__))
import gxm_tex_dump as G  # noqa: E402
import psarc_gxp as P  # noqa: E402
import texpack_ps2 as T  # noqa: E402
from texpack_build import fnv, vita_key  # noqa: E402


class Mem:
    def __init__(self, b):
        self.b = b

    def read(self, addr, n):
        return np.frombuffer(self.b[addr:addr + n].ljust(n, b'\0'), np.uint8)


def textures(gxt):
    count, doff, dsize, np4, np8 = struct.unpack_from('<IIIII', gxt, 8)
    pal_base = doff + dsize - np4 * 64 - np8 * 1024
    for ti in range(count):
        off, size, pal, flags, ty, fmt, w, h = struct.unpack_from('<6I2H', gxt, 32 + 32 * ti)
        base = fmt & 0xFF000000
        t = dict(type=ty >> 29, base=base, swizzle=(fmt >> 12) & 7, width=w, height=h, stride=None, data=off,
                 palette=0)
        if base == 0x95000000:
            t['palette'] = pal_base + np4 * 64 + pal * 1024
        yield ti, t


def main():
    psarc, out = sys.argv[1], sys.argv[2]
    filt = sys.argv[3] if len(sys.argv) > 3 else ''
    have = set()
    if len(sys.argv) > 4:
        have = {int(l.split('\t')[0], 16) for l in open(sys.argv[4], encoding='utf-8') if '\t' in l}
    os.makedirs(out, exist_ok=True)
    f = open(psarc, 'rb')
    ents, zs, block = P.entries(f)
    files = P.read_file(f, ents[0], zs, block).decode('latin1').split('\n')
    n = 0
    for k, fn in enumerate(files):
        if not fn.lower().endswith('.wad_psp2') or filt not in fn:
            continue
        for tag, name, body in T.walk(P.read_file(f, ents[k + 1], zs, block)):
            if not (len(body) >= 46 and struct.unpack_from('<I', body, 0)[0] == 0x00070007 and body[4:8] == b'TXTR'
                    and body[14:18] == b'GXT\0'):
                continue
            gxt = body[14:]
            for ti, t in textures(gxt):
                key = vita_key(gxt, ti)
                if key is None:
                    continue
                if t['palette']:
                    key = fnv(gxt[t['palette']:t['palette'] + 1024], key)
                try:
                    px = G.decode(Mem(gxt), t)
                except Exception:  # noqa: BLE001 - formato fora do decodificador
                    px = None
                if px is None:
                    continue
                safe = ''.join(c if c.isalnum() else '_' for c in name)[:28]
                tail = '' if key in have else '_nopack'
                Image.fromarray(np.ascontiguousarray(px), 'RGBA').save(
                    os.path.join(out, '%016x_%s_%dx%d%s.png' % (key, safe, t['width'], t['height'], tail)))
                n += 1
    print(n, 'texturas ->', out)


if __name__ == '__main__':
    main()
