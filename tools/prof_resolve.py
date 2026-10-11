"""Resolve o prof.txt do vitart (VITART_PROF=1) em funções da DLL do jogo.

    python tools/prof_resolve.py <prof.txt> <dll.lldmap> <dll.syms> [top]

dll.lldmap: mapa do lld-link (/lldmap), dá o RVA da .text de cada objeto.
dll.syms:   linhas "objeto deslocamento símbolo" (llvm-nm --defined-only dos objetos, só T/t).
Saída: amostras por função (todas as threads somadas) e por thread, com a porcentagem do total.
"""
import bisect
import collections
import re
import sys


def load_map(path):
    text = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'([0-9a-f]+) ([0-9a-f]+)\s+\d+\s+(\S+):\(\.text\)\s*$', line)
        if m:
            text[m.group(3)] = int(m.group(1), 16)
    return text


def load_syms(path, text):
    out = []
    for line in open(path, encoding='utf-8'):
        obj, off, name = line.split()
        if obj in text:
            out.append((text[obj] + int(off, 16), name))
    out.sort()
    return out


def main():
    prof, lmap, syms = sys.argv[1:4]
    top = int(sys.argv[4]) if len(sys.argv) > 4 else 40
    table = load_syms(syms, load_map(lmap))
    addrs = [a for a, _ in table]
    dll = None
    per_func = collections.Counter()
    per_thread = collections.defaultdict(collections.Counter)
    total = 0
    for line in open(prof, encoding='utf-8', errors='replace'):
        if line.startswith('#'):
            continue
        n, where, thread = line.split(' ', 2)
        n = int(n)
        mod, rva = where.rsplit('+', 1)
        rva = int(rva, 16)
        if mod.lower().startswith('gow1') and mod.lower().endswith('.dll'):
            i = bisect.bisect_right(addrs, rva) - 1
            name = table[i][1] if i >= 0 else '?'
            key = 'gow1:' + name
        else:
            key = mod
        per_func[key] += n
        per_thread[thread.strip()][key] += n
        total += n
    print(f'total {total} amostras')
    for k, n in per_func.most_common(top):
        print(f'{100.0 * n / total:6.2f}% {n:7d} {k}')
    for t, c in sorted(per_thread.items(), key=lambda kv: -sum(kv[1].values())):
        s = sum(c.values())
        print(f'\n== thread {t}: {s} ({100.0 * s / total:.1f}%)')
        for k, n in c.most_common(12):
            print(f'{100.0 * n / s:6.2f}% {n:7d} {k}')


if __name__ == '__main__':
    main()
