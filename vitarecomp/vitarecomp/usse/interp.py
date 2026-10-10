"""Interpretador de referência do USSE sobre o rebaixamento de disasm.lower().

Serve de oráculo para o emissor SPIR-V (mesma semântica, executada em Python sobre dados reais
capturados do jogo). Instrução não rebaixada levanta NotImplementedError com o nome da classe.
"""
import json
import math
import os
import struct

import numpy as np

from . import operands as O
from .disasm import lower

_C = json.load(open(os.path.join(os.path.dirname(__file__), "constants.json")))
FPC0 = [struct.unpack("<f", struct.pack("<I", v))[0] for v in _C["f32_constant_table_bank_0"]]
FPC1 = [struct.unpack("<f", struct.pack("<I", v))[0] for v in _C["f32_constant_table_bank_1"]]
CONST_CH = {O.C0: 0.0, O.C1: 1.0, O.C2: 2.0, O.CH: 0.5}


class Machine:
    def __init__(self):
        self.r = {b: np.zeros(n, np.float32) for b, n in
                  ((O.TEMP, 256), (O.PA, 256), (O.SA, 512), (O.OUT, 128), (O.FPI, 64), (O.GLOBAL, 256))}

    def _half(self, bank, w, h, val=None):
        halves = self.r[bank][w:w + 1].view(np.float16)  # [baixa, alta]
        if val is None:
            return float(halves[h])
        halves[h] = val

    def read(self, op, mask, half=False):
        out = []
        for i in range(4):
            if not mask >> i & 1:
                continue
            ch = op.swizzle[i]
            if ch in CONST_CH:
                v = CONST_CH[ch]
            elif ch == O.UND:
                v = 0.0
            elif op.bank == O.FPCONST:
                v = (FPC1 if ch == O.Y else FPC0)[op.num]
            elif op.bank == O.IMM:
                v = float(op.num)
            else:
                base = 4 * op.num if op.bank == O.FPI else op.num
                if half and op.bank != O.FPI:
                    v = self._half(op.bank, base + ch // 2, ch % 2)
                else:
                    v = float(self.r[op.bank][base + ch])
            if op.abs:
                v = abs(v)
            if op.neg:
                v = -v
            out.append(v)
        return out

    def write(self, d, mask, vals, half=False):
        base = 4 * d.num if d.bank == O.FPI else d.num
        k = 0
        for i in range(4):
            if mask >> i & 1:
                if half and d.bank != O.FPI:
                    self._half(d.bank, base + i // 2, i % 2, vals[k % len(vals)])
                else:
                    self.r[d.bank][base + i] = vals[k % len(vals)]
                k += 1

    def step(self, ins, second=False):
        low = lower(ins, second)
        if low is None:
            raise NotImplementedError(ins.op)
        mn, d, m, srcs = low
        if d is None:
            return
        half = mn.endswith(".f16")
        mn = mn.split(".")[0]

        def rd(op, mk):
            return self.read(op, mk, half)
        if mn.startswith("vmad"):
            a, b, c = (rd(s, m) for s in srcs)
            res = [x * y + z for x, y, z in zip(a, b, c)]
        elif mn.startswith("vdp"):
            n = 3 if mn == "vdp3" else 4
            full = (1 << n) - 1
            a, b = rd(srcs[0], full), rd(srcs[1], full)
            res = [sum(x * y for x, y in zip(a, b))]
        elif mn in ("vrcp", "vrsq", "vlog", "vexp"):
            x = rd(srcs[0], 1)[0]
            res = [{"vrcp": lambda v: 1.0 / v if v else math.inf,
                    "vrsq": lambda v: 1.0 / math.sqrt(v) if v > 0 else math.inf,
                    "vlog": lambda v: math.log2(v) if v > 0 else -math.inf,  # base 2: a validar
                    "vexp": lambda v: 2.0 ** v}[mn](x)]
        else:
            base = mn.split(".")[0]
            if base == "vdp":
                a, b = rd(srcs[0], 0xF), rd(srcs[1], 0xF)
                res = [sum(x * y for x, y in zip(a, b))]
            else:
                a, b = rd(srcs[0], m), rd(srcs[1], m)
                fn = {"vmul": lambda x, y: x * y, "vadd": lambda x, y: x + y, "vmin": min, "vmax": max,
                      "vfrc": lambda x, y: x - math.floor(x)}.get(base)
                if fn is None:
                    raise NotImplementedError(mn)
                res = [fn(x, y) for x, y in zip(a, b)]
        self.write(d, m, res, half)
