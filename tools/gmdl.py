"""GMDL do Vita (registro ~MD_<nome>, tipo 0x0003000f): a malha remasterizada que o porte desenha no lugar da malha
de PS2 (MDL_<nome>_0, que fica intacta no WAD).

    python tools/gmdl.py <arquivo com o corpo do registro>     # resumo

Formato (god_of_war_browser, pack/wad/mesh/gmdl; variante do Vita): depois do u32 do tipo do registro,
  'GMDL' (cabeçalho em big-endian): 1, 1, 1, n de modelos, 1, e os deslocamentos dos modelos;
  modelo: 'MODL', u16 id, u16 flags (9 no Vita), u32 deslocamento dos índices; depois os dados:
    u32 n de streams (little-endian) e streams de 16 bytes: nome[4] (ao contrário), u16 id, u16 flags (2),
    u32 deslocamento e u32 tamanho (relativos ao modelo). Ids: 2 POS0 (4 floats), 4 BONI (4 bytes), 5 TEX0 (2 half),
    6 COL0 (4 half), 9 NRM0 (3 bytes com sinal);
    índices: 2 floats, u32 (big-endian), u32 n (big-endian), n u16;
    objetos: u32 texturas usadas, u32 n de objetos (big-endian), e cada objeto: bbox 6 floats, u16 textura, u8 0,
    u32 início/quantidade de vértices, u32 início/quantidade de índices, u32 n e o mapa de ossos (u32).
"""
import struct
import sys

import numpy as np

STREAM_FMT = {2: ('<f4', 4), 4: ('u1', 4), 5: ('<f2', 2), 6: ('<f2', 4), 9: ('i1', 3)}  # NRM0: 3 bytes no Vita


class Obj:
    pass


class Model:
    pass


def parse_model(b, mo):
    m = Model()
    assert b[mo:mo + 4] == b'MODL'
    m.id, m.flags, ioff = struct.unpack_from('>HHI', b, mo + 4)
    base = mo + 0xc
    n = struct.unpack_from('<I', b, base)[0]
    m.streams = {}
    m.stream_order = []
    for i in range(n):
        so = base + 4 + 16 * i
        name = b[so:so + 4][::-1].decode('latin1')
        sid, sfl, off, size = struct.unpack_from('<HHII', b, so + 4)
        dt, comps = STREAM_FMT[sid]
        arr = np.frombuffer(b, dtype=dt, count=size // np.dtype(dt).itemsize, offset=base + off).reshape(-1, comps)
        m.streams[name] = (sid, arr)
        m.stream_order.append(name)
    io = base + ioff
    m.idx_f = struct.unpack_from('<ff', b, io)
    m.idx_u = struct.unpack_from('>I', b, io + 8)[0]
    ni = struct.unpack_from('>I', b, io + 12)[0]
    m.indices = list(struct.unpack_from('>%dH' % ni, b, io + 16))
    oo = io + 16 + 2 * ni
    m.textures, nobj = struct.unpack_from('>II', b, oo)
    p = oo + 8
    m.objects = []
    for i in range(nobj):
        o = Obj()
        o.bbox = struct.unpack_from('>6f', b, p)
        o.texture, z = struct.unpack_from('>HB', b, p + 24)
        o.v0, o.vn, o.i0, o.in_, nj = struct.unpack_from('>5I', b, p + 27)
        o.joints = list(struct.unpack_from('>%dI' % nj, b, p + 47))
        p += 47 + 4 * nj
        m.objects.append(o)
    m.end = p
    return m


def parse(rec):
    b = rec[4:]  # u32 do tipo do registro (0x0003000f)
    assert b[:4] == b'GMDL'
    hdr = struct.unpack_from('>6I', b, 0)
    nmod = hdr[4]
    offs = struct.unpack_from('>%dI' % nmod, b, 0x18)
    return hdr, [parse_model(b, o) for o in offs]


def main():
    rec = open(sys.argv[1], 'rb').read()
    hdr, models = parse(rec)
    print('GMDL', hdr)
    for m in models:
        print('modelo %d flags %d: streams %s; índices %d (%s, %#x); texturas %d; objetos %d' % (
            m.id, m.flags, {k: (v[0], v[1].shape) for k, v in m.streams.items()}, len(m.indices), m.idx_f, m.idx_u,
            m.textures, len(m.objects)))
        for i, o in enumerate(m.objects):
            print('   obj %d: textura %d vértices %d+%d índices %d+%d ossos %s' % (i, o.texture, o.v0, o.vn, o.i0, o.in_, o.joints[:8]))


if __name__ == '__main__':
    main()
