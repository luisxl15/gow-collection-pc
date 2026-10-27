"""casar_globais.py <elf_a> <elf_b> <endereço global no a> [...]: acha o global equivalente no outro jogo.

Para cada uso do global no jogo A, olha os textos (strings) que o código em volta usa; acha os mesmos textos no
jogo B, pega o código em volta deles e vota no global que aparece na mesma posição relativa ao texto. Usa os
índices do xref.py (<elf>.xref.pkl). Só lê os ELFs, não roda nada.
"""
import collections
import pickle
import struct
import sys

WIN = 0x180  # bytes de código antes/depois de cada uso


def load(elf):
    d = open(elf, 'rb').read()
    x = pickle.load(open(elf + '.xref.pkl', 'rb'))
    at = {}  # endereço do movw/movt -> constante
    for c, addrs in x['refs'].items():
        for a in addrs:
            at[a] = c
    return d, x, sorted(at.items())


def text_at(d, segs, a):
    for va, off, sz, _ in segs:
        if va <= a < va + sz:
            o = off + a - va
            e = d.find(b'\0', o, o + 80)
            if e - o >= 4:
                s = d[o:e]
                if all(32 <= ch < 127 for ch in s):
                    return s.decode()
    return None


def window(seq, a):
    import bisect
    i = bisect.bisect_left(seq, (a - WIN, -1))
    out = []
    while i < len(seq) and seq[i][0] <= a + WIN:
        out.append(seq[i])
        i += 1
    return out


def main():
    ea, eb = sys.argv[1], sys.argv[2]
    da, xa, sa = load(ea)
    db, xb, sb = load(eb)
    # texto -> endereços no B
    texts_b = collections.defaultdict(list)
    for c in xb['refs']:
        t = text_at(db, xb['segs'], c)
        if t:
            texts_b[t].append(c)
    for g in (int(v, 16) for v in sys.argv[3:]):
        votes = collections.Counter()
        for use in xa['refs'].get(g, []):
            win = window(sa, use)
            for (pa, ca) in win:
                t = text_at(da, xa['segs'], ca)
                if not t or t not in texts_b:
                    continue
                rel = use - pa  # o global em relação ao texto
                for cb in texts_b[t]:
                    for ub in xb['refs'].get(cb, []):
                        for (pb, gb) in window(sb, ub + rel):
                            if abs((pb - ub) - rel) <= 8 and gb >= 0x81000000 and not text_at(db, xb['segs'], gb):
                                votes[gb] += 1
        top = votes.most_common(5)
        print('%08x ->' % g, ', '.join('%08x (%d)' % (k, v) for k, v in top) or 'nada')


if __name__ == '__main__':
    main()
