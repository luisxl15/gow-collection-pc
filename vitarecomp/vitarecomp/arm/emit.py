"""ARMv7 (ARM + Thumb-2 + VFPv3 scalar) -> C emitter.

Guest model (runtime/arm/arc.h):
  * r0-r12, sp, lr live in C locals; flags N Z C V are 0/1 locals (fn fz fc fv).
    They are written back to ctx before calls, syscalls, dispatch and returns,
    and reloaded after calls.
  * VFP registers live in ctx->d[32] (uint64); S registers alias halves.
  * Guest memory is a flat 4 GiB window: ARC_P(addr) = arc_mem + (uint32_t)addr.
  * Calls (BL/BLX) are C calls; `bx lr` / `pop {..,pc}` return; indirect
    branches go through arc_dispatch (table of recompiled functions).
"""
from __future__ import annotations

import re

from capstone import arm as A

from .analysis import Function, Insn, Program, reg_num

CC = {A.ARM_CC_EQ: "fz", A.ARM_CC_NE: "!fz", A.ARM_CC_HS: "fc", A.ARM_CC_LO: "!fc",
      A.ARM_CC_MI: "fn", A.ARM_CC_PL: "!fn", A.ARM_CC_VS: "fv", A.ARM_CC_VC: "!fv",
      A.ARM_CC_HI: "(fc && !fz)", A.ARM_CC_LS: "(!fc || fz)", A.ARM_CC_GE: "(fn == fv)",
      A.ARM_CC_LT: "(fn != fv)", A.ARM_CC_GT: "(!fz && fn == fv)", A.ARM_CC_LE: "(fz || fn != fv)"}

SFT_ASR, SFT_LSL, SFT_LSR, SFT_ROR, SFT_RRX = A.ARM_SFT_ASR, A.ARM_SFT_LSL, A.ARM_SFT_LSR, A.ARM_SFT_ROR, A.ARM_SFT_RRX
SFT_REG = {A.ARM_SFT_ASR_REG: "asr", A.ARM_SFT_LSL_REG: "lsl", A.ARM_SFT_LSR_REG: "lsr",
           A.ARM_SFT_ROR_REG: "ror"}
LOCAL = {13: "sp", 14: "lr"}


class EmitError(Exception):
    pass


def u32(v: int) -> str:
    return f"{v & 0xFFFFFFFF:#x}u"


def fname(f: Function) -> str:
    return f"arc_{f.entry:08x}{'t' if f.thumb else 'a'}_{f.name}"[:96]


class FnEmitter:
    def __init__(self, prog: Program, f: Function):
        self.prog, self.f = prog, f
        self.used: set[str] = set()
        self.written: set[str] = set()
        self.tmp = 0

    # ---- registers ----------------------------------------------------------
    def r(self, i: Insn, n: int) -> str:
        if n == 15:
            return u32(self.pc(i))
        v = LOCAL.get(n, f"r{n}")
        self.used.add(v)
        return v

    def w(self, n: int) -> str:
        if n == 15:
            raise EmitError("internal: PC write must be handled as control flow")
        v = LOCAL.get(n, f"r{n}")
        self.used.add(v)
        self.written.add(v)
        return v

    def flag_w(self, *names: str):
        for n in names:
            self.used.add(n)
            self.written.add(n)

    def flag_r(self, *names: str):
        for n in names:
            self.used.add(n)

    def pc(self, i: Insn) -> int:
        return i.addr + (4 if i.thumb else 8)

    def regnum(self, i: Insn, op) -> int:
        n = reg_num(i.cs, op.reg)
        if n is None:
            raise EmitError(f"{i.addr:#x}: unexpected register {i.cs.reg_name(op.reg)}")
        return n

    def new_tmp(self) -> str:
        self.tmp += 1
        return f"t{self.tmp}"

    # ---- shifter operand ----------------------------------------------------
    def shifted(self, i: Insn, op, want_carry: bool) -> tuple[str, str | None]:
        """Value of a register operand with its shift; carry-out expr (None = unchanged)."""
        x = self.r(i, self.regnum(i, op))
        st, sv = op.shift.type, op.shift.value
        if st == 0 or (st == SFT_LSL and sv == 0):
            return x, None
        if st in SFT_REG:
            rs = self.r(i, reg_num(i.cs, sv))
            kind = SFT_REG[st]
            if want_carry:
                self.flag_r("fc")
                return f"ARC_SHIFT_C_{kind.upper()}({x}, {rs}, fc)", f"ARC_SHIFT_CO_{kind.upper()}({x}, {rs}, fc)"
            return f"ARC_SHIFT_{kind.upper()}({x}, {rs})", None
        if st == SFT_LSL:
            return f"({x} << {sv})", f"(({x} >> {32 - sv}) & 1u)"
        if st == SFT_LSR:
            if sv == 32:
                return "0u", f"({x} >> 31)"
            return f"({x} >> {sv})", f"(({x} >> {sv - 1}) & 1u)"
        if st == SFT_ASR:
            if sv == 32:
                return f"(uint32_t)((int32_t){x} >> 31)", f"({x} >> 31)"
            return f"(uint32_t)((int32_t){x} >> {sv})", f"(({x} >> {sv - 1}) & 1u)"
        if st == SFT_ROR:
            return f"ARC_ROR({x}, {sv})", f"(ARC_ROR({x}, {sv}) >> 31)"
        if st == SFT_RRX:
            self.flag_r("fc")
            return f"((fc << 31) | ({x} >> 1))", f"({x} & 1u)"
        raise EmitError(f"{i.addr:#x}: shift type {st}")

    def operand2(self, i: Insn, op, want_carry: bool) -> tuple[str, str | None]:
        if op.type == A.ARM_OP_IMM:
            v = op.imm & 0xFFFFFFFF
            return u32(v), (self.imm_carry(i, v) if want_carry else None)
        if op.type == A.ARM_OP_REG:
            return self.shifted(i, op, want_carry)
        raise EmitError(f"{i.addr:#x}: unsupported operand type {op.type}")

    def imm_carry(self, i: Insn, v: int) -> str | None:
        """Carry-out of a modified immediate (needs the raw encoding)."""
        raw = i.raw
        if i.thumb:
            if i.size != 4:
                return None
            hw1 = raw[0] | raw[1] << 8
            hw2 = raw[2] | raw[3] << 8
            if (hw1 & 0xFA00) == 0xF000 and not hw2 & 0x8000:
                imm12 = ((hw1 >> 10) & 1) << 11 | ((hw2 >> 12) & 7) << 8 | (hw2 & 0xFF)
                return None if (imm12 >> 10) == 0 else str(v >> 31) + "u"
            return None
        word = int.from_bytes(raw, "little")
        if (word >> 25) & 7 == 1:
            return None if (word >> 8) & 0xF == 0 else str(v >> 31) + "u"
        return None

    # ---- flag helpers -----------------------------------------------------
    def nz(self, res: str) -> list[str]:
        self.flag_w("fn", "fz")
        return [f"fn = {res} >> 31; fz = {res} == 0;"]

    # ---- memory -----------------------------------------------------------
    def mem_addr(self, i: Insn, mem_op, extra=None) -> tuple[str, str | None, str | None]:
        """Returns (access address expr, writeback base reg local, new base expr)."""
        m = mem_op.mem
        base_n = reg_num(i.cs, m.base)
        if base_n == 15:
            base = u32((self.pc(i) & ~3) if i.thumb else self.pc(i))
        else:
            base = self.r(i, base_n)
        if m.index:
            idx = self.r(i, reg_num(i.cs, m.index))
            st, sv = mem_op.shift.type, mem_op.shift.value
            if st == SFT_LSL and sv:
                idx = f"({idx} << {sv})"
            elif st == SFT_LSR:
                idx = f"({idx} >> {sv})" if sv != 32 else "0u"
            elif st == SFT_ASR:
                idx = f"(uint32_t)((int32_t){idx} >> {min(sv, 31)})"
            elif st == SFT_ROR:
                idx = f"ARC_ROR({idx}, {sv})"
            elif st == SFT_RRX:
                self.flag_r("fc")
                idx = f"((fc << 31) | ({idx} >> 1))"
            off = f"- {idx}" if mem_op.subtracted else f"+ {idx}"
        else:
            d = m.disp
            off = f"+ {u32(d)}" if d >= 0 else f"- {u32(-d)}"
        if extra is not None:   # post-indexed: access at base, then base += extra
            if extra.type == A.ARM_OP_IMM:
                e = extra.imm
                if not i.thumb:  # ARM: the sign lives in the U bit (capstone drops it for LDRH/LDRSB/STRH...)
                    e = abs(e) if (int.from_bytes(i.raw, "little") >> 23) & 1 else -abs(e)
                post = f"{base} + {u32(e)}" if e >= 0 else f"{base} - {u32(-e)}"
            else:
                val, _ = self.shifted(i, extra, False)
                sub = self.post_reg_subtract(i)
                post = f"{base} {'-' if sub else '+'} {val}"
            return base, self.w(base_n), post
        addr = f"({base} {off})"
        if i.cs.writeback:
            return addr, self.w(base_n), addr
        return addr, None, None

    def post_reg_subtract(self, i: Insn) -> bool:
        if i.thumb:
            return False
        return not (int.from_bytes(i.raw, "little") >> 23) & 1

    # ---- control helpers ----------------------------------------------------
    def call_direct(self, target_key: int, i: Insn) -> list[str]:
        f = self.prog.functions.get(target_key)
        stub = self.prog.img.imports.get(target_key & ~1)
        if stub is not None:
            name, lib, nid = stub
            return [f"SYNC_OUT arc_import(ctx, {nid:#010x}u, {i.addr:#010x}u); SYNC_IN /* {name} */"]
        if f is None:
            return [f"SYNC_OUT arc_dispatch(ctx, {u32(target_key)}, {i.addr:#010x}u); SYNC_IN"]
        return [f"SYNC_OUT {fname(f)}(ctx); SYNC_IN"]

    def goto_or_tail(self, target: int, i: Insn) -> str:
        key = target | self.thumb_bit
        if target == self.f.entry or (target in self.f.insns and key not in self.prog.functions):
            return f"goto L_{target:08x};"
        if key in self.prog.functions:
            return f"{{ SYNC_OUT {fname(self.prog.functions[key])}(ctx); return; }}"
        if target in self.f.insns:
            return f"goto L_{target:08x};"
        return f"{{ SYNC_OUT arc_dispatch(ctx, {u32(key)}, {i.addr:#010x}u); return; }}"

    @property
    def thumb_bit(self) -> int:
        return 1 if self.f.thumb else 0

    def link(self, i: Insn) -> list[str]:
        return [f"{self.w(14)} = {u32(i.next | self.thumb_bit)};"]

    def indirect(self, target: str, i: Insn, is_return_like: bool, interworking: bool = True) -> list[str]:
        """Branch to a computed address. Thumb ALU writes to PC (mov/add pc) do not
        interwork (BranchWritePC): the target stays Thumb whatever bit 0 says."""
        if is_return_like:
            return ["{ SYNC_OUT return; }"]
        if not interworking and self.f.thumb:
            target = f"(({target}) | 1u)"
        lines = [f"{{ uint32_t jt = {target};"]
        cases = sorted(l for l in self.f.labels)
        if cases:
            lines.append("  switch (jt) {")
            for c in cases:
                lines.append(f"  case {u32(c | self.thumb_bit)}: goto L_{c:08x};")
            lines.append("  default: break; }")
        lines.append(f"  SYNC_OUT arc_dispatch(ctx, jt, {i.addr:#010x}u); return; }}")
        return lines

    # ---- main dispatch --------------------------------------------------------
    def insn(self, i: Insn) -> list[str]:
        body = self.translate(i)
        cc = i.cs.cc
        if cc in (A.ARM_CC_AL, A.ARM_CC_INVALID) or i.name in ("b", "cbz", "cbnz", "it"):
            return body
        cond = CC[cc]
        for fl in ("fn", "fz", "fc", "fv"):
            if fl in cond:
                self.flag_r(fl)
        return [f"if ({cond}) {{"] + ["    " + b for b in body] + ["}"]

    def translate(self, i: Insn) -> list[str]:  # noqa: C901
        n, ops, cs = i.name, i.cs.operands, i.cs
        uf = i.uf
        R = lambda k: self.regnum(i, ops[k])  # noqa: E731

        if n in ("it", "hint", "nop", "dmb", "dsb", "isb", "pld", "pli", "pldw", "clrex", "yield"):
            return [f"/* {cs.mnemonic} */"]
        if n == "svc":
            return [f"SYNC_OUT arc_svc(ctx, {ops[0].imm & 0xFFFFFF:#x}u, {i.addr:#010x}u); SYNC_IN"]
        if n in ("udf", "bkpt"):
            return [f"arc_trap(ctx, {i.addr:#010x}u);"]

        # ---- branches ----
        if n == "b":
            tgt = ops[0].imm & 0xFFFFFFFF
            g = self.goto_or_tail(tgt, i)
            if cs.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
                cond = CC[cs.cc]
                for fl in ("fn", "fz", "fc", "fv"):
                    if fl in cond:
                        self.flag_r(fl)
                return [f"if ({cond}) {g}"]
            return [g]
        if n in ("cbz", "cbnz"):
            x = self.r(i, R(0))
            tgt = ops[1].imm & 0xFFFFFFFF
            return [f"if ({x} {'==' if n == 'cbz' else '!='} 0) {self.goto_or_tail(tgt, i)}"]
        if n in ("bl", "blx") and ops[0].type == A.ARM_OP_IMM:
            tgt = ops[0].imm & 0xFFFFFFFF
            tthumb = self.f.thumb if n == "bl" else not self.f.thumb
            return self.link(i) + self.call_direct(tgt | (1 if tthumb else 0), i)
        if n == "blx":
            t = self.new_tmp()
            return [f"{{ uint32_t {t} = {self.r(i, R(0))};"] + self.link(i) + \
                [f"SYNC_OUT arc_dispatch(ctx, {t}, {i.addr:#010x}u); SYNC_IN }}"]
        if n == "bx":
            rn = R(0)
            if rn == 14:
                return ["{ SYNC_OUT return; }"]
            return self.indirect(self.r(i, rn), i, False)
        if n in ("tbb", "tbh"):
            m = ops[0].mem
            idx = self.r(i, reg_num(cs, m.index))
            targets = self.f.tables.get(i.addr)
            if targets is None:
                raise EmitError(f"{i.addr:#x}: {n} without a resolved table")
            lines = [f"switch ({idx}) {{"]
            for k, t in enumerate(targets):
                lines.append(f"case {k}: goto L_{t:08x};")
            lines.append(f"default: arc_trap(ctx, {i.addr:#010x}u); }}")
            return lines

        # ---- loads / stores ----
        if n in ("ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrex", "ldrexb", "ldrexh", "ldrt", "ldrbt"):
            rt = R(0)
            extra = ops[2] if len(ops) > 2 else None
            addr, wb, newbase = self.mem_addr(i, ops[1], extra)
            load = {"ldr": "ARC_LW", "ldrex": "ARC_LW", "ldrt": "ARC_LW", "ldrb": "ARC_LBU",
                    "ldrexb": "ARC_LBU", "ldrbt": "ARC_LBU", "ldrh": "ARC_LHU", "ldrexh": "ARC_LHU",
                    "ldrsb": "ARC_LB", "ldrsh": "ARC_LH"}[n]
            t = self.new_tmp()
            lines = [f"{{ uint32_t {t} = {load}({addr});"]
            if wb:
                lines.append(f"  {wb} = {newbase};")
            if rt == 15:
                is_ret = reg_num(cs, ops[1].mem.base) == 13 and cs.writeback
                lines += ["  " + l for l in self.indirect(t, i, is_ret)]
                lines.append("}")
                return lines
            lines.append(f"  {self.w(rt)} = {t}; }}")
            return lines
        if n in ("str", "strb", "strh", "strt", "strbt"):
            extra = ops[2] if len(ops) > 2 else None
            val = self.r(i, R(0))
            addr, wb, newbase = self.mem_addr(i, ops[1], extra)
            store = {"str": "ARC_SW", "strt": "ARC_SW", "strb": "ARC_SB", "strbt": "ARC_SB", "strh": "ARC_SH"}[n]
            lines = [f"{store}({addr}, {val});"]
            if wb:
                lines.append(f"{wb} = {newbase};")
            return lines
        if n in ("strex", "strexb", "strexh"):
            store = {"strex": "ARC_SW", "strexb": "ARC_SB", "strexh": "ARC_SH"}[n]
            addr, _, _ = self.mem_addr(i, ops[2])
            return [f"{store}({addr}, {self.r(i, R(1))});", f"{self.w(R(0))} = 0u;"]
        if n in ("ldrd", "strd", "ldrexd", "strexd"):
            if n == "strexd":
                addr, _, _ = self.mem_addr(i, ops[3])
                return [f"{{ uint32_t a = {addr}; ARC_SW(a, {self.r(i, R(1))}); ARC_SW(a + 4u, {self.r(i, R(2))}); }}",
                        f"{self.w(R(0))} = 0u;"]
            extra = ops[3] if len(ops) > 3 else None
            addr, wb, newbase = self.mem_addr(i, ops[2], extra)
            if n in ("ldrd", "ldrexd"):
                lines = [f"{{ uint32_t a = {addr}; uint32_t v0 = ARC_LW(a), v1 = ARC_LW(a + 4u);"]
                if wb:
                    lines.append(f"  {wb} = {newbase};")
                lines.append(f"  {self.w(R(0))} = v0; {self.w(R(1))} = v1; }}")
                return lines
            lines = [f"{{ uint32_t a = {addr}; ARC_SW(a, {self.r(i, R(0))}); ARC_SW(a + 4u, {self.r(i, R(1))});"]
            if wb:
                lines.append(f"  {wb} = {newbase};")
            lines.append("}")
            return lines
        if n in ("push", "pop", "ldm", "ldmdb", "ldmib", "ldmda", "stm", "stmdb", "stmib", "stmda"):
            return self.multiple(i)

        # ---- data processing ----
        if n in ("mov", "mvn", "movw", "movt", "adr", "lsl", "lsr", "asr", "ror", "rrx"):
            return self.moves(i)
        if n in ("add", "adc", "sub", "sbc", "rsb", "rsc", "cmp", "cmn", "addw", "subw"):
            return self.arith(i)
        if n in ("and", "orr", "orn", "eor", "bic", "tst", "teq"):
            return self.logical(i)
        if n in ("mul", "mla", "mls"):
            a, b = self.r(i, R(1)), self.r(i, R(2))
            if n == "mul":
                res = f"{a} * {b}"
            else:
                c = self.r(i, R(3))
                res = f"{c} {'+' if n == 'mla' else '-'} {a} * {b}"
            t = self.new_tmp()
            lines = [f"{{ uint32_t {t} = {res}; {self.w(R(0))} = {t};"]
            if uf:
                lines += ["  " + l for l in self.nz(t)]
            return lines + ["}"]
        if n in ("umull", "smull", "umlal", "smlal"):
            lo, hi = R(0), R(1)
            a, b = self.r(i, R(2)), self.r(i, R(3))
            prod = f"(uint64_t){a} * {b}" if n[0] == "u" else f"(uint64_t)((int64_t)(int32_t){a} * (int32_t){b})"
            acc = f" + (((uint64_t){self.r(i, hi)} << 32) | {self.r(i, lo)})" if n.endswith("lal") else ""
            lines = [f"{{ uint64_t p = {prod}{acc}; {self.w(lo)} = (uint32_t)p; {self.w(hi)} = (uint32_t)(p >> 32);"]
            if uf:
                self.flag_w("fn", "fz")
                lines.append("  fn = (uint32_t)(p >> 63); fz = p == 0;")
            return lines + ["}"]
        if n in ("sdiv", "udiv"):
            a, b = self.r(i, R(1)), self.r(i, R(2))
            fn_ = "ARC_SDIV" if n == "sdiv" else "ARC_UDIV"
            return [f"{self.w(R(0))} = {fn_}({a}, {b});"]
        if n in ("clz", "rbit", "rev", "rev16", "revsh"):
            x = self.r(i, R(1))
            expr = {"clz": f"ARC_CLZ({x})", "rbit": f"arc_rbit({x})", "rev": f"__builtin_bswap32({x})",
                    "rev16": f"(((({x}) >> 8) & 0x00ff00ffu) | ((({x}) << 8) & 0xff00ff00u))",
                    "revsh": f"(uint32_t)(int32_t)(int16_t)(((({x}) >> 8) & 0xffu) | ((({x}) << 8) & 0xff00u))"}[n]
            return [f"{self.w(R(0))} = {expr};"]
        if n in ("uxtb", "uxth", "sxtb", "sxth", "uxtab", "uxtah", "sxtab", "sxtah"):
            src = ops[-1]
            x = self.r(i, self.regnum(i, src))
            rot = src.shift.value if src.shift.type == SFT_ROR else 0
            if rot:
                x = f"ARC_ROR({x}, {rot})"
            size = n[-1]  # uxtb / uxtab -> 'b'
            ext = {"b": "(uint8_t)", "h": "(uint16_t)"}[size]
            val = f"(uint32_t){ext}{x}" if n[0] == "u" else f"(uint32_t)(int32_t)({'int8_t' if size == 'b' else 'int16_t'}){x}"
            if len(n) == 5:  # *xta*: add
                val = f"{self.r(i, R(1))} + {val}"
            return [f"{self.w(R(0))} = {val};"]
        if n in ("ubfx", "sbfx"):
            x = self.r(i, R(1))
            lsb, width = ops[2].imm, ops[3].imm
            if n == "ubfx":
                mask = (1 << width) - 1 if width < 32 else 0xFFFFFFFF
                return [f"{self.w(R(0))} = ({x} >> {lsb}) & {u32(mask)};"]
            return [f"{self.w(R(0))} = (uint32_t)((int32_t)({x} << {32 - lsb - width}) >> {32 - width});"]
        if n in ("bfi", "bfc"):
            lsb, width = ops[-2].imm, ops[-1].imm
            mask = (((1 << width) - 1) << lsb) & 0xFFFFFFFF
            d = self.r(i, R(0))
            src = f"(({self.r(i, R(1))} << {lsb}) & {u32(mask)})" if n == "bfi" else "0u"
            return [f"{self.w(R(0))} = ({d} & {u32(~mask)}) | {src};"]
        if n in ("usat", "ssat"):
            sat = ops[1].imm
            x, _ = self.shifted(i, ops[2], False)
            fn_ = "arc_usat" if n == "usat" else "arc_ssat"
            return [f"{self.w(R(0))} = {fn_}({x}, {sat});"]

        if n == "mrs":
            self.flag_r("fn", "fz", "fc", "fv")
            return [f"{self.w(R(0))} = (fn << 31) | (fz << 30) | (fc << 29) | (fv << 28) | ctx->apsr_rest;"]
        if n == "msr":
            src = ops[-1]
            val = u32(src.imm) if src.type == A.ARM_OP_IMM else self.r(i, self.regnum(i, src))
            self.flag_w("fn", "fz", "fc", "fv")
            t = self.new_tmp()
            return [f"{{ uint32_t {t} = {val}; fn = {t} >> 31; fz = ({t} >> 30) & 1u; fc = ({t} >> 29) & 1u;"
                    f" fv = ({t} >> 28) & 1u; ctx->apsr_rest = (ctx->apsr_rest & ~0x08000000u) | ({t} & 0x08000000u); }}"]

        # ---- VFP ----
        if n.startswith("v") or n in ("fconsts", "fconstd", "fmstat"):
            return self.vfp(i)
        raise EmitError(f"{i.addr:#x}: no translation for '{cs.mnemonic} {cs.op_str}' ({n})")

    def moves(self, i: Insn) -> list[str]:
        n, ops, uf = i.name, i.cs.operands, i.uf
        rd = self.regnum(i, ops[0])
        if n == "movt":
            return [f"{self.w(rd)} = ({self.r(i, rd)} & 0xffffu) | {u32((ops[1].imm & 0xFFFF) << 16)};"]
        if n == "movw":
            return [f"{self.w(rd)} = {u32(ops[1].imm & 0xFFFF)};"]
        if n == "adr":
            return [f"{self.w(rd)} = {u32(((self.pc(i) & ~3) if i.thumb else self.pc(i)) + ops[1].imm)};"]
        if n in ("lsl", "lsr", "asr", "ror", "rrx"):
            if len(ops) == 2 and ops[1].type == A.ARM_OP_REG and ops[1].shift.type:
                # ARM-mode alias "lsl rd, rm, #n" == "mov rd, rm, lsl #n": shift sits on the operand
                val, carry = self.shifted(i, ops[1], True)
            elif n == "rrx":
                val, carry = self.shifted(i, _FakeShift(ops[1], SFT_RRX, 0), True)
            elif len(ops) == 3 and ops[2].type == A.ARM_OP_IMM:
                st = {"lsl": SFT_LSL, "lsr": SFT_LSR, "asr": SFT_ASR, "ror": SFT_ROR}[n]
                val, carry = self.shifted(i, _FakeShift(ops[1], st, ops[2].imm), True)
            else:
                src = ops[1] if len(ops) == 3 else ops[0]
                amt = ops[2] if len(ops) == 3 else ops[1]
                st = {"lsl": A.ARM_SFT_LSL_REG, "lsr": A.ARM_SFT_LSR_REG, "asr": A.ARM_SFT_ASR_REG,
                      "ror": A.ARM_SFT_ROR_REG}[n]
                val, carry = self.shifted(i, _FakeShift(src, st, amt.reg), True)
            return self.set_result(i, rd, val, carry if uf else None, uf, logical=True)
        val, carry = self.operand2(i, ops[1], uf)
        if n == "mvn":
            val = f"~{val}"
        if rd == 15:
            is_ret = ops[1].type == A.ARM_OP_REG and self.regnum(i, ops[1]) == 14
            return self.indirect(val, i, is_ret, interworking=False)
        return self.set_result(i, rd, val, carry, uf, logical=True)

    def set_result(self, i, rd, val, carry, uf, logical) -> list[str]:
        t = self.new_tmp()
        lines = [f"{{ uint32_t {t} = {val};"]
        if uf:
            lines += ["  " + l for l in self.nz(t)]
            if carry is not None:
                self.flag_w("fc")
                lines.append(f"  fc = {carry};")
        lines.append(f"  {self.w(rd)} = {t}; }}")
        return lines

    def logical(self, i: Insn) -> list[str]:
        n, ops, uf = i.name, i.cs.operands, i.uf
        test = n in ("tst", "teq")
        if test:
            a, (b, carry) = self.r(i, self.regnum(i, ops[0])), self.operand2(i, ops[1], True)
            uf = True
        elif len(ops) == 2:
            rd = self.regnum(i, ops[0])
            a, (b, carry) = self.r(i, rd), self.operand2(i, ops[1], uf)
        else:
            rd = self.regnum(i, ops[0])
            a, (b, carry) = self.r(i, self.regnum(i, ops[1])), self.operand2(i, ops[2], uf)
        op = {"and": "&", "tst": "&", "orr": "|", "eor": "^", "teq": "^"}.get(n)
        if n == "orn":
            expr = f"{a} | ~{b}"
        elif n == "bic":
            expr = f"{a} & ~{b}"
        else:
            expr = f"{a} {op} {b}"
        t = self.new_tmp()
        lines = [f"{{ uint32_t {t} = {expr};"]
        if uf:
            lines += ["  " + l for l in self.nz(t)]
            if carry is not None:
                self.flag_w("fc")
                lines.append(f"  fc = {carry};")
        if not test:
            lines.append(f"  {self.w(rd)} = {t};")
        return lines + ["}"]

    def arith(self, i: Insn) -> list[str]:
        n, ops, uf = i.name, i.cs.operands, i.uf
        if n in ("cmp", "cmn"):
            rd, a, (b, _) = None, self.r(i, self.regnum(i, ops[0])), self.operand2(i, ops[1], False)
            uf = True
        elif len(ops) == 2:
            rd = self.regnum(i, ops[0])
            a, (b, _) = self.r(i, rd), self.operand2(i, ops[1], False)
        else:
            rd = self.regnum(i, ops[0])
            a_n = self.regnum(i, ops[1])
            a = u32((self.pc(i) & ~3) if i.thumb else self.pc(i)) if a_n == 15 else self.r(i, a_n)
            b, _ = self.operand2(i, ops[2], False)
        if n in ("rsb", "rsc"):
            a, b = b, a
        base = n.rstrip("w")
        if base in ("add", "cmn"):
            x, y, cin = a, b, "0u"
        elif base == "adc":
            x, y, cin = a, b, "fc"
        elif base in ("sub", "cmp", "rsb"):
            x, y, cin = a, f"~{b}", "1u"
        else:  # sbc / rsc
            x, y, cin = a, f"~{b}", "fc"
        if cin == "fc":
            self.flag_r("fc")
        if rd == 15:
            return self.indirect(f"({x} + {y} + {cin})", i, False, interworking=False)
        t = self.new_tmp()
        lines = [f"{{ uint32_t xa = {x}, xb = {y};"]
        if uf:
            lines.append(f"  uint64_t s64 = (uint64_t)xa + xb + {cin}; uint32_t {t} = (uint32_t)s64;")
            self.flag_w("fn", "fz", "fc", "fv")
            lines.append(f"  fn = {t} >> 31; fz = {t} == 0; fc = (uint32_t)(s64 >> 32);"
                         f" fv = ((~(xa ^ xb)) & (xa ^ {t})) >> 31;")
        else:
            lines.append(f"  uint32_t {t} = xa + xb + {cin};")
        if rd is not None:
            lines.append(f"  {self.w(rd)} = {t};")
        return lines + ["}"]

    def multiple(self, i: Insn) -> list[str]:
        n, ops, cs = i.name, i.cs.operands, i.cs
        if n in ("push", "pop"):
            base, regs, wb = 13, [self.regnum(i, o) for o in ops], True
            mode = "db" if n == "push" else "ia"
            load = n == "pop"
        else:
            base = self.regnum(i, ops[0])
            regs = [self.regnum(i, o) for o in ops[1:]]
            wb = cs.writeback
            load = n.startswith("ldm")
            mode = n[3:] or "ia"
        k = len(regs)
        b = self.r(i, base)
        start = {"ia": b, "ib": f"{b} + 4u", "db": f"{b} - {4 * k}u", "da": f"{b} - {4 * k - 4}u"}[mode]
        newb = f"{b} + {4 * k}u" if mode in ("ia", "ib") else f"{b} - {4 * k}u"
        lines = [f"{{ uint32_t a = {start}, nb = {newb};"]
        if load:
            loads = []
            for j, rr in enumerate(regs):
                loads.append((rr, f"ARC_LW(a + {4 * j}u)"))
            tmp = []
            for j, (rr, expr) in enumerate(loads):
                lines.append(f"  uint32_t v{j} = {expr};")
                tmp.append((rr, f"v{j}"))
            if wb and base not in regs:
                lines.append(f"  {self.w(base)} = nb;")
            pc_val = None
            for rr, v in tmp:
                if rr == 15:
                    pc_val = v
                else:
                    lines.append(f"  {self.w(rr)} = {v};")
            if pc_val is not None:
                lines += ["  " + l for l in self.indirect(pc_val, i, base == 13)]
        else:
            for j, rr in enumerate(regs):
                val = u32(self.pc(i)) if rr == 15 else self.r(i, rr)
                lines.append(f"  ARC_SW(a + {4 * j}u, {val});")
            if wb:
                lines.append(f"  {self.w(base)} = nb;")
        return lines + ["}"]

    # ---- VFP (scalar) -----------------------------------------------------------
    def vreg(self, i: Insn, op) -> tuple[str, int]:
        name = i.cs.reg_name(op.reg)
        if name in ("fpscr", "apsr_nzcv", "fpexc", "fpsid", "mvfr0", "mvfr1"):
            return name, -1
        return name[0], int(name[1:])

    def vget(self, kind: str, n: int) -> str:
        return f"ARC_GS(ctx, {n})" if kind == "s" else f"ARC_GD(ctx, {n})"

    def vset(self, kind: str, n: int, expr: str) -> str:
        return f"ARC_SS(ctx, {n}, {expr});" if kind == "s" else f"ARC_SD(ctx, {n}, {expr});"

    def vfp(self, i: Insn) -> list[str]:  # noqa: C901
        n, ops, cs = i.name, i.cs.operands, i.cs
        mn = cs.mnemonic
        if is_neon(i):
            from .neon import Neon, NeonError
            try:
                return Neon(self, i).translate()
            except NeonError as e:
                raise EmitError(f"{i.addr:#x}: {e}")
        if n in ("vldr", "vstr"):
            kind, k = self.vreg(i, ops[0])
            addr, _, _ = self.mem_addr(i, ops[1])
            if n == "vldr":
                if kind == "s":
                    return [f"ARC_SSB(ctx, {k}, ARC_LW({addr}));"]
                return [f"{{ uint32_t a = {addr}; ctx->d[{k}] = (uint64_t)ARC_LW(a) | ((uint64_t)ARC_LW(a + 4u) << 32); }}"]
            if kind == "s":
                return [f"ARC_SW({addr}, ARC_GSB(ctx, {k}));"]
            return [f"{{ uint32_t a = {addr}; ARC_SW(a, (uint32_t)ctx->d[{k}]); ARC_SW(a + 4u, (uint32_t)(ctx->d[{k}] >> 32)); }}"]
        if n in ("vpush", "vpop", "vldmia", "vstmia", "vldmdb", "vstmdb"):
            if n in ("vpush", "vpop"):
                base, regs, wb = 13, ops, True
                mode, load = ("db", False) if n == "vpush" else ("ia", True)
            else:
                base, regs, wb = self.regnum(i, ops[0]), ops[1:], cs.writeback
                mode, load = n[4:6], n.startswith("vldm")
            kinds = [self.vreg(i, o) for o in regs]
            size = sum(4 if k == "s" else 8 for k, _ in kinds)
            b = self.r(i, base)
            start = b if mode == "ia" else f"{b} - {size}u"
            newb = f"{b} + {size}u" if mode == "ia" else f"{b} - {size}u"
            lines = [f"{{ uint32_t a = {start}, nb = {newb};"]
            off = 0
            for kind, k in kinds:
                if kind == "s":
                    lines.append(f"  ARC_SSB(ctx, {k}, ARC_LW(a + {off}u));" if load else
                                 f"  ARC_SW(a + {off}u, ARC_GSB(ctx, {k}));")
                    off += 4
                else:
                    lines.append(f"  ctx->d[{k}] = (uint64_t)ARC_LW(a + {off}u) | ((uint64_t)ARC_LW(a + {off + 4}u) << 32);" if load
                                 else f"  ARC_SW(a + {off}u, (uint32_t)ctx->d[{k}]); ARC_SW(a + {off + 4}u, (uint32_t)(ctx->d[{k}] >> 32));")
                    off += 8
            if wb:
                lines.append(f"  {self.w(base)} = nb;")
            return lines + ["}"]
        if n in ("fconsts", "fconstd"):
            kind, k = self.vreg(i, ops[0])
            return [self.vset(kind, k, repr(float(ops[1].fp)) + ("f" if kind == "s" else ""))]
        if n == "fmstat":
            self.flag_w("fn", "fz", "fc", "fv")
            return ["fn = ctx->fpscr >> 31; fz = (ctx->fpscr >> 30) & 1u; fc = (ctx->fpscr >> 29) & 1u; fv = (ctx->fpscr >> 28) & 1u;"]
        if n == "vmrs":
            return [f"{self.w(self.regnum(i, ops[0]))} = ctx->fpscr;"]
        if n == "vmsr":
            return [f"ctx->fpscr = {self.r(i, self.regnum(i, ops[1]))};"]
        if n == "vmov":
            return self.vmov(i)
        if n in ("vadd", "vsub", "vmul", "vdiv", "vnmul"):
            kind, d = self.vreg(i, ops[0])
            _, a = self.vreg(i, ops[1])
            _, b = self.vreg(i, ops[2])
            op = {"vadd": "+", "vsub": "-", "vmul": "*", "vdiv": "/", "vnmul": "*"}[n]
            ty, fix = ("float", "ARC_FIX_S2") if kind == "s" else ("double", "ARC_FIX_D2")
            expr = f"{fix}(x {op} y, x, y)"
            if n == "vnmul":
                expr = f"-({expr})"
            return [f"{{ {ty} x = {self.vget(kind, a)}, y = {self.vget(kind, b)}; {self.vset(kind, d, expr)} }}"]
        if n in ("vmla", "vmls", "vnmla", "vnmls"):
            kind, d = self.vreg(i, ops[0])
            _, a = self.vreg(i, ops[1])
            _, b = self.vreg(i, ops[2])
            ty, fix = ("float", "ARC_FIX_S2") if kind == "s" else ("double", "ARC_FIX_D2")
            neg_p = n in ("vmls", "vnmla")
            neg_d = n in ("vnmla", "vnmls")
            return [f"{{ {ty} x = {self.vget(kind, a)}, y = {self.vget(kind, b)}, acc = {self.vget(kind, d)};",
                    f"  {ty} p = {fix}(x * y, x, y);" + (" p = -p;" if neg_p else "") + (" acc = -acc;" if neg_d else ""),
                    f"  {self.vset(kind, d, f'{fix}(acc + p, acc, p)')} }}"]
        if n in ("vneg", "vabs", "vsqrt"):
            kind, d = self.vreg(i, ops[0])
            _, a = self.vreg(i, ops[1])
            if n == "vsqrt":
                ty, fix, fn_ = ("float", "ARC_FIX_S1", "__builtin_sqrtf") if kind == "s" else ("double", "ARC_FIX_D1", "__builtin_sqrt")
                return [f"{{ {ty} x = {self.vget(kind, a)}; {self.vset(kind, d, f'{fix}({fn_}(x), x)')} }}"]
            if kind == "s":
                op = "^ 0x80000000u" if n == "vneg" else "& 0x7fffffffu"
                return [f"ARC_SSB(ctx, {d}, ARC_GSB(ctx, {a}) {op});"]
            op = "^ 0x8000000000000000ull" if n == "vneg" else "& 0x7fffffffffffffffull"
            return [f"ctx->d[{d}] = ctx->d[{a}] {op};"]
        if n in ("vcmp", "vcmpe"):
            kind, a = self.vreg(i, ops[0])
            ty = "float" if kind == "s" else "double"
            if ops[1].type == A.ARM_OP_IMM or ops[1].type == A.ARM_OP_FP:
                other = "0.0"
            else:
                _, b = self.vreg(i, ops[1])
                other = self.vget(kind, b)
            return [f"{{ {ty} x = {self.vget(kind, a)}, y = {other}; ctx->fpscr = (ctx->fpscr & 0x0fffffffu) | arc_vcmp_flags((double)x, (double)y); }}"]
        if n in ("vcvt", "vcvtr"):
            return self.vcvt(i)
        raise EmitError(f"{i.addr:#x}: no VFP translation for '{mn} {cs.op_str}' ({n})")

    def vmov(self, i: Insn) -> list[str]:
        ops, cs = i.cs.operands, i.cs
        kinds = []
        for o in ops:
            if o.type == A.ARM_OP_REG:
                nm = cs.reg_name(o.reg)
                kinds.append("r" if reg_num(cs, o.reg) is not None else nm[0])
            elif o.type == A.ARM_OP_FP:
                kinds.append("fp")
            else:
                kinds.append("imm")
        sig = "".join(k[0] for k in kinds)
        def vn(o):
            return int(cs.reg_name(o.reg)[1:])
        if sig == "rs":
            return [f"{self.w(self.regnum(i, ops[0]))} = ARC_GSB(ctx, {vn(ops[1])});"]
        if sig == "sr":
            return [f"ARC_SSB(ctx, {vn(ops[0])}, {self.r(i, self.regnum(i, ops[1]))});"]
        if sig == "rrd":
            d = vn(ops[2])
            return [f"{self.w(self.regnum(i, ops[0]))} = (uint32_t)ctx->d[{d}];",
                    f"{self.w(self.regnum(i, ops[1]))} = (uint32_t)(ctx->d[{d}] >> 32);"]
        if sig == "drr":
            d = vn(ops[0])
            return [f"ctx->d[{d}] = (uint64_t){self.r(i, self.regnum(i, ops[1]))} | ((uint64_t){self.r(i, self.regnum(i, ops[2]))} << 32);"]
        if sig == "rrss":
            a, b = vn(ops[2]), vn(ops[3])
            return [f"{self.w(self.regnum(i, ops[0]))} = ARC_GSB(ctx, {a});",
                    f"{self.w(self.regnum(i, ops[1]))} = ARC_GSB(ctx, {b});"]
        if sig == "ssrr":
            a, b = vn(ops[0]), vn(ops[1])
            return [f"ARC_SSB(ctx, {a}, {self.r(i, self.regnum(i, ops[2]))});",
                    f"ARC_SSB(ctx, {b}, {self.r(i, self.regnum(i, ops[3]))});"]
        if sig == "ss":
            return [f"ARC_SSB(ctx, {vn(ops[0])}, ARC_GSB(ctx, {vn(ops[1])}));"]
        if sig == "dd":
            return [f"ctx->d[{vn(ops[0])}] = ctx->d[{vn(ops[1])}];"]
        if sig in ("rd", "dr") and any(o.vector_index != -1 for o in ops):
            if sig == "rd":
                d, k = vn(ops[1]), ops[1].vector_index
                return [f"{self.w(self.regnum(i, ops[0]))} = (uint32_t)(ctx->d[{d}] >> {32 * k});"]
            d, k = vn(ops[0]), ops[0].vector_index
            mask = "0xffffffff00000000ull" if k == 0 else "0x00000000ffffffffull"
            return [f"ctx->d[{d}] = (ctx->d[{d}] & {mask}) | ((uint64_t){self.r(i, self.regnum(i, ops[1]))} << {32 * k});"]
        raise EmitError(f"{i.addr:#x}: vmov form '{cs.op_str}' ({sig}) not supported yet")

    def vcvt(self, i: Insn) -> list[str]:
        ops, mn = i.cs.operands, i.cs.mnemonic
        m = re.search(r"\.([suf]\d\d)\.([suf]\d\d)", mn)
        if m and len(ops) == 3 and ops[2].type == A.ARM_OP_IMM:
            # Fixed-point <-> float: scale by 2^fbits (exact), one rounding step.
            to, frm, fbits = m[1], m[2], ops[2].imm
            dk, d = self.vreg(i, ops[0])
            scale = float(2 ** fbits)
            if to[0] in "su":  # float -> fixed, round toward zero, saturate to the fixed width
                fn_ = {"s32": "arc_f2s32", "u32": "arc_f2u32", "s16": "arc_f2s16", "u16": "arc_f2u16"}[to]
                return [f"ARC_SSB(ctx, {d}, {fn_}((double){self.vget(dk, d)} * {scale!r}, 3u));"]
            src = {"s32": f"(double)(int32_t)ARC_GSB(ctx, {d})", "u32": f"(double)ARC_GSB(ctx, {d})",
                   "s16": f"(double)(int16_t)ARC_GSB(ctx, {d})", "u16": f"(double)(uint16_t)ARC_GSB(ctx, {d})"}[frm]
            ty = "float" if to == "f32" else "double"
            return [self.vset(dk, d, f"({ty})({src} / {scale!r})")]
        if not m or len(ops) != 2:
            raise EmitError(f"{i.addr:#x}: vcvt form '{mn} {i.cs.op_str}' not supported yet")
        to, frm = m[1], m[2]
        dk, d = self.vreg(i, ops[0])
        sk, s = self.vreg(i, ops[1])
        if frm in ("f32", "f64") and to in ("f32", "f64"):
            conv = "arc_cvt_d_s" if to == "f64" else "arc_cvt_s_d"
            return [self.vset(dk, d, f"{conv}({self.vget(sk, s)})")]
        if to in ("s32", "u32"):
            mode = "(ctx->fpscr >> 22) & 3u" if i.name == "vcvtr" else "3u"  # 3 = round toward zero
            fn_ = "arc_f2s32" if to == "s32" else "arc_f2u32"
            return [f"ARC_SSB(ctx, {d}, {fn_}((double){self.vget(sk, s)}, {mode}));"]
        if frm in ("s32", "u32"):
            src = f"(int32_t)ARC_GSB(ctx, {s})" if frm == "s32" else f"ARC_GSB(ctx, {s})"
            ty = "float" if dk == "s" else "double"
            return [self.vset(dk, d, f"({ty}){src}")]
        raise EmitError(f"{i.addr:#x}: vcvt {to}<-{frm} not supported yet")


NEON_ONLY = {"vld1", "vld2", "vld3", "vld4", "vst1", "vst2", "vst3", "vst4", "vdup", "vext", "vtrn", "vzip",
             "vuzp", "vswp", "vtbl", "vtbx", "vrev16", "vrev32", "vrev64", "vmovl", "vmovn", "vqmovn",
             "vqmovun", "vshrn", "vrshrn", "vqshrn", "vqrshrn", "vqshrun", "vqrshrun", "vshll", "vaddl",
             "vsubl", "vaddw", "vsubw", "vmull", "vmlal", "vmlsl", "vpadd", "vpaddl", "vpadal", "vpmax",
             "vpmin", "vbsl", "vbit", "vbif", "vand", "vorr", "veor", "vbic", "vorn", "vmvn", "vtst",
             "vceq", "vcge", "vcgt", "vcle", "vclt", "vacge", "vacgt", "vqadd", "vqsub", "vhadd", "vrhadd",
             "vhsub", "vabd", "vaba", "vabdl", "vabal", "vmax", "vmin", "vrecpe", "vrsqrte", "vrecps",
             "vrsqrts", "vshl", "vshr", "vrshr", "vsra", "vrsra", "vsri", "vsli", "vqshl", "vqshlu",
             "vrshl", "vqrshl", "vqdmulh", "vqrdmulh", "vcnt", "vclz", "vcls", "vqabs", "vqneg",
             "vaddhn", "vraddhn", "vsubhn", "vrsubhn"}
VFP_ONLY = {"vldr", "vstr", "vpush", "vpop", "vldmia", "vstmia", "vldmdb", "vstmdb", "vmrs", "vmsr",
            "fmstat", "fconsts", "fconstd", "vcmp", "vcmpe", "vsqrt", "vdiv", "vnmul", "vnmla", "vnmls",
            "vcvtr"}


def is_neon(i: Insn) -> bool:
    """Advanced SIMD vs scalar VFP share mnemonics; decide from operands and data type."""
    if i.name in VFP_ONLY:
        return False
    names = [i.cs.reg_name(o.reg) for o in i.cs.operands if o.type == A.ARM_OP_REG]
    if any(nm[0] == "q" for nm in names):
        return True
    if any(nm[0] == "s" and nm[1:].isdigit() for nm in names):
        return False
    if i.name in NEON_ONLY:
        return True
    dts = i.cs.mnemonic.split(".")[1:]
    if "f64" in dts:
        return False
    if i.name == "vmov":
        has_lane = any(o.vector_index != -1 for o in i.cs.operands)
        has_imm = any(o.type in (A.ARM_OP_IMM, A.ARM_OP_FP) for o in i.cs.operands)
        return has_lane or has_imm or (bool(dts) and dts[0] != "f64" and not any(nm[0] == "r" for nm in names))
    return any(nm[0] == "d" for nm in names)


class _FakeShift:
    """Present a register operand with an explicit shift to FnEmitter.shifted()."""

    def __init__(self, op, st, sv):
        self.reg = op.reg
        self.type = A.ARM_OP_REG

        class S:
            pass
        self.shift = S()
        self.shift.type, self.shift.value = st, sv


# ---- function assembly --------------------------------------------------------

def emit_function(prog: Program, f: Function, trace: bool = False) -> tuple[str, list[str]]:
    em = FnEmitter(prog, f)
    body: list[str] = []
    errors: list[str] = []
    if trace:  # every register is synced at every instruction, so all of them are "used"
        em.used.update(f"r{k}" for k in range(13))
        em.used.update(("sp", "lr", "fn", "fz", "fc", "fv"))
    for a in sorted(f.insns):
        i = f.insns[a]
        body.append(f"L_{a:08x}:; /* {i.cs.mnemonic} {i.cs.op_str} */")
        if trace:
            body.append(f"    {{ SYNC_OUT arc_trace_insn(ctx, {a | (1 if f.thumb else 0):#010x}u); }}")
        try:
            body += ["    " + l for l in em.insn(i)]
        except EmitError as e:
            errors.append(str(e))
            body.append(f"    arc_unimpl(ctx, {a:#010x}u);")
            continue
        except Exception as e:  # translator bug: keep going, report precisely
            errors.append(f"{a:#x}: internal error {type(e).__name__}: {e} in '{i.cs.mnemonic} {i.cs.op_str}'")
            body.append(f"    arc_unimpl(ctx, {a:#010x}u);")
        nxt = i.next
        if nxt not in f.insns and not _ends_flow(i):
            # fall-through into code that belongs to another function (or past the end)
            key = nxt | (1 if f.thumb else 0)
            target = prog.functions.get(key)
            body.append(f"    {{ SYNC_OUT {fname(target)}(ctx); return; }}" if target
                        else f"    {{ SYNC_OUT arc_dispatch(ctx, {u32(key)}, {a:#010x}u); return; }}")
    used = sorted(em.used, key=_var_order)
    written = [v for v in used if v in em.written]
    out = " ".join(f"ctx->{_field(v)} = {v};" for v in written)
    inn = " ".join(f"{v} = ctx->{_field(v)};" for v in used)
    text = "\n".join(body).replace("SYNC_OUT", out).replace("SYNC_IN", inn)
    decl = [f"    uint32_t {v} = ctx->{_field(v)};" for v in used]
    head = [f"/* {f.name} @ {f.entry:#010x} ({'Thumb' if f.thumb else 'ARM'}) */",
            f"void {fname(f)}(arc_ctx *ctx) {{"] + decl + [f"    goto L_{f.entry:08x};"]
    return "\n".join(head + [text, f"    {{ {out} return; }}", "}"]), errors


def _ends_flow(i: Insn) -> bool:
    from .analysis import _is_return, writes_pc
    if i.cs.cc not in (A.ARM_CC_AL, A.ARM_CC_INVALID):
        return False
    return i.name in ("b", "tbb", "tbh", "udf") or _is_return(i) or writes_pc(i) or \
        (i.name == "bx")


def _field(v: str) -> str:
    if v in ("fn", "fz", "fc", "fv"):
        return v[1]
    if v == "sp":
        return "r[13]"
    if v == "lr":
        return "r[14]"
    return f"r[{v[1:]}]"


def _var_order(v: str):
    order = {"sp": 13, "lr": 14, "fn": 20, "fz": 21, "fc": 22, "fv": 23}
    return order.get(v, int(v[1:]) if v[1:].isdigit() else 99)
