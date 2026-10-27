"""inimigo_vida.py <enemy_dump_gow1_e.bin>: acha o campo de vida dos inimigos do GoW1 pelos bytes gravados.

Registro: quadro, objeto, vtable (u32 cada) e 0x800 bytes. Para cada objeto com várias amostras, um float que cai
(e não sobe) com um vizinho fixo >= ele é candidato a vida/máximo; vota por (vtable, deslocamento).
"""
import collections
import math
import struct
import sys

REC = 12 + 0x800


def main():
    d = open(sys.argv[1], 'rb').read()
    objs = collections.defaultdict(list)
    for o in range(0, len(d) - REC + 1, REC):
        frame, obj, vt = struct.unpack_from('<III', d, o)
        objs[(obj, vt)].append(d[o + 12:o + REC])
    print('%d objetos, classes: %s' % (len(objs), collections.Counter(vt for _, vt in objs).most_common(8)))
    votes = collections.Counter()
    examples = {}
    for (obj, vt), recs in objs.items():
        if len(recs) < 4:
            continue
        fl = lambda r, off: struct.unpack_from('<f', r, off)[0]  # noqa: E731
        for off in range(0, 0x800 - 4, 4):
            s = [fl(r, off) for r in recs]
            if not all(math.isfinite(v) and 0 <= v < 100000 for v in s) or s[-1] >= s[0]:
                continue
            if any(b > a + 1e-3 for a, b in zip(s, s[1:])):
                continue
            for nb in (off + 4, off - 4):
                if not 0 <= nb < 0x800:
                    continue
                m = [fl(r, nb) for r in recs]
                if max(m) == min(m) and m[0] >= s[0] > 0 and m[0] < 100000:
                    votes[(vt, off, nb)] += 1
                    examples[(vt, off, nb)] = (obj, s[0], s[-1], m[0])
    for (vt, off, nb), n in votes.most_common(15):
        obj, a, b, m = examples[(vt, off, nb)]
        print('vt %08x vida +%03x máx +%03x: %d objetos (ex. %08x %.1f -> %.1f de %.1f)' % (vt, off, nb, n, obj, a, b, m))


if __name__ == '__main__':
    main()
