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
        self.pred = [False] * 4
        self.killed = False

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
        pr = O.predicate(ins)
        if pr is not None and self.pred[pr[0]] == pr[1]:
            return  # predicado falso: a instrução não executa
        half = mn.endswith(".f16")
        mn = mn.split(".")[0]

        def rd(op, mk):
            return self.read(op, mk, half)
        if mn == "kill":
            self.killed = True
            return
        if mn.startswith("vpck_"):
            _, sf, df = mn.split("_")
            vals = [self.read(op, 1, sf == "f16")[0] for op in srcs]
            self.write(d, m, vals, df == "f16")
            return
        if mn.startswith("vtst_"):
            _, alu, cmp = mn.split("_")
            c = m.bit_length() - 1
            a, b = rd(srcs[0], 1 << c), rd(srcs[1], 1 << c)
            if alu == "sub":
                lhs, rhs = a[0], b[0]
            else:
                lhs = {"add": a[0] + b[0], "mul": a[0] * b[0], "min": min(a[0], b[0]), "max": max(a[0], b[0])}[alu]
                rhs = 0.0
            self.pred[d.num] = {"ne": lhs != rhs, "eq": lhs == rhs, "lt": lhs < rhs, "le": lhs <= rhs,
                                "gt": lhs > rhs, "ge": lhs >= rhs}[cmp]
            return
        if mn == "vmov":
            self.write(d, m, rd(srcs[0], m), half)
            return
        if mn.startswith("vmovc_"):
            c, a, b = (rd(s_, m) for s_ in srcs)
            test = {"eq": lambda v: v == 0, "ne": lambda v: v != 0, "lt": lambda v: v < 0, "le": lambda v: v <= 0}[mn[6:]]
            self.write(d, m, [x if test(t) else y for t, x, y in zip(c, a, b)], half)
            return
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
