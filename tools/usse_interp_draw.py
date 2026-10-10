"""Teste de fidelidade do tradutor: executa o vertex shader real de um desenho capturado no
interpretador de referência (o mesmo rebaixamento que gera o SPIR-V) e confere se as posições de
saída caem dentro do viewport.

    python tools/usse_interp_draw.py <dir da captura> [índice do desenho] [vértices]

Entradas como o tradutor monta: PA[recurso + c] = atributo; SA pelos contêineres (14 = default
uniform buffer); endereços de uniform buffer = ldst_base_value (o default fica no deslocamento 0 da
memória do vldst, que aqui é o próprio default uniform buffer). Saída: OUTPUT 0..3 = posição de clip.
"""
import json
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(__file__)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "vitarecomp"))
import gxm_softraster as sr  # noqa: E402
from vitarecomp.usse import operands as O  # noqa: E402
from vitarecomp.usse.decode import Program  # noqa: E402
from vitarecomp.usse.interp import Machine  # noqa: E402


def main():
    d = sys.argv[1]
    which = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    nverts = int(sys.argv[3]) if len(sys.argv) > 3 else 200
    r = sr.Raster(d)
    draws = [json.loads(l) for l in open(os.path.join(d, "draws.jsonl"), encoding="utf-8")]
    dr = draws[which]
    size = struct.unpack("<I", r.mem.read(dr["vp"] + 8, 4).tobytes())[0]
    prog = Program.parse(r.mem.read(dr["vp"], size).tobytes())
    vu = r.blob(*dr["vu"]) or b""
    # atributos por registrador
    by_res = {}
    nv = min(dr["maxi"] + 1, nverts)
    for st_i, off, fmt, comps, reg in dr["attrs"]:
        st = dr["streams"][st_i]
        dt, norm = sr.ATTR_FMT[fmt]
        esz = np.dtype(dt).itemsize
        buf = np.frombuffer(r.bin, np.uint8, st["size"], st["off"])
        vals = np.zeros((nv, 4), np.float32)
        for v in range(nv):
            b = v * st["stride"] + off
            x = np.frombuffer(buf[b:b + comps * esz].tobytes(), dt).astype(np.float32)
            if norm:
                x /= float(np.iinfo(np.dtype(dt)).max)
            vals[v, :comps] = x
        by_res[reg] = (vals, comps)
    xo, xs, yo, ys, zo, zs = struct.unpack("<6f", struct.pack("<6I", *dr["vps"]))
    cont = {ci: (base, sz) for ci, _, base, sz in prog.containers}
    inside = errors = 0
    first_err = None
    samples = []
    for v in range(nv):
        m = Machine()
        m.memory = vu
        for res, (vals, comps) in by_res.items():
            m.r[O.PA][res:res + 4] = vals[v]
        if 14 in cont:
            base, sz = cont[14]
            words = np.frombuffer(vu[:4 * sz].ljust(4 * sz, b"\0"), np.float32)
            m.r[O.SA][base:base + sz] = words
        data_base = cont.get(19, (0, 0))[0]
        for reside, ldst_off, ldst_val in prog.uniform_buffers:
            m.r[O.SA][data_base + ldst_off:data_base + ldst_off + 1].view(np.uint32)[0] = ldst_val & 0xFFFFFFFF
        lit_base = cont.get(16, cont.get(19, (0, 0)))[0]
        for off, val in prog.literals:
            m.r[O.SA][lit_base + off] = val
        try:
            for ins in prog.secondary:
                m.step(ins, True)
            for ins in prog.primary:
                m.step(ins, False)
        except Exception as e:
            errors += 1
            first_err = first_err or "%s: %s" % (type(e).__name__, e)
            continue
        c = m.r[O.OUT][0:4].astype(np.float64)
        if c[3] > 0:
            x, y = xo + xs * c[0] / c[3], yo + ys * c[1] / c[3]
            inside += 0 <= x < 720 and 0 <= y < 408
            if len(samples) < 5:
                samples.append((round(x, 1), round(y, 1), round(c[3], 3)))
    print("desenho %d: %d vértices, %d na tela, %d com erro %s" % (which, nv, inside, errors, first_err or ""))
    print("amostras (x, y, w):", samples)


if __name__ == "__main__":
    main()
