"""Confere a leitura das varyings do GXP contra o próprio programa, em todos os programas de uma captura.

    python tools/usse_varyings_check.py <dir da captura>

Fragmento: a soma dos registradores declarados pelos descritores tem de ser igual à contagem de PA do
cabeçalho (0x30), e o código não pode ler PA além disso. Vértice: o código não pode escrever OUTPUT além
das saídas declaradas, e a última saída declarada tem de ser escrita.
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "vitarecomp"))
from usse_coverage import programs  # noqa: E402
from vitarecomp.usse import operands as O  # noqa: E402
from vitarecomp.usse.disasm import lower  # noqa: E402


def touched(prog, bank, want_dest):
    """Maior registrador (em palavras de 32 bits) do banco lido (fontes) ou escrito (destino)."""
    hi = -1
    for ins in prog.primary:
        try:
            lo = lower(ins, False)
        except Exception:
            continue
        if not lo or lo[0] == "vdual":
            continue
        ops = [lo[1]] if want_dest else list(lo[3])
        for op in ops:
            if isinstance(op, O.Operand) and op.bank == bank:
                hi = max(hi, op.num + 3)  # vec4 no pior caso
    return hi


def main():
    progs = programs(sys.argv[1])
    stats = collections.Counter()
    bad = []
    for h, p in progs.items():
        if p.fragment:
            total = sum(i["regs"] for i in p.frag_inputs)
            stats["frag"] += 1
            if total == p.pa_count:
                stats["frag: soma == pa_count"] += 1
            else:
                bad.append((h, "frag", total, p.pa_count, p.frag_inputs))
            for i in p.frag_inputs:
                stats["frag %s %s" % (i["kind"], i.get("dtype", i.get("store")))] += 1
            stats["saída tipo %d x%d" % (p.output_type, p.output_components)] += 1
        else:
            stats["vert"] += 1
            name, loc, n, reg = p.vertex_outputs[-1]
            total = reg + n
            hi = touched(p, O.OUT, True)
            if hi < total + 3:
                stats["vert: nada escrito além das saídas"] += 1
            else:
                bad.append((h, "vert", total, hi, p.vertex_outputs))
            for o in p.vertex_outputs:
                stats["vert saída %s x%d" % (o[0], o[2])] += 1
    for k, v in sorted(stats.items()):
        print("%-40s %d" % (k, v))
    print("divergências:", len(bad))
    for b in bad[:8]:
        print(" ", b[:4])
        print("    ", b[4])


if __name__ == "__main__":
    main()
