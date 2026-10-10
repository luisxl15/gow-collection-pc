"""Tradutor USSE -> SPIR-V sobre o rebaixamento de disasm.lower() (mesma semântica do interp.py).

Estado: cobre as classes F32 rebaixadas (vmad, vdp, vmul/vadd/vmin/vmax/vfrc, vrcp/vrsq/vlog/vexp,
nop/phas). A ligação de entradas e saídas ainda é provisória: PA recebe os atributos pelo índice de
recurso; SA recebe o storage buffer inteiro (set 0, binding 0); gl_Position ou a cor vêm de OUTPUT 0..3.
O mapeamento fiel vem dos contêineres e das varyings do GXP (próximo passo).
"""
from . import operands as O
from . import spirv as S
from .disasm import lower
from .interp import FPC0, FPC1

GLSL_PACKH, GLSL_UNPACKH = 58, 62
GLSL_FABS, GLSL_FMA, GLSL_INVSQRT, GLSL_LOG2, GLSL_EXP2, GLSL_FLOOR, GLSL_FMIN, GLSL_FMAX = 4, 50, 32, 30, 29, 8, 37, 40
OpFNegate = 127
BANK_VAR = {O.TEMP: "temp", O.PA: "pa", O.SA: "sa", O.OUT: "out", O.FPI: "fpi"}


class Unsupported(Exception):
    pass


class Translator:
    def __init__(self, prog):
        self.p = prog
        b = self.b = S.Builder()
        self.glsl = b.id()
        b.ext = b.inst(S.OpExtInstImport, self.glsl, *S._str_words("GLSL.std.450"))
        self.f32 = b.f32()
        self.i32 = b.i32()
        self.v4 = b.vec(self.f32, 4)
        self.v2 = b.vec(self.f32, 2)
        S.RegisterFile.SIZES = {"pa": 256, "sa": 512, "temp": 256, "out": 128, "fpi": 64}
        self.regs = S.RegisterFile(b)
        self.boolt = b._type("bool", 20)
        self.preds = b.variable(b.array(self.f32, 4), S.SCPrivate, "pred")
        self.cur_pred = None  # (id do bool) enquanto a instrução atual é predicada

    def const(self, v):
        return self.b.const(self.f32, float(v))

    def ext(self, code, *args):
        return self.b.emit(S.OpExtInst, self.f32, self.glsl, code, *args)

    def word_u(self, bank, n):
        return self.b.emit(S.OpBitcast, self.b.u32(), self.regs.read(bank, n))

    def read_half(self, bank, base, ch):
        v2 = self.b.emit(S.OpExtInst, self.v2, self.glsl, GLSL_UNPACKH, self.word_u(bank, base + ch // 2))
        return self.b.emit(S.OpCompositeExtract, self.f32, v2, ch % 2)

    def write_half(self, bank, n, half, val):
        b = self.b
        v2 = b.emit(S.OpExtInst, self.v2, self.glsl, GLSL_UNPACKH, self.word_u(bank, n))
        other = b.emit(S.OpCompositeExtract, self.f32, v2, 1 - half)
        pair = b.emit(S.OpCompositeConstruct, self.v2, *((val, other) if half == 0 else (other, val)))
        packed = b.emit(S.OpExtInst, b.u32(), self.glsl, GLSL_PACKH, pair)
        self.regs.write(bank, n, b.emit(S.OpBitcast, self.f32, packed))

    def read(self, op, mask, half=False):
        out = []
        for i in range(4):
            if not mask >> i & 1:
                continue
            ch = op.swizzle[i]
            if ch in (O.C0, O.C1, O.C2, O.CH):
                v = self.const({O.C0: 0, O.C1: 1, O.C2: 2, O.CH: 0.5}[ch])
            elif ch == O.UND:
                v = self.const(0)
            elif op.bank == O.FPCONST:
                v = self.const((FPC1 if ch == O.Y else FPC0)[op.num])
            elif op.bank == O.IMM:
                v = self.const(op.num)
            elif op.bank in BANK_VAR:
                base = 4 * op.num if op.bank == O.FPI else op.num
                if half and op.bank != O.FPI:  # FPINTERNAL é sempre F32
                    v = self.read_half(BANK_VAR[op.bank], base, ch)
                else:
                    v = self.regs.read(BANK_VAR[op.bank], base + ch)
            else:
                raise Unsupported("banco %s" % op.bank)
            if op.abs:
                v = self.ext(GLSL_FABS, v)
            if op.neg:
                v = self.b.emit(OpFNegate, self.f32, v)
            out.append(v)
        return out

    def write(self, d, mask, vals, half=False):
        if d.bank not in BANK_VAR:
            raise Unsupported("destino %s" % d.bank)
        base = 4 * d.num if d.bank == O.FPI else d.num
        k = 0
        for i in range(4):
            if mask >> i & 1:
                val = vals[k % len(vals)]
                if self.cur_pred is not None:
                    old = self.read_half(BANK_VAR[d.bank], base, i) if half and d.bank != O.FPI else \
                        self.regs.read(BANK_VAR[d.bank], base + i)
                    val = self.b.emit(169, self.f32, self.cur_pred, val, old)  # OpSelect
                if half and d.bank != O.FPI:
                    self.write_half(BANK_VAR[d.bank], base + i // 2, i % 2, val)
                else:
                    self.regs.write(BANK_VAR[d.bank], base + i, val)
                k += 1

    def instr(self, ins, second):
        low = lower(ins, second)
        if low is None:
            raise Unsupported(ins.op)
        mn, d, m, srcs = low
        if d is None:
            return
        try:
            pr = O.predicate(ins)
        except ValueError as e:
            raise Unsupported(str(e))
        self.cur_pred = None
        if pr is not None:
            pv = self.b.load(self.f32, self.b.chain(S.SCPrivate, self.f32, self.preds, self.b.const(self.i32, pr[0])))
            self.cur_pred = self.b.emit(182 if not pr[1] else 180, self.boolt, pv, self.const(0))  # != 0 / == 0
        try:
            self._instr(mn, d, m, srcs)
        finally:
            self.cur_pred = None

    def kill(self):
        """Descarte do fragmento: desvio para um bloco com OpKill (sob o predicado, se houver)."""
        b = self.b
        if self.cur_pred is None:
            cond = b.emit(180, self.boolt, self.const(0), self.const(0))  # sempre verdadeiro
        else:
            cond = self.cur_pred
        kill_lbl, merge_lbl = b.id(), b.id()
        b.body += b.inst(247, merge_lbl, 0)              # OpSelectionMerge
        b.body += b.inst(250, cond, kill_lbl, merge_lbl)  # OpBranchConditional
        b.body += b.inst(248, kill_lbl) + b.inst(252)     # OpLabel; OpKill
        b.body += b.inst(248, merge_lbl)

    def _instr(self, mn, d, m, srcs):
        if mn.startswith("vpck_"):
            _, sf, df = mn.split("_")
            vals = [self.read(op, 1, sf == "f16")[0] for op in srcs]
            self.write(d, m, vals, df == "f16")
            return
        if mn.startswith("vbw_"):
            b, u32 = self.b, self.b.u32()

            def word(op):
                if op.bank == O.IMM:
                    return b.const(u32, op.num)
                if op.bank not in BANK_VAR:
                    raise Unsupported("banco %s" % op.bank)
                base = 4 * op.num if op.bank == O.FPI else op.num
                return b.emit(S.OpBitcast, u32, self.regs.read(BANK_VAR[op.bank], base))
            a, x = word(srcs[0]), word(srcs[1])
            code = {"and": 199, "or": 197, "xor": 198, "shl": 196, "shr": 194, "asr": 195}[mn[4:]]
            r = b.emit(code, u32, a, x)
            self.write(d, 1, [b.emit(S.OpBitcast, self.f32, r)])
            return
        if mn == "kill":
            if not self.p.fragment:
                raise Unsupported("kill fora de fragment shader")
            self.kill()
            return
        half = mn.endswith(".f16")
        mn = mn.split(".")[0]
        b = self.b

        def rd(op, mk):
            return self.read(op, mk, half)
        if mn.startswith("vtst_"):
            _, alu, cmp = mn.split(".")[0].split("_")
            c = m.bit_length() - 1
            a, x = rd(srcs[0], 1 << c)[0], rd(srcs[1], 1 << c)[0]
            if alu == "sub":
                lhs, rhs = a, x
            else:
                rhs = self.const(0)
                if alu == "add":
                    lhs = b.emit(S.OpFAdd, self.f32, a, x)
                elif alu == "mul":
                    lhs = b.emit(S.OpFMul, self.f32, a, x)
                else:
                    lhs = self.ext(GLSL_FMIN if alu == "min" else GLSL_FMAX, a, x)
            op = {"ne": 182, "eq": 180, "lt": 184, "le": 188, "gt": 186, "ge": 190}[cmp]
            bit = b.emit(op, self.boolt, lhs, rhs)
            val = b.emit(169, self.f32, bit, self.const(1), self.const(0))
            if self.cur_pred is not None:
                old = b.load(self.f32, b.chain(S.SCPrivate, self.f32, self.preds, b.const(self.i32, d.num)))
                val = b.emit(169, self.f32, self.cur_pred, val, old)
            b.store(b.chain(S.SCPrivate, self.f32, self.preds, b.const(self.i32, d.num)), val)
            return
        if mn == "vmov":
            self.write(d, m, rd(srcs[0], m), half)
            return
        if mn.startswith("vmovc_"):
            c, a, x = (rd(s_, m) for s_ in srcs)
            op = {"eq": 180, "ne": 182, "lt": 184, "le": 188}[mn[6:]]  # OpFOrd*
            boolt = b._type("bool", 20)
            res = [b.emit(169, self.f32, b.emit(op, boolt, t, self.const(0)), p, q) for t, p, q in zip(c, a, x)]
            self.write(d, m, res, half)
            return
        if mn.startswith("vmad"):
            a, x, c = (rd(s, m) for s in srcs)
            res = [self.ext(GLSL_FMA, p, q, r) for p, q, r in zip(a, x, c)]
        elif mn.startswith("vdp"):
            n = 3 if mn == "vdp3" else 4
            full = (1 << n) - 1
            a, x = rd(srcs[0], full), rd(srcs[1], full)
            acc = b.emit(S.OpFMul, self.f32, a[0], x[0])
            for p, q in zip(a[1:], x[1:]):
                acc = self.ext(GLSL_FMA, p, q, acc)
            res = [acc]
        elif mn in ("vrcp", "vrsq", "vlog", "vexp"):
            v = rd(srcs[0], 1)[0]
            if mn == "vrcp":
                res = [b.emit(S.OpFDiv, self.f32, self.const(1), v)]
            else:
                res = [self.ext({"vrsq": GLSL_INVSQRT, "vlog": GLSL_LOG2, "vexp": GLSL_EXP2}[mn], v)]
        else:
            a, x = rd(srcs[0], m), rd(srcs[1], m)
            if mn == "vmul":
                res = [b.emit(S.OpFMul, self.f32, p, q) for p, q in zip(a, x)]
            elif mn == "vadd":
                res = [b.emit(S.OpFAdd, self.f32, p, q) for p, q in zip(a, x)]
            elif mn in ("vmin", "vmax"):
                res = [self.ext(GLSL_FMIN if mn == "vmin" else GLSL_FMAX, p, q) for p, q in zip(a, x)]
            elif mn == "vfrc":
                res = [b.emit(S.OpFSub, self.f32, p, self.ext(GLSL_FLOOR, p)) for p in a]
            else:
                raise Unsupported(mn)
        self.write(d, m, res, half)

    def build(self):
        b, p = self.b, self.p
        ifaces = []
        avars = []
        for k, q in enumerate(q for q in p.params if q["category"] == 0):
            v = b.variable(self.v4, S.SCInput, q["name"].replace(".", "_"))
            b.decorate(v, S.DecLocation, k)
            avars.append((v, q["resource"]))
            ifaces.append(v)
        rta = b.runtime_array(self.f32, 4)
        ub_t = b.struct("ub", rta)
        b.decorate(ub_t, S.DecBlock)
        b.member_decorate(ub_t, 0, S.DecOffset, 0)
        ub = b.variable(ub_t, S.SCStorageBuffer, "uniforms")
        b.decorate(ub, S.DecDescriptorSet, 0)
        b.decorate(ub, S.DecBinding, 0)
        b.decorate(ub, S.DecNonWritable)
        if p.fragment:
            outv = b.variable(self.v4, S.SCOutput, "color")
            b.decorate(outv, S.DecLocation, 0)
        else:
            pv_t = b.struct("pv", self.v4)
            b.decorate(pv_t, S.DecBlock)
            b.member_decorate(pv_t, 0, S.DecBuiltIn, S.BuiltInPosition)
            outv = b.variable(pv_t, S.SCOutput, "gl_PerVertex")
        ifaces.append(outv)
        main = b.id()
        b.body += b.inst(S.OpFunction, b.void(), main, 0, b.fn(b.void()))
        b.body += b.inst(S.OpLabel, b.id())
        for v, res in avars:
            val = b.load(self.v4, v)
            for c in range(4):
                if res + c < 256:
                    self.regs.write("pa", res + c, b.emit(S.OpCompositeExtract, self.f32, val, c))
        for w in range(min(512, max(p.default_uniform_words, 1))):
            ptr = b.chain(S.SCStorageBuffer, self.f32, ub, b.const(self.i32, 0), b.const(self.i32, w))
            self.regs.write("sa", w, b.load(self.f32, ptr))
        for ins in p.secondary:
            self.instr(ins, True)
        for ins in p.primary:
            self.instr(ins, False)
        res = b.emit(S.OpCompositeConstruct, self.v4, *[self.regs.read("out", c) for c in range(4)])
        if p.fragment:
            b.store(outv, res)
        else:
            b.store(b.chain(S.SCOutput, self.v4, outv, b.const(self.i32, 0)), res)
        b.body += b.inst(S.OpReturn) + b.inst(S.OpFunctionEnd)
        return b.finish(S.ExecFragment if p.fragment else S.ExecVertex, main, ifaces, frag=p.fragment)


def translate(prog):
    return Translator(prog).build()
