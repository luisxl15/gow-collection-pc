"""Invariante de decodificação: dentro de uma função, nenhuma instrução decodificada pode
começar no meio de outra. Uma sobreposição prova que algum alvo (tabela, ponteiro, BL)
caiu no meio de uma instrução. Uso: python tools/check_overlap.py módulo.elf [...]"""
import sys
from collections import Counter
from vitarecomp import module as M
from vitarecomp.arm import image, analysis

total = Counter()
for path in sys.argv[1:]:
    mod = M.load(open(path, 'rb').read(), path)
    img = image.from_vita_module(mod)
    prog = analysis.analyze(img)
    bad = []
    for f in prog.functions.values():
        ks = sorted(f.insns)
        for a, b in zip(ks, ks[1:]):
            if a + f.insns[a].size > b:
                bad.append((f.name, hex(a), f.insns[a].cs.mnemonic, hex(b), f.insns[b].cs.mnemonic,
                            'swept' if f.swept else ''))
    n = sum(len(f.insns) for f in prog.functions.values())
    print(f"{path}: {len(prog.functions)} funções, {n} instruções, {len(bad)} sobreposições")
    for x in bad[:8]:
        print("   ", *x)
    total['funcs'] += len(prog.functions); total['overlaps'] += len(bad)
print(dict(total))
