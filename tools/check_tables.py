"""Controle das tabelas de tbb/tbh: compara o limite provado pela guarda com o limite estrutural
(layout) e conta sobreposições de decodificação. Uso: python tools/check_tables.py módulo.elf [...]"""
import sys
from collections import Counter
from vitarecomp import module as M
from vitarecomp.arm import image, analysis

res = Counter(); bad = []
for path in sys.argv[1:]:
    mod = M.load(open(path, 'rb').read(), path)
    img = image.from_vita_module(mod)
    prog = analysis.analyze(img)
    seen = set(); ov = 0
    for f in prog.functions.values():
        ks = sorted(f.insns)
        ov += sum(1 for a, b in zip(ks, ks[1:]) if a + f.insns[a].size > b)
        for a, targets in f.tables.items():
            if a in seen:
                continue
            seen.add(a)
            i = f.insns[a]
            lay = analysis._table_by_layout(img, i.next, i.name)
            k = 'igual' if len(lay) == len(targets) else ('guarda < layout' if len(lay) > len(targets) else 'guarda > LAYOUT')
            res[k] += 1
            if k != 'igual':
                bad.append((path.split('/')[-1], hex(a), len(targets), len(lay)))
    lw = set(w.split(' at ')[1].split()[0] for w in prog.warnings if 'layout' in w)
    print(f"{path}: {len(prog.functions)} funções, {len(seen)} tabelas, {len(lw)} só por layout, {ov} sobreposições")
print(dict(res))
for b in bad:
    print('  ', *b)
