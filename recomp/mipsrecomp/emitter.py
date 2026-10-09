"""Translate analysed MIPS functions into portable C99 (plus GCC builtins).

Model
-----
* Guest state lives in ``rc_ctx`` (GPRs, HI/LO, FPRs as raw 32-bit words with
  FR=0 pairing, FCSR). Guest RAM is a flat little-endian array ``rc_mem``.
* Every guest function becomes one C function ``void fn(rc_ctx *ctx)``.
  ``jal`` -> direct C call, ``jr $ra`` -> ``return``, ``j`` to another
  function -> tail call, ``jalr``/computed ``jr`` -> table dispatch.
* Delay slots are reproduced exactly: the branch condition is evaluated before
  the delay-slot instruction runs; likely branches annul the slot.
"""
from __future__ import annotations

import re

from .analysis import Function, Program
from .decoder import (BRANCH, BRANCH_LIKELY, BRANCH_LINK, CALL, CALL_REG, FMT_D, FMT_S, FMT_W,
                      JUMP, JUMP_REG, Insn)


class EmitError(Exception):
    pass


def R(n: int) -> str:
    return "0u" if n == 0 else f"ctx->r[{n}]"


def W(n: int, expr: str) -> list[str]:
    """Write a GPR; writes to $zero are dropped (the expression is still pure)."""
    return [] if n == 0 else [f"ctx->r[{n}] = {expr};"]


def S(n: int) -> str:
    return f"(int32_t){R(n)}"


def ea(i: Insn) -> str:
    return f"{R(i.rs)} + {i.simm}u" if i.simm >= 0 else f"{R(i.rs)} - {-i.simm}u"


def cc_mask(cc: int) -> int:
    return 1 << 23 if cc == 0 else 1 << (24 + cc)


def fname(f: Function) -> str:
    return f"rc_{f.start:08x}_{f.name}"[:96]


class Emitter:
    def __init__(self, prog: Program, trace: bool = False, trace_insn: bool = False,
                 locals_: bool = True):
        self.prog = prog
        self.locals = locals_
        self.trace = trace
        self.trace_insn = trace_insn

    def tr(self, addr: int) -> list[str]:
        return [f"    rc_trace_insn(ctx, {addr:#010x}u);"] if self.trace_insn else []

    # ---- helpers ------------------------------------------------------------
    def call_expr(self, target: int, origin: Insn) -> str:
        f = self.prog.functions.get(target)
        if f is None:
            raise EmitError(f"{origin}: call target {target:#010x} is not a known function")
        return f"{fname(f)}(ctx);"

    def cond(self, i: Insn) -> str:
        op = i.op
        if op in ("beq", "beql"):
            return f"{R(i.rs)} == {R(i.rt)}"
        if op in ("bne", "bnel"):
            return f"{R(i.rs)} != {R(i.rt)}"
        if op in ("blez", "blezl"):
            return f"{S(i.rs)} <= 0"
        if op in ("bgtz", "bgtzl"):
            return f"{S(i.rs)} > 0"
        if op in ("bltz", "bltzl", "bltzal", "bltzall"):
            return f"{S(i.rs)} < 0"
        if op in ("bgez", "bgezl", "bgezal", "bgezall"):
            return f"{S(i.rs)} >= 0"
        if op in ("bc1t", "bc1tl"):
            return f"(ctx->fcr31 & {cc_mask(i.cc):#x}u) != 0"
        if op in ("bc1f", "bc1fl"):
            return f"(ctx->fcr31 & {cc_mask(i.cc):#x}u) == 0"
        raise EmitError(f"{i}: no condition for {op}")

    # ---- straight-line instructions ----------------------------------------
    def simple(self, i: Insn) -> list[str]:  # noqa: C901 - a big dispatch is the clearest form
        op, rs, rt, rd, sa = i.op, i.rs, i.rt, i.rd, i.sa
        pc = f"{i.addr:#010x}u"
        if i.word >> 26 in (0x11, 0x13):  # COP1 / COP1X share mnemonics with integer ops
            return self.cop1(i)
        if op == "sll":
            if rd == 0:
                return ["/* nop */"]
            return W(rd, f"{R(rt)} << {sa}")
        if op == "srl":
            return W(rd, f"{R(rt)} >> {sa}")
        if op == "sra":
            return W(rd, f"(uint32_t)({S(rt)} >> {sa})")
        if op == "rotr":
            return W(rd, f"RC_ROTR({R(rt)}, {sa})")
        if op == "sllv":
            return W(rd, f"{R(rt)} << ({R(rs)} & 31)")
        if op == "srlv":
            return W(rd, f"{R(rt)} >> ({R(rs)} & 31)")
        if op == "srav":
            return W(rd, f"(uint32_t)({S(rt)} >> ({R(rs)} & 31))")
        if op == "rotrv":
            return W(rd, f"RC_ROTR({R(rt)}, {R(rs)} & 31)")
        if op == "movz":
            return [f"if ({R(rt)} == 0) {{ {' '.join(W(rd, R(rs)))} }}"]
        if op == "movn":
            return [f"if ({R(rt)} != 0) {{ {' '.join(W(rd, R(rs)))} }}"]
        if op in ("movf", "movt"):
            test = "!=" if op == "movt" else "=="
            return [f"if ((ctx->fcr31 & {cc_mask(i.cc):#x}u) {test} 0) {{ {' '.join(W(rd, R(rs)))} }}"]
        if op == "syscall":
            return [f"ctx->pc = {pc}; rc_syscall(ctx);"]
        if op == "break":
            return [f"rc_break(ctx, {pc}, {(i.word >> 6) & 0xFFFFF:#x});"]
        if op in ("sync", "cache", "pref", "prefx"):
            return [f"/* {op} */"]
        if op == "mfhi":
            return W(rd, "ctx->hi")
        if op == "mflo":
            return W(rd, "ctx->lo")
        if op == "mthi":
            return [f"ctx->hi = {R(rs)};"]
        if op == "mtlo":
            return [f"ctx->lo = {R(rs)};"]
        if op == "mult":
            return [f"{{ int64_t p = (int64_t){S(rs)} * {S(rt)}; ctx->lo = (uint32_t)p; ctx->hi = (uint32_t)((uint64_t)p >> 32); }}"]
        if op == "multu":
            return [f"{{ uint64_t p = (uint64_t){R(rs)} * {R(rt)}; ctx->lo = (uint32_t)p; ctx->hi = (uint32_t)(p >> 32); }}"]
        if op in ("madd", "maddu", "msub", "msubu"):
            signed = op in ("madd", "msub")
            prod = f"(uint64_t)((int64_t){S(rs)} * {S(rt)})" if signed else f"(uint64_t){R(rs)} * {R(rt)}"
            sign = "+" if op.startswith("madd") else "-"
            return [f"{{ uint64_t acc = ((uint64_t)ctx->hi << 32) | ctx->lo; acc = acc {sign} {prod}; ctx->lo = (uint32_t)acc; ctx->hi = (uint32_t)(acc >> 32); }}"]
        if op == "div":
            return [f"rc_div(ctx, {R(rs)}, {R(rt)});"]
        if op == "divu":
            return [f"rc_divu(ctx, {R(rs)}, {R(rt)});"]
        if op == "add":
            return [f"{{ uint32_t s = {R(rs)} + {R(rt)}; if ((({R(rs)} ^ s) & ({R(rt)} ^ s)) >> 31) rc_overflow(ctx, {pc}); {' '.join(W(rd, 's'))} }}"]
        if op == "sub":
            return [f"{{ uint32_t s = {R(rs)} - {R(rt)}; if ((({R(rs)} ^ {R(rt)}) & ({R(rs)} ^ s)) >> 31) rc_overflow(ctx, {pc}); {' '.join(W(rd, 's'))} }}"]
        if op == "addi":
            return [f"{{ uint32_t s = {R(rs)} + {i.simm & 0xFFFFFFFF:#x}u; if ((({R(rs)} ^ s) & ({i.simm & 0xFFFFFFFF:#x}u ^ s)) >> 31) rc_overflow(ctx, {pc}); {' '.join(W(rt, 's'))} }}"]
        binops = {"addu": "+", "subu": "-", "and": "&", "or": "|", "xor": "^"}
        if op in binops:
            return W(rd, f"{R(rs)} {binops[op]} {R(rt)}")
        if op == "nor":
            return W(rd, f"~({R(rs)} | {R(rt)})")
        if op == "slt":
            return W(rd, f"{S(rs)} < {S(rt)}")
        if op == "sltu":
            return W(rd, f"{R(rs)} < {R(rt)}")
        if op == "addiu":
            return W(rt, ea(i))
        if op == "slti":
            return W(rt, f"{S(rs)} < {i.simm}")
        if op == "sltiu":
            return W(rt, f"{R(rs)} < {i.simm & 0xFFFFFFFF:#x}u")
        if op == "andi":
            return W(rt, f"{R(rs)} & {i.imm:#x}u")
        if op == "ori":
            return W(rt, f"{R(rs)} | {i.imm:#x}u")
        if op == "xori":
            return W(rt, f"{R(rs)} ^ {i.imm:#x}u")
        if op == "lui":
            return W(rt, f"{i.imm << 16:#x}u")
        traps = {"tge": ">=", "tlt": "<", "teq": "==", "tne": "!=", "tgeu": ">=", "tltu": "<"}
        if op in traps:
            a, b = (S(rs), S(rt)) if op in ("tge", "tlt") else (R(rs), R(rt))
            return [f"if ({a} {traps[op]} {b}) rc_trap(ctx, {pc});"]
        itraps = {"tgei": ">=", "tlti": "<", "teqi": "==", "tnei": "!=", "tgeiu": ">=", "tltiu": "<"}
        if op in itraps:
            if op in ("tgeiu", "tltiu"):
                a, b = R(rs), f"{i.simm & 0xFFFFFFFF:#x}u"
            else:
                a, b = S(rs), str(i.simm)
            return [f"if ({a} {itraps[op]} {b}) rc_trap(ctx, {pc});"]
        loads = {"lb": "RC_LB", "lbu": "RC_LBU", "lh": "RC_LH", "lhu": "RC_LHU", "lw": "RC_LW",
                 "ll": "RC_LW"}
        if op in loads:
            return W(rt, f"{loads[op]}({ea(i)})")
        if op in ("lwl", "lwr"):
            return W(rt, f"rc_{op}({ea(i)}, {R(rt)})")
        stores = {"sb": "RC_SB", "sh": "RC_SH", "sw": "RC_SW"}
        if op in stores:
            return [f"{stores[op]}({ea(i)}, {R(rt)});"]
        if op == "sc":
            return [f"RC_SW({ea(i)}, {R(rt)});"] + W(rt, "1u")
        if op in ("swl", "swr"):
            return [f"rc_{op}({ea(i)}, {R(rt)});"]
        if op == "mul":
            # Unsigned multiply: signed overflow would be UB in C (and GCC exploits it).
            return W(rd, f"{R(rs)} * {R(rt)}")
        if op == "clz":
            return W(rd, f"RC_CLZ({R(rs)})")
        if op == "clo":
            return W(rd, f"RC_CLZ(~{R(rs)})")
        if op == "ext":
            size = rd + 1
            mask = (1 << size) - 1 if size < 32 else 0xFFFFFFFF
            return W(rt, f"({R(rs)} >> {sa}) & {mask:#x}u")
        if op == "ins":
            size = rd - sa + 1
            if size <= 0:
                raise EmitError(f"{i}: invalid ins field")
            mask = ((1 << size) - 1 if size < 32 else 0xFFFFFFFF) << sa & 0xFFFFFFFF
            return W(rt, f"({R(rt)} & {~mask & 0xFFFFFFFF:#x}u) | (({R(rs)} << {sa}) & {mask:#x}u)")
        if op == "wsbh":
            return W(rd, f"(({R(rt)} & 0x00ff00ffu) << 8) | (({R(rt)} >> 8) & 0x00ff00ffu)")
        if op == "seb":
            return W(rd, f"(uint32_t)(int32_t)(int8_t){R(rt)}")
        if op == "seh":
            return W(rd, f"(uint32_t)(int32_t)(int16_t){R(rt)}")
        if op == "rdhwr":
            if rd != 29:
                raise EmitError(f"{i}: rdhwr ${rd} unsupported (only $29 ULR)")
            return W(rt, "ctx->tls")
        return self.cop1(i)

    def cop1(self, i: Insn) -> list[str]:
        op, fmt = i.op, i.fmt
        fs, ft, fd = i.fs, i.ft, i.fd
        if op == "mfc1":
            return W(i.rt, f"ctx->f[{fs}]")
        if op == "mtc1":
            return [f"ctx->f[{fs}] = {R(i.rt)};"]
        if op == "mfhc1":
            return W(i.rt, f"ctx->f[{fs | 1}]")
        if op == "mthc1":
            return [f"ctx->f[{fs | 1}] = {R(i.rt)};"]
        if op == "cfc1":
            return W(i.rt, f"rc_cfc1(ctx, {fs})")
        if op == "ctc1":
            return [f"rc_ctc1(ctx, {fs}, {R(i.rt)});"]
        if op in ("lwc1", "swc1", "ldc1", "sdc1"):
            ft = i.rt  # primary-opcode FPU loads/stores encode the FPR in the rt field
        if op == "lwc1":
            return [f"ctx->f[{ft}] = RC_LW({ea(i)});"]
        if op == "swc1":
            return [f"RC_SW({ea(i)}, ctx->f[{ft}]);"]
        if op == "ldc1":
            return [f"{{ uint32_t a = {ea(i)}; ctx->f[{ft & ~1}] = RC_LW(a); ctx->f[{ft | 1}] = RC_LW(a + 4); }}"]
        if op == "sdc1":
            return [f"{{ uint32_t a = {ea(i)}; RC_SW(a, ctx->f[{ft & ~1}]); RC_SW(a + 4, ctx->f[{ft | 1}]); }}"]
        if op in ("lwxc1", "ldxc1", "swxc1", "sdxc1"):
            addr = f"{R(i.fr)} + {R(i.ft)}"
            if op == "lwxc1":
                return [f"ctx->f[{fd}] = RC_LW({addr});"]
            if op == "swxc1":
                return [f"RC_SW({addr}, ctx->f[{fs}]);"]
            if op == "ldxc1":
                return [f"{{ uint32_t a = {addr}; ctx->f[{fd & ~1}] = RC_LW(a); ctx->f[{fd | 1}] = RC_LW(a + 4); }}"]
            return [f"{{ uint32_t a = {addr}; RC_SW(a, ctx->f[{fs & ~1}]); RC_SW(a + 4, ctx->f[{fs | 1}]); }}"]

        if fmt == FMT_S:
            get, put, ty = "RC_FS", "RC_SET_FS", "float"
        elif fmt == FMT_D:
            get, put, ty = "RC_FD", "RC_SET_FD", "double"
        elif fmt == FMT_W:
            get, put, ty = None, None, "int32_t"
        else:
            raise EmitError(f"{i}: unsupported FPU format {fmt}")

        def g(n: int) -> str:
            return f"{get}(ctx, {n})"

        def p(n: int, expr: str) -> list[str]:
            return [f"{put}(ctx, {n}, {expr});"]

        fix = "S" if fmt == FMT_S else "D"
        arith = {"add": "+", "sub": "-", "mul": "*", "div": "/"}
        if op in arith:
            return [f"{{ {ty} a = {g(fs)}, b = {g(ft)}, r = a {arith[op]} b; "
                    f"{put}(ctx, {fd}, RC_FIX_{fix}2(r, a, b)); }}"]
        if op == "sqrt":
            fn = "__builtin_sqrtf" if fmt == FMT_S else "__builtin_sqrt"
            return [f"{{ {ty} a = {g(fs)}, r = {fn}(a); {put}(ctx, {fd}, RC_FIX_{fix}1(r, a)); }}"]
        if op in ("abs", "neg", "mov"):
            hi = fs | 1 if fmt == FMT_D else fs
            hd = fd | 1 if fmt == FMT_D else fd
            lines = []
            if fmt == FMT_D:
                lines.append(f"ctx->f[{fd & ~1}] = ctx->f[{fs & ~1}];")
            expr = {"abs": f"ctx->f[{hi}] & 0x7fffffffu", "neg": f"ctx->f[{hi}] ^ 0x80000000u",
                    "mov": f"ctx->f[{hi}]"}[op]
            # High word last so fd == fs aliasing is harmless.
            return lines + [f"ctx->f[{hd}] = {expr};"]
        if op in ("round.w", "trunc.w", "ceil.w", "floor.w", "cvt.w"):
            mode = {"round.w": "0", "trunc.w": "1", "ceil.w": "2", "floor.w": "3",
                    "cvt.w": "(ctx->fcr31 & 3)"}[op]
            return [f"ctx->f[{fd}] = (uint32_t)rc_to_w((double){g(fs)}, {mode});"]
        if op == "cvt.s":
            if fmt == FMT_W:
                return [f"RC_SET_FS(ctx, {fd}, (float)(int32_t)ctx->f[{fs}]);"]
            return [f"RC_SET_FS(ctx, {fd}, rc_cvt_s_d({g(fs)}));"]
        if op == "cvt.d":
            if fmt == FMT_W:
                return [f"RC_SET_FD(ctx, {fd}, (double)(int32_t)ctx->f[{fs}]);"]
            return [f"RC_SET_FD(ctx, {fd}, rc_cvt_d_s({g(fs)}));"]
        if op in ("movf.fmt", "movt.fmt", "movz.fmt", "movn.fmt"):
            if op == "movz.fmt":
                test = f"{R(i.ft)} == 0"
            elif op == "movn.fmt":
                test = f"{R(i.ft)} != 0"
            else:
                test = f"(ctx->fcr31 & {cc_mask(i.cc):#x}u) {'!=' if op == 'movt.fmt' else '=='} 0"
            if fmt == FMT_D:
                body = f"ctx->f[{fd & ~1}] = ctx->f[{fs & ~1}]; ctx->f[{fd | 1}] = ctx->f[{fs | 1}];"
            else:
                body = f"ctx->f[{fd}] = ctx->f[{fs}];"
            return [f"if ({test}) {{ {body} }}"]
        if op == "c":
            m = cc_mask(i.cc)
            parts = []
            if i.cond & 1:
                parts.append("(a != a || b != b)")
            if i.cond & 2:
                parts.append("a == b")
            if i.cond & 4:
                parts.append("a < b")
            expr = " || ".join(parts) or "0"
            return [f"{{ {ty} a = {g(fs)}, b = {g(ft)}; if ({expr}) ctx->fcr31 |= {m:#x}u; else ctx->fcr31 &= ~{m:#x}u; }}"]
        if op in ("madd.fmt", "msub.fmt", "nmadd.fmt", "nmsub.fmt"):
            # MIPS32r2 legacy multiply-add is unfused: round after mul, then after add.
            # Each step gets its own NaN fix-up; the final negation is a sign-bit flip.
            sign = "-" if op.startswith("msub") or op.startswith("nmsub") else "+"
            neg = op.startswith("n")
            lines = [f"{{ {ty} x = {g(fs)}, y = {g(ft)}, z = {g(i.fr)}, m = x * y;",
                     f"  m = RC_FIX_{fix}2(m, x, y); {ty} r = m {sign} z; r = RC_FIX_{fix}2(r, m, z);"]
            if neg:
                lines.append(f"  r = -r;")
            lines.append(f"  {put}(ctx, {fd}, r); }}")
            return [" ".join(lines)]
        raise EmitError(f"{i}: no translation for {op}")

    # ---- control flow --------------------------------------------------------
    def control(self, f: Function, i: Insn, delay: Insn) -> list[str]:
        if delay.flow is not None:
            raise EmitError(f"{i}: control-flow instruction in delay slot ({delay})")
        dslot = [t.strip() for t in self.tr(delay.addr)] + self.simple(delay)
        ret_addr = f"{i.addr + 8:#010x}u"
        out: list[str] = []
        if i.flow in (BRANCH, BRANCH_LIKELY):
            target_in = f.contains(i.target)
            goto = f"goto L_{i.target:08x};" if target_in else f"{{ {self.call_expr(i.target, i)} return; }}"
            if i.flow == BRANCH_LIKELY:
                out.append(f"if ({self.cond(i)}) {{")
                out += ["    " + s for s in dslot]
                out.append(f"    {goto}")
                out.append("}")
            elif i.op == "beq" and i.rs == i.rt:  # b
                out += dslot + [goto]
            else:
                out.append(f"bc = {self.cond(i)};")
                out += dslot
                out.append(f"if (bc) {goto}")
            return out
        if i.flow == BRANCH_LINK:
            uncond = i.op == "bgezal" and i.rs == 0
            if not uncond:
                out.append(f"bc = {self.cond(i)};")
            out += W(31, ret_addr)
            out += dslot
            call = self.call_expr(i.target, i)
            out.append(call if uncond else f"if (bc) {call}")
            return out
        if i.flow == JUMP:
            out += dslot
            if f.contains(i.target):
                out.append(f"goto L_{i.target:08x};")
            else:
                out += [self.call_expr(i.target, i), "return;"]
            return out
        if i.flow == CALL:
            out += W(31, ret_addr)
            out += dslot
            out.append(self.call_expr(i.target, i))
            return out
        if i.flow == CALL_REG:
            out.append(f"{{ uint32_t t = {R(i.rs)};")
            out += ["    " + s for s in W(i.rd, ret_addr)]
            out += ["    " + s for s in dslot]
            out.append(f"    rc_dispatch(ctx, t, {i.addr:#010x}u); }}")
            return out
        if i.flow == JUMP_REG:
            if i.rs == 31:
                return dslot + ["return;"]
            out.append(f"{{ uint32_t t = {R(i.rs)};")
            out += ["    " + s for s in dslot]
            if f.jump_table_targets:
                out.append("    switch (t) {")
                for tgt in sorted(f.jump_table_targets):
                    out.append(f"    case {tgt:#010x}u: goto L_{tgt:08x};")
                out.append("    default: break;")
                out.append("    }")
            out.append(f"    rc_dispatch(ctx, t, {i.addr:#010x}u); return; }}")
            return out
        raise EmitError(f"{i}: unhandled control flow")

    # ---- functions -------------------------------------------------------------
    def function(self, f: Function) -> str:
        lines = [f"/* {f.name} @ {f.start:#010x} chunks={[(hex(s), hex(e)) for s, e in f.chunks]} */",
                 f"void {fname(f)}(rc_ctx *ctx) {{", "    uint32_t bc; (void)bc;"]
        if self.trace:
            lines.append(f"    rc_trace_enter(ctx, {f.start:#010x}u);")
        for ci, (s, e) in enumerate(f.chunks):
            a = s
            last_flow_uncond = False
            while a < e:
                i = f.insns[a]
                if a in f.labels:
                    lines.append(f"L_{a:08x}:;")
                lines += self.tr(a)
                if i.op is None:
                    lines.append(f"    rc_unknown(ctx, {a:#010x}u, {i.word:#010x}u);")
                    a += 4
                    continue
                if i.flow is None:
                    body = self.simple(i)
                    lines += [f"    {b}" for b in body] if body else []
                    last_flow_uncond = False
                    a += 4
                    continue
                if a + 4 >= e:
                    raise EmitError(f"{i}: delay slot falls outside function {f.name}")
                delay = f.insns[a + 4]
                lines.append(f"    /* {a:08x}: {i.op} */")
                lines += [f"    {b}" for b in self.control(f, i, delay)]
                if a + 4 in f.labels:
                    # Someone branches straight into the delay slot: give it its own copy.
                    lines.append(f"    goto N_{a + 8:08x};")
                    lines.append(f"L_{a + 4:08x}:;")
                    lines += self.tr(a + 4)
                    lines += [f"    {b}" for b in self.simple(delay)]
                    lines.append(f"N_{a + 8:08x}:;")
                last_flow_uncond = (i.flow in (JUMP, JUMP_REG)) or (i.op == "beq" and i.rs == i.rt)
                a += 8
            if not last_flow_uncond:
                nxt = self.prog.functions.get(e)
                if nxt is not None:
                    lines.append(f"    {fname(nxt)}(ctx); return; /* falls through */")
                else:
                    lines.append(f"    rc_fallthrough(ctx, {e:#010x}u);")
        lines.append("}")
        return "\n".join(localize(lines)) if self.locals else "\n".join(lines)


# ---- register promotion ------------------------------------------------------
# Guest registers are rewritten from ctx fields into C locals so the host
# compiler can keep them in machine registers (guest memory stores through
# rc_mem would otherwise force reloads of every ctx field). State is written
# back to ctx before calls/syscalls/returns and reloaded after calls.

_CALL_RE = re.compile(r"\b((?:rc_[0-9a-f]{8}_\w+)|rc_dispatch|rc_syscall|rc_trace_insn)\((ctx[^;]*)\);")
_TAIL_RE = re.compile(r"\b(rc_[0-9a-f]{8}_\w+|rc_dispatch)\((ctx[^;]*)\); return;")
_ASSIGN_RE = re.compile(r"\b(r\d+|f\d+|hi|lo|fcsr) (?:[|&]?=)(?!=)")


PROMOTE_FPRS = True


def localize(lines: list[str]) -> list[str]:
    head, body = lines[:2], lines[2:-1]
    text = "\n".join(body)
    gprs: set[int] = set()
    fprs: set[int] = set()
    written: set[str] = set()

    def gpr(m):
        gprs.add(int(m[1]))
        return f"r{m[1]}"

    def fpr(m):
        fprs.add(int(m[1]))
        return f"f{m[1]}"

    def fs(m):
        fprs.add(int(m[1]))
        return f"RC_LFS({m[1]})"

    def fd(m):
        n = int(m[1])
        fprs.update((n & ~1, n | 1))
        return f"RC_LFD({n & ~1}, {n | 1})"

    def set_fs(m):
        fprs.add(int(m[1]))
        written.add(f"f{m[1]}")
        return f"RC_LSET_FS({m[1]},"

    def set_fd(m):
        n = int(m[1])
        fprs.update((n & ~1, n | 1))
        written.update((f"f{n & ~1}", f"f{n | 1}"))
        return f"RC_LSET_FD({n & ~1}, {n | 1},"

    text = re.sub(r"ctx->r\[(\d+)\]", gpr, text)
    if PROMOTE_FPRS:
        text = re.sub(r"ctx->f\[(\d+)\]", fpr, text)
        text = re.sub(r"RC_FS\(ctx, (\d+)\)", fs, text)
        text = re.sub(r"RC_FD\(ctx, (\d+)\)", fd, text)
        text = re.sub(r"RC_SET_FS\(ctx, (\d+),", set_fs, text)
        text = re.sub(r"RC_SET_FD\(ctx, (\d+),", set_fd, text)
        text = text.replace("ctx->fcr31", "fcsr")
    text = text.replace("ctx->hi", "hi").replace("ctx->lo", "lo")
    if "rc_div(ctx, " in text or "rc_divu(ctx, " in text:
        written.update(("hi", "lo"))
    text = text.replace("rc_divu(ctx, ", "RC_DIVU(").replace("rc_div(ctx, ", "RC_DIV(")
    text = re.sub(r"rc_cfc1\(ctx, (\d+)\)", r"RC_CFC1(\1)", text)
    if PROMOTE_FPRS:
        if "rc_ctc1(ctx, " in text:
            written.add("fcsr")
        text = text.replace("rc_ctc1(ctx, ", "RC_CTC1(")
    else:
        text = re.sub(r"RC_CFC1\((\d+)\)", r"rc_cfc1(ctx, \1)", text)
    written.update(_ASSIGN_RE.findall(text))

    special = [n for n in (("hi", "lo", "fcsr") if PROMOTE_FPRS else ("hi", "lo")) if re.search(rf"\b{n}\b", text) or
               (n in ("hi", "lo") and "RC_DIV" in text) or (n == "fcsr" and "RC_C" in text)]
    field = {"hi": "hi", "lo": "lo", "fcsr": "fcr31"}
    used = [f"r{n}" for n in sorted(gprs)] + [f"f{n}" for n in sorted(fprs)] + special

    def ctx_field(v: str) -> str:
        if v[0] == "r" and v[1:].isdigit():
            return f"ctx->r[{v[1:]}]"
        if v[0] == "f" and v[1:].isdigit():
            return f"ctx->f[{v[1:]}]"
        return f"ctx->{field[v]}"

    out_vars = [v for v in used if v in written]
    sync_out = " ".join(f"{ctx_field(v)} = {v};" for v in out_vars)
    sync_in = " ".join(f"{v} = {ctx_field(v)};" for v in used)

    text = _TAIL_RE.sub(lambda m: f"{{ {sync_out} {m[1]}({m[2]}); return; }}", text)
    text = _CALL_RE.sub(lambda m: f"{{ {sync_out} {m[1]}({m[2]}); {sync_in} }}", text)
    text = re.sub(r"(?<![{;] )\breturn;", f"{{ {sync_out} return; }}", text)
    # Returns already guarded by a tail-call sync above keep a single flush.
    decls = [f"    uint32_t {v} = {ctx_field(v)};" for v in used]
    return head + decls + text.split("\n") + ["    " + sync_out, lines[-1]]
