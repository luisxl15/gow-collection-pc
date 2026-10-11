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

GLSL_PACKH, GLSL_UNPACKH, GLSL_PACKUNORM4X8 = 58, 62, 55
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
        self.rep = O.RepeatState()

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
        if ins.op == "smlsi":
            self.rep.smlsi(ins.f)
            return
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

    def memory(self):
        """Storage buffer (set 0, binding 1) com a memória que o vldst lê, em palavras de 32 bits."""
        if getattr(self, "_mem", None) is None:
            b = self.b
            rta = b.runtime_array(b.u32(), 4)
            st = b.struct("mem", rta)
            b.decorate(st, S.DecBlock)
            b.member_decorate(st, 0, S.DecOffset, 0)
            self._mem = b.variable(st, S.SCStorageBuffer, "memory")
            b.decorate(self._mem, S.DecDescriptorSet, 2 if self.p.fragment else 0)
            b.decorate(self._mem, S.DecBinding, 1)
            b.decorate(self._mem, S.DecNonWritable)
        return self._mem

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

    def dual_op(self, op):
        b, h = self.b, op["half"]
        a = [self.read(s_, op["smask"], h and s_.bank != O.FPI) for s_ in op["srcs"]]
        n = op["name"]
        F = self.f32
        if n in ("vmad", "fmad"):
            return [b.emit(S.OpFAdd, F, b.emit(S.OpFMul, F, x, y), z) for x, y, z in zip(*a)]
        if n in ("vmul", "fmul"):
            return [b.emit(S.OpFMul, F, x, y) for x, y in zip(*a)]
        if n in ("vadd", "fadd"):
            return [b.emit(S.OpFAdd, F, x, y) for x, y in zip(*a)]
        if n == "vmov":
            return a[0]
        if n in ("vdp", "vssq"):
            xs, ys = (a[0], a[1]) if n == "vdp" else (a[0], a[0])
            acc = b.emit(S.OpFMul, F, xs[0], ys[0])
            for x, y in zip(xs[1:], ys[1:]):
                acc = self.ext(GLSL_FMA, x, y, acc)
            return [acc]
        if n == "frcp":
            return [b.emit(S.OpFDiv, F, self.const(1), a[0][0])]
        if n == "frsq":
            return [self.ext(GLSL_INVSQRT, a[0][0])]
        if n == "fexp":
            return [self.ext(GLSL_EXP2, a[0][0])]
        if n == "flog":
            return [self.ext(GLSL_LOG2, a[0][0])]
        if n == "fsubflr":
            return [b.emit(S.OpFSub, F, a[0][0], self.ext(GLSL_FLOOR, a[1][0]))]
        raise Unsupported(n)

    def _vdp_once(self, mn, d, m, srcs, rd):
        n = 3 if mn == "vdp3" else 4
        full = (1 << n) - 1
        a, x = rd(srcs[0], full), rd(srcs[1], full)
        acc = self.b.emit(S.OpFMul, self.f32, a[0], x[0])
        for p_, q in zip(a[1:], x[1:]):
            acc = self.ext(GLSL_FMA, p_, q, acc)
        self.write(d, m, [acc])

    def _instr(self, mn, d, m, srcs):
        if mn == "vdual":
            results = [self.dual_op(op) for op in srcs]  # as duas leem antes de escrever
            for op, res in zip(srcs, results):
                self.write(op["dest"], op["mask"], res, op["half"] and op["dest"].bank != O.FPI)
            return
        if mn.startswith("vpck_"):
            parts = mn.split("_")
            sf, df = parts[1], parts[2]
            if len(parts) == 4:  # repetido: multiplicadores 1 no destino e 2 nas fontes (float)
                for r in range(int(parts[3][3:]) + 1):
                    picks = [O.shifted(op, self.rep.offset(getattr(op, "src_index", 1), r, op.bank, 2)) for op in srcs]
                    dst = O.shifted(d, self.rep.offset(3, r, d.bank, 1))
                    self.write(dst, m, [self.read(op, 1, sf == "f16")[0] for op in picks], df == "f16")
                return
            vals = [self.read(op, 1, sf == "f16")[0] for op in srcs]
            if df == "u16":
                b, u32 = self.b, self.b.u32()
                k = 0
                for i in range(4):
                    if m >> i & 1:
                        if d.bank not in BANK_VAR:
                            raise Unsupported("destino %s" % d.bank)
                        n = d.num + i // 2
                        v = b.emit(109, u32, vals[k])  # OpConvertFToU
                        v = b.emit(199, u32, v, b.const(u32, 0xFFFF))
                        w = b.emit(S.OpBitcast, u32, self.regs.read(BANK_VAR[d.bank], n))
                        keep = b.emit(199, u32, w, b.const(u32, 0xFFFF0000 if i % 2 == 0 else 0x0000FFFF))
                        if i % 2:
                            v = b.emit(196, u32, v, b.const(u32, 16))
                        nw = b.emit(S.OpBitcast, self.f32, b.emit(197, u32, keep, v))
                        if self.cur_pred is not None:
                            nw = b.emit(169, self.f32, self.cur_pred, nw, self.regs.read(BANK_VAR[d.bank], n))
                        self.regs.write(BANK_VAR[d.bank], n, nw)
                        k += 1
                return
            self.write(d, m, vals, df == "f16")
            return
        if mn.startswith("i32mad_"):
            b, u32 = self.b, self.b.u32()
            signed = mn.endswith("_s")

            def part(op):
                if op.bank == O.IMM:
                    return b.const(u32, op.num)
                if op.bank not in BANK_VAR:
                    raise Unsupported("banco %s" % op.bank)
                w = b.emit(S.OpBitcast, u32, self.regs.read(BANK_VAR[op.bank], op.num))
                h = op.swizzle[0]
                if h == 9:
                    return w
                if signed:  # extensão de sinal da metade escolhida
                    t = b.emit(196, u32, w, b.const(u32, 16 - 16 * h))  # ShiftLeftLogical
                    return b.emit(195, u32, t, b.const(u32, 16))         # ShiftRightArithmetic
                t = b.emit(194, u32, w, b.const(u32, 16 * h))           # ShiftRightLogical
                return b.emit(199, u32, t, b.const(u32, 0xFFFF))         # BitwiseAnd
            r = b.emit(S.OpIAdd, u32, b.emit(S.OpIMul, u32, part(srcs[0]), part(srcs[1])), part(srcs[2]))
            self.write(d, 1, [b.emit(S.OpBitcast, self.f32, r)])
            return
        if mn == "vldst":
            b, u32 = self.b, self.b.u32()

            def word(op):
                if op.bank == O.IMM:
                    return b.const(u32, op.num)
                if op.bank not in BANK_VAR:
                    raise Unsupported("banco %s" % op.bank)
                return b.emit(S.OpBitcast, u32, self.regs.read(BANK_VAR[op.bank], op.num))
            addr = word(srcs[0])
            for op in srcs[1:]:
                addr = b.emit(S.OpIAdd, u32, addr, word(op))
            idx = b.emit(194, u32, addr, b.const(u32, 2))  # bytes -> palavras
            vals = []
            for k in range(m):
                i = b.emit(S.OpIAdd, u32, idx, b.const(u32, k))
                w = b.load(u32, b.chain(S.SCStorageBuffer, u32, self.memory(), b.const(self.i32, 0), i))
                vals.append(b.emit(S.OpBitcast, self.f32, w))
            for k, v in enumerate(vals):
                if d.bank not in BANK_VAR or d.num + k >= S.RegisterFile.SIZES[BANK_VAR[d.bank]]:
                    raise Unsupported("destino de vldst")
                old = self.regs.read(BANK_VAR[d.bank], d.num + k) if self.cur_pred is not None else None
                self.regs.write(BANK_VAR[d.bank], d.num + k,
                                v if old is None else b.emit(169, self.f32, self.cur_pred, v, old))
            return
        if mn.startswith("vbw_") and mn.count("_") == 2:
            parts = mn.split("_")
            for r in range(int(parts[2][3:]) + 1):
                sh = lambda op, i: op if op.bank == O.IMM else O.shifted(op, self.rep.offset(i, r, op.bank, 1))
                self._instr("vbw_" + parts[1], O.shifted(d, self.rep.offset(3, r, d.bank, 1)), 1, [sh(srcs[0], 1), sh(srcs[1], 2)])
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
        if mn.startswith("vmovrep"):
            for r in range(int(mn[7:]) + 1):
                src = O.shifted(srcs[0], self.rep.offset(1, r, srcs[0].bank, 2))
                dst = O.shifted(d, self.rep.offset(3, r, d.bank, 2))
                self.write(dst, m, rd(src, m), half)
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
        if "_ext" in mn and mn.startswith("vdp"):
            base_mn, n_rep = mn.split("_ext")
            mask = m
            for r in range(int(n_rep) + 1):
                s1 = srcs[0] if srcs[0].bank == O.FPI else O.shifted(srcs[0], 4 * r)
                s1.swizzle, s1.neg, s1.abs = srcs[0].swizzle, srcs[0].neg, srcs[0].abs
                self._vdp_once(base_mn, d, mask, [s1, srcs[1]], rd)
                mask = ((mask << 1) | (mask >> 3)) & 0xF
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

    # ------------------------------------------------------------------ E/S
    # Contrato com o renderizador (Vulkan):
    #   vértice: atributos em location k (ordem dos parâmetros de categoria 0); saídas em gl_Position e
    #     nas locations da varying (cor0 1, cor1 2, fog 3, texcoordN 4+N), vec4 cada;
    #   fragmento: as mesmas locations como entrada; texturas em set 1, binding = recurso do sampler;
    #     cor em location 0;
    #   uniforms: storage buffer binding 0 (default uniform buffer) e memória do vldst em binding 1, no set 0
    #     (vértice) ou 2 (fragmento): os conteúdos são diferentes e o layout do pipeline é um só.
    def _pack_words(self, vals, dtype):
        """Valores float -> palavras de 32 bits como o hardware põe nos PA (f32, f16 aos pares, u8 x4)."""
        b, u32 = self.b, self.b.u32()
        if dtype == "f32":
            return list(vals)
        if dtype == "f16":
            out = []
            for k in range(0, len(vals), 2):
                second = vals[k + 1] if k + 1 < len(vals) else self.const(0)
                pair = b.emit(S.OpCompositeConstruct, self.v2, vals[k], second)
                out.append(b.emit(S.OpBitcast, self.f32, b.emit(S.OpExtInst, u32, self.glsl, GLSL_PACKH, pair)))
            return out
        if dtype in ("u8", "int"):
            v = list(vals) + [self.const(0)] * (4 - len(vals))
            vec = b.emit(S.OpCompositeConstruct, self.v4, *v[:4])
            return [b.emit(S.OpBitcast, self.f32, b.emit(S.OpExtInst, u32, self.glsl, GLSL_PACKUNORM4X8, vec))]
        raise Unsupported("varying " + dtype)

    def _write_pa(self, pa, regs, words):
        for k, w in enumerate(words[:regs]):
            if pa + k < 256:
                self.regs.write("pa", pa + k, w)

    def _frag_inputs(self, ifaces):
        b, p = self.b, self.p
        locs = {}

        def input_var(loc, name):
            if loc not in locs:
                if loc == -1:
                    v = b.variable(self.v4, S.SCInput, "gl_FragCoord")
                    b.decorate(v, S.DecBuiltIn, S.BuiltInFragCoord)
                else:
                    v = b.variable(self.v4, S.SCInput, name)
                    b.decorate(v, S.DecLocation, loc)
                ifaces.append(v)
                locs[loc] = v
            return locs[loc]

        samplers = {}
        for inp in p.frag_inputs or []:
            if inp["kind"] == "iter":
                if inp["loc"] < -1:
                    raise Unsupported("varying " + inp["name"])
                val = b.load(self.v4, input_var(inp["loc"], inp["name"]))
                comps = [b.emit(S.OpCompositeExtract, self.f32, val, c) for c in range(inp["comps"])]
                self._write_pa(inp["pa"], inp["regs"], self._pack_words(comps, inp["dtype"]))
                continue
            if inp["coord"] > 9:
                raise Unsupported("textura com coordenada de sprite")
            simg = b.sampled_image()
            if inp["sampler"] not in samplers:
                sv = b.variable(simg, S.SCUniformConstant, "tex%d" % inp["sampler"])
                b.decorate(sv, S.DecDescriptorSet, 1)
                b.decorate(sv, S.DecBinding, inp["sampler"])
                samplers[inp["sampler"]] = sv
            coord = b.load(self.v4, input_var(4 + inp["coord"], "texcoord%d" % inp["coord"]))
            img = b.load(simg, samplers[inp["sampler"]])
            if inp["proj"] is None:
                c2 = b.emit(S.OpVectorShuffle, self.v2, coord, coord, 0, 1)
                texel = b.emit(S.OpImageSampleImplicitLod, self.v4, img, c2)
            else:
                v3 = b.vec(self.f32, 3)
                c3 = b.emit(S.OpVectorShuffle, v3, coord, coord, 0, 1, inp["proj"])
                texel = b.emit(S.OpImageSampleProjImplicitLod, self.v4, img, c3)
            comps = [b.emit(S.OpCompositeExtract, self.f32, texel, c) for c in range(4)]
            self._write_pa(inp["pa"], inp["regs"], self._pack_words(comps, inp["store"]))

    def _frag_color(self):
        p = self.p
        bank, base = ("out", 0) if p.native_color else ("pa", p.output_start)
        n = min(p.output_components or 4, 4)
        if p.output_type == 1:  # F16: componentes aos pares por palavra
            vals = [self.read_half(bank, base, c) for c in range(n)]
        elif p.output_type == 0:
            vals = [self.regs.read(bank, base + c) for c in range(n)]
        else:
            raise Unsupported("saída de fragmento tipo %d" % p.output_type)
        while len(vals) < 4:
            vals.append(self.const(1 if len(vals) == 3 else 0))
        return self.b.emit(S.OpCompositeConstruct, self.v4, *vals)

    def build(self):
        b, p = self.b, self.p
        ifaces = []
        avars = []
        if not p.fragment:
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
        b.decorate(ub, S.DecDescriptorSet, 2 if p.fragment else 0)
        b.decorate(ub, S.DecBinding, 0)
        b.decorate(ub, S.DecNonWritable)
        outs = []  # (variável, registrador OUTPUT inicial, componentes)
        if p.fragment:
            outv = b.variable(self.v4, S.SCOutput, "color")
            b.decorate(outv, S.DecLocation, 0)
            ifaces.append(outv)
        else:
            pv_t = b.struct("pv", self.v4)
            b.decorate(pv_t, S.DecBlock)
            b.member_decorate(pv_t, 0, S.DecBuiltIn, S.BuiltInPosition)
            outv = b.variable(pv_t, S.SCOutput, "gl_PerVertex")
            ifaces.append(outv)
            for name, loc, n, reg in p.vertex_outputs or [("position", -1, 4, 0)]:
                if loc < 0:
                    continue  # posição vai no gl_PerVertex; psize fica de fora (pontos não usados)
                v = b.variable(self.v4, S.SCOutput, name)
                b.decorate(v, S.DecLocation, loc)
                ifaces.append(v)
                outs.append((v, reg, n))
        main = b.id()
        b.body += b.inst(S.OpFunction, b.void(), main, 0, b.fn(b.void()))
        b.body += b.inst(S.OpLabel, b.id())
        for v, res in avars:
            val = b.load(self.v4, v)
            for c in range(4):
                if res + c < 256:
                    self.regs.write("pa", res + c, b.emit(S.OpCompositeExtract, self.f32, val, c))
        if p.fragment:
            self._frag_inputs(ifaces)
        # Entradas em SA pelas tabelas do GXP: contêiner 14 = default uniform buffer copiado a partir
        # de base_sa_offset; infos de uniform buffer põem em SA (base do contêiner 19 + ldst_base_offset)
        # o endereço em bytes do buffer na memória do vldst (o default fica em 0) + ldst_base_value;
        # literais vão para o contêiner 16 (ou 19).
        cont = {ci: (base, size) for ci, _, base, size in (p.containers or [])}
        if 14 in cont:
            base, size = cont[14]
            for w in range(min(size, 512 - base)):
                ptr = b.chain(S.SCStorageBuffer, self.f32, ub, b.const(self.i32, 0), b.const(self.i32, w))
                self.regs.write("sa", base + w, b.load(self.f32, ptr))
        data_base = cont.get(19, (0, 0))[0]
        for reside, ldst_off, ldst_val in (p.uniform_buffers or []):
            n = data_base + ldst_off
            if n < 512:
                addr = b.const(b.u32(), ldst_val & 0xFFFFFFFF)  # buffer 0 = default no deslocamento 0
                self.regs.write("sa", n, b.emit(S.OpBitcast, self.f32, addr))
        lit_base = cont.get(16, cont.get(19, (0, 0)))[0]
        for off, val in (p.literals or []):
            if lit_base + off < 512:
                self.regs.write("sa", lit_base + off, self.const(val))
        for ins in p.secondary:
            self.instr(ins, True)
        for ins in p.primary:
            self.instr(ins, False)
        if p.fragment:
            b.store(outv, self._frag_color())
        else:
            res = b.emit(S.OpCompositeConstruct, self.v4, *[self.regs.read("out", c) for c in range(4)])
            b.store(b.chain(S.SCOutput, self.v4, outv, b.const(self.i32, 0)), res)
            for v, reg, n in outs:
                vals = [self.regs.read("out", reg + c) for c in range(n)] + [self.const(0)] * (4 - n)
                b.store(v, b.emit(S.OpCompositeConstruct, self.v4, *vals))
        b.body += b.inst(S.OpReturn) + b.inst(S.OpFunctionEnd)
        return b.finish(S.ExecFragment if p.fragment else S.ExecVertex, main, ifaces, frag=p.fragment)


def translate(prog):
    return Translator(prog).build()
