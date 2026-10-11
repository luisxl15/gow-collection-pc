"""Advanced SIMD (NEON) -> C.

Every instruction is lowered to a C block that copies its source D registers
into local uint64_t arrays, computes lane by lane through typed pointers, and
copies the destination back (so overlapping source/destination registers are
handled exactly like the hardware).

Float lanes follow the ARM "standard FPSCR" that NEON always uses: inputs and
results are flushed to zero and every NaN result is the default NaN.
Saturation (FPSCR.QC) is computed but not recorded.
"""
from __future__ import annotations

import re

from capstone import arm as A

CT = {"i8": "uint8_t", "s8": "int8_t", "u8": "uint8_t", "p8": "uint8_t", "8": "uint8_t",
      "i16": "uint16_t", "s16": "int16_t", "u16": "uint16_t", "p16": "uint16_t", "16": "uint16_t",
      "i32": "uint32_t", "s32": "int32_t", "u32": "uint32_t", "32": "uint32_t", "f32": "float",
      "i64": "uint64_t", "s64": "int64_t", "u64": "uint64_t", "64": "uint64_t"}



def is_core(name: str) -> bool:
    """Registrador de núcleo, inclusive pelos apelidos que o capstone usa (sb=r9, sl=r10, fp=r11, ip=r12)."""
    return (name[0] == "r" and name[1:].isdigit()) or name in ("sb", "sl", "fp", "ip", "sp", "lr", "pc")

class NeonError(Exception):
    pass


def bits(dt: str) -> int:
    return int(re.sub(r"\D", "", dt) or 0)


def signed(dt: str) -> str:
    return "int%d_t" % bits(dt)


def unsigned(dt: str) -> str:
    return "uint%d_t" % bits(dt)


def wider(dt: str) -> str:
    return dt[0] + str(bits(dt) * 2) if dt[0] in "isup" else str(bits(dt) * 2)


def narrower(dt: str) -> str:
    return dt[0] + str(bits(dt) // 2) if dt[0] in "isup" else str(bits(dt) // 2)


class Neon:
    def __init__(self, em, i):
        self.em, self.i = em, i
        self.cs = i.cs
        self.ops = list(i.cs.operands)
        parts = i.cs.mnemonic.split(".")
        self.base = parts[0]
        self.dts = parts[1:]
        self.dt = self.dts[0] if self.dts else ""
        self.lines: list[str] = []
        self.k = 0

    # ---- registers ----
    def reg(self, op) -> tuple[int, int, int]:
        """(first D index, number of D regs, scalar index or -1)."""
        name = self.cs.reg_name(op.reg)
        if name[0] == "q":
            return 2 * int(name[1:]), 2, -1
        if name[0] == "d":
            return int(name[1:]), 1, op.vector_index
        raise NeonError(f"not a SIMD register: {name}")

    def load(self, d: int, n: int, name: str | None = None) -> str:
        self.k += 1
        name = name or f"v{self.k}"
        init = ", ".join(f"ctx->d[{d + j}]" for j in range(n))
        self.lines.append(f"uint64_t {name}[2] = {{{init}{', 0' if n == 1 else ''}}};")
        return name

    def store(self, name: str, d: int, n: int):
        for j in range(n):
            self.lines.append(f"ctx->d[{d + j}] = {name}[{j}];")

    def out(self) -> list[str]:
        return ["{"] + ["  " + l for l in self.lines] + ["}"]

    def lanes(self, nd: int, dt: str) -> int:
        return nd * 64 // bits(dt)

    @staticmethod
    def el(arr: str, ty: str, k: str) -> str:
        return f"(({ty} *){arr})[{k}]"

    # ---- dispatch ----
    def translate(self) -> list[str]:
        b = self.base
        h = getattr(self, "op_" + b, None)
        if h is None:
            if b in BIN_INT or b in BIN_FLOAT or b in CMP or b in BITWISE:
                h = self.op_binary
            elif b in UNARY:
                h = self.op_unary
            else:
                raise NeonError(f"NEON '{self.cs.mnemonic} {self.cs.op_str}' not supported yet")
        h()
        return self.out()

    # ---- elementwise -----------------------------------------------------------
    def op_binary(self):
        b, dt = self.base, self.dt
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        third = self.ops[2]
        A_ = self.load(a, nd)
        if third.type in (A.ARM_OP_IMM, A.ARM_OP_FP):      # compare against #0
            Bv = None
        else:
            bb, _, bidx = self.reg(third)
            Bv = ("scalar", self.load(bb, 1), bidx) if bidx != -1 else ("vec", self.load(bb, nd), None)
        D = self.load(d, nd)
        L = self.lanes(nd, dt if b not in BITWISE else "64")
        if b in BITWISE:
            ty = "uint64_t"
            bexpr = self.el(Bv[1], ty, "k")
            expr = BITWISE[b].format(a=self.el(A_, ty, "k"), b=bexpr, d=self.el(D, ty, "k"))
            self.lines.append(f"for (int k = 0; k < {nd}; k++) {self.el(D, ty, 'k')} = {expr};")
            self.store(D, d, nd)
            return
        ty = CT[dt]
        src_ty = ty
        if b in CMP and dt.startswith("i"):
            src_ty = unsigned(dt)
        bval = "0" if Bv is None else (self.el(Bv[1], ty, "k") if Bv[0] == "vec" else self.el(Bv[1], ty, str(Bv[2])))
        a_e, b_e, d_e = self.el(A_, ty, "k"), bval, self.el(D, ty, "k")
        if dt == "f32":
            a_e, b_e, d_e = f"arc_nftz({a_e})", (f"arc_nftz({b_e})" if Bv else "0.0f"), f"arc_nftz({d_e})"
            if b in CMP:
                expr = CMP[b].format(a=a_e, b=b_e)
                self.lines.append(f"for (int k = 0; k < {L}; k++) (({unsigned('32')} *){D})[k] = ({expr}) ? 0xffffffffu : 0;")
            else:
                expr = BIN_FLOAT[b].format(a=a_e, b=b_e, d=d_e)
                self.lines.append(f"for (int k = 0; k < {L}; k++) {self.el(D, 'float', 'k')} = {expr};")
            self.store(D, d, nd)
            return
        if b in CMP:
            mask = "(" + unsigned(dt) + ")~0ull"
            expr = CMP[b].format(a=f"({src_ty}){a_e}", b=f"({src_ty}){b_e}")
            self.lines.append(f"for (int k = 0; k < {L}; k++) (({unsigned(dt)} *){D})[k] = ({expr}) ? {mask} : 0;")
            self.store(D, d, nd)
            return
        tmpl = BIN_INT.get(b)
        if tmpl is None:
            raise NeonError(f"integer {b} not supported")
        n = bits(dt)
        expr = tmpl(a_e, b_e, d_e, dt, n)
        self.lines.append(f"for (int k = 0; k < {L}; k++) {self.el(D, ty, 'k')} = ({ty})({expr});")
        self.store(D, d, nd)

    def op_unary(self):
        b, dt = self.base, self.dt
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        A_ = self.load(a, nd)
        D = self.load(d, nd)
        if b in ("vmvn",):
            self.lines.append(f"for (int k = 0; k < {nd}; k++) {D}[k] = ~{A_}[k];")
            self.store(D, d, nd)
            return
        ty = CT[dt] if dt else "uint8_t"
        L = self.lanes(nd, dt or "8")
        a_e = self.el(A_, ty, "k")
        expr = UNARY[b](a_e, dt, bits(dt or "8"))
        self.lines.append(f"for (int k = 0; k < {L}; k++) {self.el(D, ty, 'k')} = {expr};")
        self.store(D, d, nd)

    # ---- moves / dup ----
    def op_vmov(self):
        ops = self.ops
        k0 = ops[0]
        if len(ops) == 2 and ops[1].type in (A.ARM_OP_IMM, A.ARM_OP_FP):
            d, nd, _ = self.reg(k0)
            if ops[1].type == A.ARM_OP_FP:
                val = f"arc_fbits({float(ops[1].fp)!r}f)"
                ty = "uint32_t"
            else:
                ty = unsigned(self.dt if self.dt not in ("f32",) else "32")
                val = f"({ty}){ops[1].imm & 0xFFFFFFFFFFFFFFFF:#x}ull"
                if self.dt == "i64":
                    val = f"{_expand_i64(bytes(self.cs.bytes))}ull"
            D = self.load(d, nd)
            L = nd * 64 // int(ty[4:-2])
            self.lines.append(f"for (int k = 0; k < {L}; k++) (({ty} *){D})[k] = {val};")
            self.store(D, d, nd)
            return
        names = [self.cs.reg_name(o.reg) for o in ops]
        if is_core(names[0]):  # vmov.32 r0, d0[1]
            d, _, idx = self.reg(ops[1])
            ty = CT.get(self.dt, "uint32_t")
            V = self.load(d, 1)
            ext = f"(uint32_t)(int32_t)(({ty} *){V})[{idx}]" if ty.startswith("int") else f"(uint32_t)(({ty} *){V})[{idx}]"
            self.lines.append(f"{self.em.w(self.em.regnum(self.i, ops[0]))} = {ext};")
            return
        if is_core(names[1]) and ops[0].vector_index != -1:  # vmov.8 d0[3], r1
            d, _, idx = self.reg(ops[0])
            ty = unsigned(self.dt or "32")
            V = self.load(d, 1)
            self.lines.append(f"(({ty} *){V})[{idx}] = ({ty}){self.em.r(self.i, self.em.regnum(self.i, ops[1]))};")
            self.store(V, d, 1)
            return
        raise NeonError(f"vmov form '{self.cs.op_str}'")

    def op_vmvn(self):
        if self.ops[1].type == A.ARM_OP_IMM:
            d, nd, _ = self.reg(self.ops[0])
            ty = unsigned(self.dt)
            D = self.load(d, nd)
            L = self.lanes(nd, self.dt)
            self.lines.append(f"for (int k = 0; k < {L}; k++) (({ty} *){D})[k] = ({ty})~({ty}){self.ops[1].imm & 0xFFFFFFFF:#x}u;")
            self.store(D, d, nd)
            return
        self.op_unary()

    def op_vorr(self):
        if self.ops[-1].type == A.ARM_OP_IMM:
            return self._bitimm("|")
        self.op_binary()

    def op_vbic(self):
        if self.ops[-1].type == A.ARM_OP_IMM:
            return self._bitimm("& ~")
        self.op_binary()

    def _bitimm(self, op):
        d, nd, _ = self.reg(self.ops[0])
        ty = unsigned(self.dt)
        D = self.load(d, nd)
        L = self.lanes(nd, self.dt)
        self.lines.append(f"for (int k = 0; k < {L}; k++) (({ty} *){D})[k] = (({ty} *){D})[k] {op}({ty}){self.ops[-1].imm & 0xFFFFFFFF:#x}u;")
        self.store(D, d, nd)

    def op_vdup(self):
        d, nd, _ = self.reg(self.ops[0])
        ty = unsigned(self.dt)
        D = self.load(d, nd)
        src = self.ops[1]
        if is_core(self.cs.reg_name(src.reg)):
            val = f"({ty}){self.em.r(self.i, self.em.regnum(self.i, src))}"
        else:
            s, _, idx = self.reg(src)
            V = self.load(s, 1)
            val = f"(({ty} *){V})[{idx}]"
        L = self.lanes(nd, self.dt)
        self.lines.append(f"{{ {ty} x = {val}; for (int k = 0; k < {L}; k++) (({ty} *){D})[k] = x; }}")
        self.store(D, d, nd)

    def op_vswp(self):
        a, n, _ = self.reg(self.ops[0])
        b, _, _ = self.reg(self.ops[1])
        A_, B_ = self.load(a, n), self.load(b, n)
        self.store(B_, a, n)
        self.store(A_, b, n)

    # ---- widening / narrowing ----
    def op_vmovl(self):
        d, _, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        A_ = self.load(a, 1)
        D = self.load(d, 2)
        it, ot = CT[self.dt], CT[wider(self.dt)]
        L = 64 // bits(self.dt)
        self.lines.append(f"for (int k = 0; k < {L}; k++) (({ot} *){D})[k] = ({ot})(({it} *){A_})[k];")
        self.store(D, d, 2)

    def op_vmovn(self):
        self._narrow(lambda x, it, ot: f"({ot}){x}", self.dt)

    def op_vqmovn(self):
        self._narrow(lambda x, it, ot: _sat(x, narrower(self.dt), "s" if self.dt[0] == "s" else "u"), self.dt)

    def op_vqmovun(self):
        self._narrow(lambda x, it, ot: _sat(x, "u" + str(bits(self.dt) // 2), "u"), self.dt)

    def _narrow(self, fn, dt, src_override=None):
        d, _, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        A_ = self.load(a, 2)
        D = self.load(d, 1)
        it = CT[dt] if dt[0] != "i" else unsigned(dt)
        if dt[0] == "s":
            it = signed(dt)
        ot = unsigned(narrower(dt))
        L = 64 // (bits(dt) // 2)
        x = src_override or f"(({it} *){A_})[k]"
        self.lines.append(f"for (int k = 0; k < {L}; k++) (({ot} *){D})[k] = ({ot})({fn(x, it, ot)});")
        self.store(D, d, 1)

    def op_vshrn(self):
        sh = self.ops[2].imm
        self._narrow(lambda x, it, ot: f"({x} >> {sh})", "u" + str(bits(self.dt)))

    def op_vrshrn(self):
        sh = self.ops[2].imm
        n = bits(self.dt)
        self._narrow(lambda x, it, ot: _rshift(x, sh, "u", n), "u" + str(n))

    def _qshrn(self, rounding: bool, out_unsigned_from_signed: bool):
        sh = self.ops[2].imm
        dt = self.dt
        n = bits(dt)
        ins = "s" if dt[0] == "s" else "u"
        outk = "u" if (out_unsigned_from_signed or dt[0] == "u") else "s"
        def fn(x, it, ot):
            v = _rshift(x, sh, ins, n) if rounding else f"({x} >> {sh})"
            return _sat(v, outk + str(n // 2), outk)
        self._narrow(fn, (ins + str(n)))

    def op_vqshrn(self):
        self._qshrn(False, False)

    def op_vqrshrn(self):
        self._qshrn(True, False)

    def op_vqshrun(self):
        self._qshrn(False, True)

    def op_vqrshrun(self):
        self._qshrn(True, True)

    def op_vshll(self):
        d, _, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        sh = self.ops[2].imm
        A_ = self.load(a, 1)
        D = self.load(d, 2)
        it = CT[self.dt] if self.dt[0] != "i" else unsigned(self.dt)
        ot = (signed if self.dt[0] == "s" else unsigned)(wider(self.dt))
        L = 64 // bits(self.dt)
        self.lines.append(f"for (int k = 0; k < {L}; k++) (({ot} *){D})[k] = ({ot})((uint64_t)({ot})(({it} *){A_})[k] << {sh});")
        self.store(D, d, 2)

    def _long(self, expr_fn, acc: bool = False, wide: bool = False, scalar_ok: bool = True):
        """D (Q) = f(A, B) with A,B half width (wide: A is Q already)."""
        dt = self.dt
        d, _, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        b, _, bidx = self.reg(self.ops[2])
        A_ = self.load(a, 2 if wide else 1)
        B_ = self.load(b, 1)
        D = self.load(d, 2)
        it = CT[dt] if dt[0] != "i" else unsigned(dt)
        ot = (signed if dt[0] == "s" else unsigned)(wider(dt))
        L = 64 // bits(dt)
        ae = f"(({ot} *){A_})[k]" if wide else f"({ot})(({it} *){A_})[k]"
        be = f"({ot})(({it} *){B_})[{bidx if bidx != -1 else 'k'}]"
        de = f"(({ot} *){D})[k]"
        self.lines.append(f"for (int k = 0; k < {L}; k++) {de} = ({ot})({expr_fn(ae, be, de, ot)});")
        self.store(D, d, 2)

    def op_vaddl(self):
        self._long(lambda a, b, d, t: f"(uint64_t){a} + (uint64_t){b}")

    def op_vsubl(self):
        self._long(lambda a, b, d, t: f"(uint64_t){a} - (uint64_t){b}")

    def op_vaddw(self):
        self._long(lambda a, b, d, t: f"(uint64_t){a} + (uint64_t){b}", wide=True)

    def op_vsubw(self):
        self._long(lambda a, b, d, t: f"(uint64_t){a} - (uint64_t){b}", wide=True)

    def op_vmull(self):
        if self.dt == "p8":
            raise NeonError("vmull.p8 not supported yet")
        self._long(lambda a, b, d, t: f"(uint64_t){a} * (uint64_t){b}")

    def op_vmlal(self):
        self._long(lambda a, b, d, t: f"(uint64_t){d} + (uint64_t){a} * (uint64_t){b}")

    def op_vmlsl(self):
        self._long(lambda a, b, d, t: f"(uint64_t){d} - (uint64_t){a} * (uint64_t){b}")

    def op_vabdl(self):
        self._long(lambda a, b, d, t: f"({a} > {b} ? (uint64_t){a} - (uint64_t){b} : (uint64_t){b} - (uint64_t){a})")

    def op_vabal(self):
        self._long(lambda a, b, d, t: f"(uint64_t){d} + ({a} > {b} ? (uint64_t){a} - (uint64_t){b} : (uint64_t){b} - (uint64_t){a})")

    def op_vaddhn(self):
        self._hn("+", False)

    def op_vraddhn(self):
        self._hn("+", True)

    def op_vsubhn(self):
        self._hn("-", False)

    def op_vrsubhn(self):
        self._hn("-", True)

    def _hn(self, op, rnd):
        dt = self.dt
        n = bits(dt)
        d, _, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        b, _, _ = self.reg(self.ops[2])
        A_, B_, D = self.load(a, 2), self.load(b, 2), self.load(d, 1)
        it, ot = unsigned(dt), unsigned(narrower(dt))
        r = f" + ({it})1 << {n // 2 - 1}" if rnd else ""
        self.lines.append(f"for (int k = 0; k < {64 // (n // 2)}; k++) (({ot} *){D})[k] = ({ot})(((({it})((({it} *){A_})[k] {op} (({it} *){B_})[k])){r}) >> {n // 2});")
        self.store(D, d, 1)

    # ---- shifts by immediate ----
    def _shift_imm(self, fn):
        dt = self.dt
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        sh = self.ops[2].imm
        A_, D = self.load(a, nd), self.load(d, nd)
        ty = CT[dt] if dt[0] != "i" else unsigned(dt)
        L = self.lanes(nd, dt)
        self.lines.append(f"for (int k = 0; k < {L}; k++) (({ty} *){D})[k] = ({ty})({fn(f'(({ty} *){A_})[k]', f'(({ty} *){D})[k]', sh, ty, bits(dt))});")
        self.store(D, d, nd)

    def op_vshl(self):
        if self.ops[2].type == A.ARM_OP_IMM:
            return self._shift_imm(lambda x, d, s, t, n: f"(uint64_t){x} << {s}")
        self._shift_reg(rounding=False, saturating=False)

    def op_vshr(self):
        self._shift_imm(lambda x, d, s, t, n: _shr(x, s, t, n))

    def op_vrshr(self):
        self._shift_imm(lambda x, d, s, t, n: _rshift(x, s, "s" if t.startswith("int") else "u", n))

    def op_vsra(self):
        self._shift_imm(lambda x, d, s, t, n: f"(uint64_t){d} + (uint64_t)({t})({_shr(x, s, t, n)})")

    def op_vrsra(self):
        self._shift_imm(lambda x, d, s, t, n: f"(uint64_t){d} + (uint64_t)({t})({_rshift(x, s, 's' if t.startswith('int') else 'u', n)})")

    def op_vsri(self):
        def fn(x, d, s, t, n):
            mask = ((1 << n) - 1) >> s if s < n else 0
            return f"(({d} & ~({t}){mask:#x}ull) | (({t})({x} >> {s}) & ({t}){mask:#x}ull))" if s < n else d
        self._shift_imm(fn)

    def op_vsli(self):
        def fn(x, d, s, t, n):
            mask = (((1 << n) - 1) << s) & ((1 << n) - 1)
            return f"(({d} & ~({t}){mask:#x}ull) | (({t})((uint64_t){x} << {s}) & ({t}){mask:#x}ull))"
        self._shift_imm(fn)

    def op_vqshl(self):
        if self.ops[2].type != A.ARM_OP_IMM:
            return self._shift_reg(rounding=False, saturating=True)
        dt = self.dt
        def fn(x, d, s, t, n):
            return _sat(f"((__int128){x} << {s})", dt, dt[0])
        self._shift_imm(fn)

    def op_vqshlu(self):
        dt = self.dt
        def fn(x, d, s, t, n):
            return _sat(f"((__int128){x} << {s})", "u" + str(bits(dt)), "u")
        self._shift_imm(fn)

    def op_vrshl(self):
        self._shift_reg(rounding=True, saturating=False)

    def op_vqrshl(self):
        self._shift_reg(rounding=True, saturating=True)

    def _shift_reg(self, rounding: bool, saturating: bool):
        dt = self.dt
        n = bits(dt)
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        b, _, _ = self.reg(self.ops[2])
        A_, B_, D = self.load(a, nd), self.load(b, nd), self.load(d, nd)
        ty = CT[dt] if dt[0] != "i" else unsigned(dt)
        sgn = 1 if dt[0] == "s" else 0
        L = self.lanes(nd, dt)
        self.lines.append(f"for (int k = 0; k < {L}; k++) (({ty} *){D})[k] = ({ty})arc_nshl((({ty} *){A_})[k], (int8_t)(({unsigned(dt)} *){B_})[k], {n}, {sgn}, {int(rounding)}, {int(saturating)});")
        self.store(D, d, nd)

    # ---- pairwise ----
    def _pairwise(self, fn, dt=None):
        dt = dt or self.dt
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        b, _, _ = self.reg(self.ops[2])
        A_, B_, D = self.load(a, 1), self.load(b, 1), self.load(d, 1)
        ty = CT[dt] if dt[0] != "i" else unsigned(dt)
        L = 64 // bits(dt)
        h = L // 2
        self.lines.append(f"{ty} cat[{L * 2}]; __builtin_memcpy(cat, {A_}, 8); __builtin_memcpy((uint8_t *)cat + 8, {B_}, 8);")
        self.lines.append(f"for (int k = 0; k < {L}; k++) (({ty} *){D})[k] = {fn('cat[2 * k]', 'cat[2 * k + 1]', ty)};")
        self.store(D, d, 1)

    def op_vpadd(self):
        if self.dt == "f32":
            return self._pairwise(lambda a, b, t: f"arc_nfix(arc_nftz({a}) + arc_nftz({b}))")
        self._pairwise(lambda a, b, t: f"({t})({a} + {b})")

    def op_vpmax(self):
        if self.dt == "f32":
            return self._pairwise(lambda a, b, t: f"arc_nmax(arc_nftz({a}), arc_nftz({b}))")
        self._pairwise(lambda a, b, t: f"({a} > {b} ? {a} : {b})")

    def op_vpmin(self):
        if self.dt == "f32":
            return self._pairwise(lambda a, b, t: f"arc_nmin(arc_nftz({a}), arc_nftz({b}))")
        self._pairwise(lambda a, b, t: f"({a} < {b} ? {a} : {b})")

    def _paddl(self, acc: bool):
        dt = self.dt
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        A_, D = self.load(a, nd), self.load(d, nd)
        it = CT[dt]
        ot = (signed if dt[0] == "s" else unsigned)(wider(dt))
        L = nd * 64 // (bits(dt) * 2)
        accx = f"(({ot} *){D})[k] + " if acc else ""
        self.lines.append(f"for (int k = 0; k < {L}; k++) (({ot} *){D})[k] = ({ot})({accx}({ot})(({it} *){A_})[2 * k] + ({ot})(({it} *){A_})[2 * k + 1]);")
        self.store(D, d, nd)

    def op_vpaddl(self):
        self._paddl(False)

    def op_vpadal(self):
        self._paddl(True)

    # ---- permutes ----
    def op_vext(self):
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        b, _, _ = self.reg(self.ops[2])
        imm = self.ops[3].imm * bits(self.dt) // 8
        A_, B_, D = self.load(a, nd), self.load(b, nd), self.load(d, nd)
        nb = 8 * nd
        self.lines.append(f"uint8_t cat[32]; __builtin_memcpy(cat, {A_}, {nb}); __builtin_memcpy(cat + {nb}, {B_}, {nb});")
        self.lines.append(f"__builtin_memcpy({D}, cat + {imm}, {nb});")
        self.store(D, d, nd)

    def _rev(self, group_bits):
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        A_, D = self.load(a, nd), self.load(d, nd)
        ty = unsigned(self.dt)
        per = group_bits // bits(self.dt)
        L = self.lanes(nd, self.dt)
        self.lines.append(f"for (int k = 0; k < {L}; k++) (({ty} *){D})[k] = (({ty} *){A_})[(k / {per}) * {per} + ({per} - 1 - k % {per})];")
        self.store(D, d, nd)

    def op_vrev16(self):
        self._rev(16)

    def op_vrev32(self):
        self._rev(32)

    def op_vrev64(self):
        self._rev(64)

    def _two_dest(self, body):
        a, nd, _ = self.reg(self.ops[0])
        b, _, _ = self.reg(self.ops[1])
        A_, B_ = self.load(a, nd), self.load(b, nd)
        ty = unsigned(self.dt)
        L = self.lanes(nd, self.dt)
        self.lines.append(f"{ty} x[{L}], y[{L}], *pa = ({ty} *){A_}, *pb = ({ty} *){B_};")
        self.lines += body(L)
        self.lines.append(f"__builtin_memcpy({A_}, x, {8 * nd}); __builtin_memcpy({B_}, y, {8 * nd});")
        self.store(A_, a, nd)
        self.store(B_, b, nd)

    def op_vtrn(self):
        self._two_dest(lambda L: [f"for (int k = 0; k < {L}; k += 2) {{ x[k] = pa[k]; x[k + 1] = pb[k]; y[k] = pa[k + 1]; y[k + 1] = pb[k + 1]; }}"])

    def op_vzip(self):
        self._two_dest(lambda L: [f"for (int k = 0; k < {L} / 2; k++) {{ x[2 * k] = pa[k]; x[2 * k + 1] = pb[k]; }}",
                                  f"for (int k = 0; k < {L} / 2; k++) {{ y[2 * k] = pa[{L} / 2 + k]; y[2 * k + 1] = pb[{L} / 2 + k]; }}"])

    def op_vuzp(self):
        self._two_dest(lambda L: [f"for (int k = 0; k < {L} / 2; k++) {{ x[k] = pa[2 * k]; x[{L} / 2 + k] = pb[2 * k]; y[k] = pa[2 * k + 1]; y[{L} / 2 + k] = pb[2 * k + 1]; }}"])

    def _tbl(self, extend: bool):
        d, _, _ = self.reg(self.ops[0])
        tbl = [self.reg(o)[0] for o in self.ops[1:-1]]
        m, _, _ = self.reg(self.ops[-1])
        T = self.load(tbl[0], 1, "tb0")
        for j, t in enumerate(tbl[1:], 1):
            self.load(t, 1, f"tb{j}")
        M, D = self.load(m, 1), self.load(d, 1)
        n = 8 * len(tbl)
        self.lines.append(f"uint8_t tab[{n}];" + "".join(f" __builtin_memcpy(tab + {8 * j}, tb{j}, 8);" for j in range(len(tbl))))
        dflt = f"((uint8_t *){D})[k]" if extend else "0"
        self.lines.append(f"uint8_t res[8]; for (int k = 0; k < 8; k++) {{ uint8_t ix = ((uint8_t *){M})[k]; res[k] = ix < {n} ? tab[ix] : {dflt}; }}")
        self.lines.append(f"__builtin_memcpy({D}, res, 8);")
        self.store(D, d, 1)

    def op_vtbl(self):
        self._tbl(False)

    def op_vtbx(self):
        self._tbl(True)

    # ---- conversions ----
    def op_vcvt(self):
        to, frm = self.dts[0], self.dts[1]
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        fbits = self.ops[2].imm if len(self.ops) > 2 else 0
        A_, D = self.load(a, nd), self.load(d, nd)
        L = nd * 2
        scale = float(2 ** fbits)
        if to in ("s32", "u32"):
            fn = "arc_f2s32" if to == "s32" else "arc_f2u32"
            self.lines.append(f"for (int k = 0; k < {L}; k++) ((uint32_t *){D})[k] = {fn}((double)arc_nftz(((float *){A_})[k]) * {scale!r}, 3u);")
        elif frm in ("s32", "u32"):
            it = "int32_t" if frm == "s32" else "uint32_t"
            self.lines.append(f"for (int k = 0; k < {L}; k++) ((float *){D})[k] = arc_nfix((float)((double)(({it} *){A_})[k] / {scale!r}));")
        else:
            raise NeonError(f"vcvt {to}<-{frm} (half precision) not supported yet")
        self.store(D, d, nd)

    def _float_unary(self, fn):
        d, nd, _ = self.reg(self.ops[0])
        a, _, _ = self.reg(self.ops[1])
        A_, D = self.load(a, nd), self.load(d, nd)
        if self.dt == "u32":
            self.lines.append(f"for (int k = 0; k < {nd * 2}; k++) ((uint32_t *){D})[k] = {fn}_u32(((uint32_t *){A_})[k]);")
        else:
            self.lines.append(f"for (int k = 0; k < {nd * 2}; k++) ((float *){D})[k] = {fn}(((float *){A_})[k]);")
        self.store(D, d, nd)

    def op_vrecpe(self):
        self._float_unary("arc_recpe")

    def op_vrsqrte(self):
        self._float_unary("arc_rsqrte")

    # ---- loads / stores ----
    def _ldst(self, load: bool):
        nstruct = int(self.base[-1])
        size = bits(self.dt)
        ops = self.ops
        regs_ops = [o for o in ops if o.type == A.ARM_OP_REG and self.cs.reg_name(o.reg)[0] == "d"]
        mem = next(o for o in ops if o.type == A.ARM_OP_MEM)
        post = ops[-1] if ops[-1].type == A.ARM_OP_REG and self.cs.reg_name(ops[-1].reg)[0] == "r" else None
        dregs = [int(self.cs.reg_name(o.reg)[1:]) for o in regs_ops]
        lst = self.cs.op_str[self.cs.op_str.index("{") + 1:self.cs.op_str.index("}")]
        lane_m = re.findall(r"\[(\d*)\]", lst)
        em = self.em
        base_n = em.regnum(self.i, _RegOp(mem.mem.base))
        base = em.r(self.i, base_n)
        esz = size // 8
        self.lines.append(f"uint32_t ea = {base};")
        total = 0
        if not lane_m:  # multiple structures: element k of struct s comes from D[s + j*?]
            regs = dregs
            nel = 8 // esz
            if nstruct == 1:
                for j, dr in enumerate(regs):
                    if load:
                        self.lines.append(f"ctx->d[{dr}] = (uint64_t)ARC_LW(ea + {8 * j}u) | ((uint64_t)ARC_LW(ea + {8 * j + 4}u) << 32);")
                    else:
                        self.lines.append(f"ARC_SW(ea + {8 * j}u, (uint32_t)ctx->d[{dr}]); ARC_SW(ea + {8 * j + 4}u, (uint32_t)(ctx->d[{dr}] >> 32));")
                total = 8 * len(regs)
            else:
                # regs = nstruct registers (single spacing) or 2*nstruct (vld2 with 4 regs = two groups)
                groups = len(regs) // nstruct
                ty = unsigned(str(size))
                arrs = []
                for j, dr in enumerate(regs):
                    arrs.append(self.load(dr, 1, f"r{j}_"))
                off = 0
                for g in range(groups):
                    for e in range(nel):
                        for s in range(nstruct):
                            idx = g + s * groups  # ARM ARM: Elem[D[d+r+s*inc], e], inc = groups
                            arr = arrs[idx]
                            addr = f"ea + {off}u"
                            if load:
                                self.lines.append(f"(({ty} *){arr})[{e}] = ({ty})ARC_L{esz * 8}({addr});")
                            else:
                                self.lines.append(f"ARC_S{esz * 8}({addr}, (({ty} *){arr})[{e}]);")
                            off += esz
                if load:
                    for j, dr in enumerate(regs):
                        self.store(arrs[j], dr, 1)
                total = off
        else:
            ty = unsigned(str(size))
            allr = lane_m[0] == ""
            if allr:  # load one structure and replicate to all lanes
                n_per = nstruct
                regs = dregs
                for s in range(n_per):
                    targets = regs[s::n_per] if len(regs) > n_per else [regs[s]]
                    self.lines.append(f"{{ {ty} x = ({ty})ARC_L{size}(ea + {s * esz}u);")
                    for dr in targets:
                        A_ = self.load(dr, 1)
                        self.lines.append(f"  for (int k = 0; k < {8 // esz}; k++) (({ty} *){A_})[k] = x;")
                        self.store(A_, dr, 1)
                    self.lines.append("}")
                total = nstruct * esz
            else:
                lane = int(lane_m[0])
                for s, dr in enumerate(dregs):
                    A_ = self.load(dr, 1)
                    if load:
                        self.lines.append(f"(({ty} *){A_})[{lane}] = ({ty})ARC_L{size}(ea + {s * esz}u);")
                        self.store(A_, dr, 1)
                    else:
                        self.lines.append(f"ARC_S{size}(ea + {s * esz}u, (({ty} *){A_})[{lane}]);")
                total = nstruct * esz
        if post is not None:
            self.lines.append(f"{em.w(base_n)} = ea + {em.r(self.i, em.regnum(self.i, post))};")
        elif self.cs.writeback:
            self.lines.append(f"{em.w(base_n)} = ea + {total}u;")

    def op_vld1(self):
        self._ldst(True)
    op_vld2 = op_vld3 = op_vld4 = op_vld1

    def op_vst1(self):
        self._ldst(False)
    op_vst2 = op_vst3 = op_vst4 = op_vst1


class _RegOp:
    def __init__(self, reg):
        self.reg = reg


def _expand_i64(raw: bytes) -> str:
    """VMOV.I64 (op=1, cmode=1110): cada bit de imm8 vira um byte (AdvSIMDExpandImm).

    O Capstone devolve esse imediato como 32 bits com sinal estendido (0x00000000ffffffff sai -1), então
    imm8 = i:imm3:imm4 é lido da codificação. Thumb: i é o bit 12 da 1ª meia-palavra; ARM: o bit 24.
    """
    if len(raw) != 4:
        raise NeonError("vmov.i64: codificação inesperada")
    hw1, hw2 = int.from_bytes(raw[0:2], "little"), int.from_bytes(raw[2:4], "little")
    if hw1 >> 13 == 0b111:  # Thumb: 111i 1111 1D00 0imm3 | Vd cmode 0Q op1 imm4
        i, imm3, imm4 = (hw1 >> 12) & 1, hw1 & 7, hw2 & 0xF
    else:  # ARM: 1111 001i 1D00 0imm3 Vd cmode 0Q op1 imm4
        w = int.from_bytes(raw, "little")
        i, imm3, imm4 = (w >> 24) & 1, (w >> 16) & 7, w & 0xF
    imm8 = (i << 7) | (imm3 << 4) | imm4
    v = 0
    for b in range(8):
        if imm8 >> b & 1:
            v |= 0xFF << (8 * b)
    return f"{v:#x}"


def _shr(x, s, t, n):
    if s >= n:  # shift by element size: arithmetic -> sign fill, logical -> 0
        return f"({x} < 0 ? -1 : 0)" if t.startswith("int") else "0"
    return f"({x} >> {s})"


def _rshift(x, s, kind, n):
    # rounding shift right by s (1..n): (x + 2^(s-1)) >> s computed without overflow
    ty = "__int128" if kind == "s" else "unsigned __int128"
    return f"(({ty}){x} + (({ty})1 << {s - 1})) >> {s}"


def _sat(x, dt, kind):
    n = bits(dt)
    if kind == "s":
        lo, hi = -(1 << (n - 1)), (1 << (n - 1)) - 1
        return f"arc_sat_s((__int128)({x}), {lo}ll, {hi}ll)"
    hi = (1 << n) - 1
    return f"arc_sat_u((__int128)({x}), {hi:#x}ull)"


def _sat_int(op):
    def f(a, b, d, dt, n):
        kind = "s" if dt[0] == "s" else "u"
        return _sat(f"(__int128){a} {op} (__int128){b}", dt, kind)
    return f


BIN_INT = {
    "vadd": lambda a, b, d, dt, n: f"(uint64_t){a} + (uint64_t){b}",
    "vsub": lambda a, b, d, dt, n: f"(uint64_t){a} - (uint64_t){b}",
    "vmul": lambda a, b, d, dt, n: f"(uint64_t){a} * (uint64_t){b}",
    "vmla": lambda a, b, d, dt, n: f"(uint64_t){d} + (uint64_t){a} * (uint64_t){b}",
    "vmls": lambda a, b, d, dt, n: f"(uint64_t){d} - (uint64_t){a} * (uint64_t){b}",
    "vmax": lambda a, b, d, dt, n: f"({a} > {b} ? {a} : {b})",
    "vmin": lambda a, b, d, dt, n: f"({a} < {b} ? {a} : {b})",
    "vabd": lambda a, b, d, dt, n: f"({a} > {b} ? (uint64_t){a} - (uint64_t){b} : (uint64_t){b} - (uint64_t){a})",
    "vaba": lambda a, b, d, dt, n: f"(uint64_t){d} + ({a} > {b} ? (uint64_t){a} - (uint64_t){b} : (uint64_t){b} - (uint64_t){a})",
    "vhadd": lambda a, b, d, dt, n: f"((__int128){a} + (__int128){b}) >> 1",
    "vrhadd": lambda a, b, d, dt, n: f"((__int128){a} + (__int128){b} + 1) >> 1",
    "vhsub": lambda a, b, d, dt, n: f"((__int128){a} - (__int128){b}) >> 1",
    "vqadd": _sat_int("+"),
    "vqsub": _sat_int("-"),
    "vtst": lambda a, b, d, dt, n: f"(({a} & {b}) != 0 ? ~(uint64_t)0 : 0)",
    "vqdmulh": lambda a, b, d, dt, n: _sat(f"((__int128)2 * {a} * {b}) >> {n}", dt, "s"),
    "vqrdmulh": lambda a, b, d, dt, n: _sat(f"((__int128)2 * {a} * {b} + ((__int128)1 << {n - 1})) >> {n}", dt, "s"),
}
BIN_FLOAT = {
    "vadd": "arc_nfix({a} + {b})", "vsub": "arc_nfix({a} - {b})", "vmul": "arc_nfix({a} * {b})",
    "vmla": "arc_nfix({d} + arc_nfix({a} * {b}))", "vmls": "arc_nfix({d} - arc_nfix({a} * {b}))",
    "vmax": "arc_nmax({a}, {b})", "vmin": "arc_nmin({a}, {b})",
    "vabd": "arc_nfix(__builtin_fabsf(arc_nfix({a} - {b})))",
    "vrecps": "arc_recps({a}, {b})", "vrsqrts": "arc_rsqrts({a}, {b})",
}
CMP = {"vceq": "{a} == {b}", "vcge": "{a} >= {b}", "vcgt": "{a} > {b}", "vcle": "{a} <= {b}",
       "vclt": "{a} < {b}", "vacge": "__builtin_fabsf({a}) >= __builtin_fabsf({b})",
       "vacgt": "__builtin_fabsf({a}) > __builtin_fabsf({b})"}
BITWISE = {"vand": "{a} & {b}", "vorr": "{a} | {b}", "veor": "{a} ^ {b}", "vbic": "{a} & ~{b}",
           "vorn": "{a} | ~{b}", "vbsl": "({d} & {a}) | (~{d} & {b})",
           "vbit": "({b} & {a}) | (~{b} & {d})", "vbif": "(~{b} & {a}) | ({b} & {d})"}
UNARY = {
    # Float VABS/VNEG are pure sign-bit operations (FPAbs/FPNeg): no NaN processing, no flush.
    "vabs": lambda a, dt, n: f"arc_bitsf(arc_fbits({a}) & 0x7fffffffu)" if dt == "f32" else f"({a} < 0 ? -(uint64_t){a} : {a})",
    "vneg": lambda a, dt, n: f"arc_bitsf(arc_fbits({a}) ^ 0x80000000u)" if dt == "f32" else f"(-(uint64_t){a})",
    "vqabs": lambda a, dt, n: _sat(f"({a} < 0 ? -(__int128){a} : (__int128){a})", dt, "s"),
    "vqneg": lambda a, dt, n: _sat(f"-(__int128){a}", dt, "s"),
    "vcnt": lambda a, dt, n: f"(uint8_t)__builtin_popcount({a})",
    "vclz": lambda a, dt, n: f"({a} ? __builtin_clz((uint32_t){a}) - {32 - n} : {n})",
    "vcls": lambda a, dt, n: f"arc_cls((int32_t){a}, {n})",
}
