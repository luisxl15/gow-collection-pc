"""kratos_campos.py <enemy_dump_gow1.bin>: acha posição e vida no objeto do Kratos a partir dos bytes gravados.

Cada registro: quadro (u32), endereço (u32), 0x800 bytes do objeto. Posição: trio de floats finitos, de mundo
(|v| < 50000), que muda entre registros. Vida: float que nunca sobe por muito tempo, com um vizinho fixo >= ele.
"""
import math
import struct
import sys

REC = 8 + 0x800


def main():
    d = open(sys.argv[1], 'rb').read()
    recs = []
    for o in range(0, len(d) - REC + 1, REC):
        frame, addr = struct.unpack_from('<II', d, o)
        recs.append((frame, addr, d[o + 8:o + REC]))
    print('%d registros, objetos %s' % (len(recs), sorted({hex(a) for _, a, _ in recs})))
    recs = [r for r in recs if r[1] == recs[-1][1]]  # só o objeto atual
    f = lambda r, off: struct.unpack_from('<f', r[2], off)[0]  # noqa: E731
    print('-- posição (trios que mudam)')
    for off in range(0, 0x800 - 12, 4):
        tri = [[f(r, off + 4 * k) for k in range(3)] for r in recs]
        if not all(all(math.isfinite(v) and abs(v) < 50000 for v in t) for t in tri):
            continue
        span = [max(t[k] for t in tri) - min(t[k] for t in tri) for k in range(3)]
        mag = max(abs(v) for t in tri for v in t)
        if sum(s > 1.0 for s in span) >= 2 and mag > 10:
            print('  +%03x span=(%.0f %.0f %.0f) primeiro=(%.0f %.0f %.0f) último=(%.0f %.0f %.0f)' % (
                off, *span, *tri[0], *tri[-1]))
    print('-- vida (cai e não volta, com máximo fixo ao lado)')
    for off in range(0, 0x800 - 4, 4):
        s = [f(r, off) for r in recs]
        if not all(math.isfinite(v) and 0 <= v < 100000 for v in s) or max(s) == min(s):
            continue
        ups = sum(1 for a, b in zip(s, s[1:]) if b > a)
        downs = sum(1 for a, b in zip(s, s[1:]) if b < a)
        for nb in (off + 4, off - 4, off + 8):
            if 0 <= nb < 0x800:
                m = [f(r, nb) for r in recs]
                if max(m) == min(m) and m[0] > 0 and m[0] >= max(s):
                    print('  +%03x (máx em +%03x = %g) %d desc / %d sub  %s' % (off, nb, m[0], downs, ups,
                                                                          ' '.join('%g' % v for v in s[::max(1, len(s) // 12)])))


if __name__ == '__main__':
    main()
