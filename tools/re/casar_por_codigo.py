"""casar_por_codigo.py <elf_a> <elf_b> <global no a> [...]: acha o global equivalente comparando o código.

Os dois GoW saíram do mesmo motor e do mesmo compilador: muita função é igual a menos dos endereços. Para cada
uso do global no jogo A, a assinatura é a sequência de mnemônicos (sem operandos) em volta do movw; no jogo B,
procura os movw/movt de endereço de dados com a mesma assinatura e vota no endereço que eles carregam.
"""
import collections
import os
import pickle
import struct
import sys

import capstone as cs

K = int(os.environ.get("CASAR_K", "12"))  # instruções de cada lado


def disasm(elf):
    d = open(elf, 'rb').read()
    x = pickle.load(open(elf + '.xref.pkl', 'rb'))
    va, off, fsz, _ = next(s for s in x['segs'] if s[3] & 1)
    md = cs.Cs(cs.CS_ARCH_ARM, cs.CS_MODE_THUMB)
    md.skipdata = True
    addrs, mnem = [], []
    for a, size, m, _ in md.disasm_lite(d[off:off + fsz], va):
        addrs.append(a)
        mnem.append(m)
    idx = {a: i for i, a in enumerate(addrs)}
    return x, addrs, mnem, idx


def sig(mnem, i):
    return tuple(mnem[max(0, i - K):i + K + 1])


def main():
    xa, aa, ma, ia = disasm(sys.argv[1])
    xb, ab, mb, ib = disasm(sys.argv[2])
    # assinatura -> constantes no B (só endereços de dados do jogo)
    by_sig = collections.defaultdict(collections.Counter)
    for c, uses in xb['refs'].items():
        if not (0x81200000 <= c < 0x82000000):
            continue
        for u in uses:
            if u in ib:
                by_sig[sig(mb, ib[u])][c] += 1
    for g in (int(v, 16) for v in sys.argv[3:]):
        votes = collections.Counter()
        uses = xa['refs'].get(g, [])
        hit = 0
        for u in uses:
            if u in ia:
                s = sig(ma, ia[u])
                if s in by_sig:
                    hit += 1
                    votes.update(by_sig[s])
        print('%08x (%d usos, %d casados) -> %s' % (g, len(uses), hit,
              ', '.join('%08x (%d)' % kv for kv in votes.most_common(4)) or 'nada'))


if __name__ == '__main__':
    main()
