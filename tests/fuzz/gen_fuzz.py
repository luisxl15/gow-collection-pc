#!/usr/bin/env python3
"""Random MIPS32r2 instruction-sequence generator for differential fuzzing.

Each program seeds GPRs, FPRs and a 512-byte buffer, runs a random sequence of
instructions (straight-line ops, forward branches incl. likely variants and
branches into delay slots, calls, HI/LO, COP1), then writes the complete
architectural state (GPRs except $sp, HI/LO, FPRs, FCSR, buffer) to stdout.

Excluded on purpose (documented divergences, see docs/fidelity.md):
  * add/sub/addi overflow traps and conditional traps (terminate the process)
  * ctc1 rounding-mode changes (recompiled FP arithmetic is round-to-nearest)
"""
from __future__ import annotations

import random
import sys

POOL = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 24, 25]
BASE, INDEX = 23, 22  # $s7 = buffer pointer, $s6 = small index; never written
FP_VALUES = [0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, -2.5, 3.75, 1e-3, 1e10, -1e10, 3.0e38,
             1e-40, 123456.789, -7.25, 2147483647.5, -2147483648.75, 4294967296.0, 0.1]


def r(rng):
    return rng.choice(POOL)


def src(rng):
    return 0 if rng.random() < 0.05 else r(rng)


def dst(rng):
    return 0 if rng.random() < 0.03 else r(rng)


def fpr(rng, double):
    n = rng.randrange(0, 32, 2 if double else 1)
    return n


def simple_op(rng) -> str:
    k = rng.random()
    if k < 0.30:
        op = rng.choice(["addu", "subu", "and", "or", "xor", "nor", "slt", "sltu", "sllv", "srlv",
                         "srav", "rotrv", "movz", "movn", "mul"])
        return f"{op} ${dst(rng)}, ${src(rng)}, ${src(rng)}"
    if k < 0.40:
        op = rng.choice(["sll", "srl", "sra", "rotr"])
        return f"{op} ${dst(rng)}, ${src(rng)}, {rng.randrange(32)}"
    if k < 0.50:
        op = rng.choice(["addiu", "slti", "sltiu", "andi", "ori", "xori", "lui"])
        if op == "lui":
            return f"lui ${dst(rng)}, {rng.randrange(65536)}"
        imm = rng.randrange(-32768, 32768) if op in ("addiu", "slti", "sltiu") else rng.randrange(65536)
        return f"{op} ${dst(rng)}, ${src(rng)}, {imm}"
    if k < 0.55:
        op = rng.choice(["clz", "clo", "seb", "seh", "wsbh"])
        return f"{op} ${dst(rng)}, ${src(rng)}"
    if k < 0.58:
        pos = rng.randrange(32)
        size = rng.randrange(1, 33 - pos)
        return f"{rng.choice(['ext', 'ins'])} ${dst(rng)}, ${src(rng)}, {pos}, {size}"
    if k < 0.66:
        op = rng.choice(["mult", "multu", "div", "divu", "madd", "maddu", "msub", "msubu",
                         "mfhi", "mflo", "mthi", "mtlo"])
        if op in ("mfhi", "mflo"):
            return f"{op} ${dst(rng)}"
        if op in ("mthi", "mtlo"):
            return f"{op} ${src(rng)}"
        if op in ("div", "divu"):
            return f"{op} $0, ${src(rng)}, ${src(rng)}"
        return f"{op} ${src(rng)}, ${src(rng)}"
    if k < 0.78:
        op = rng.choice(["lb", "lbu", "lh", "lhu", "lw", "lwl", "lwr", "sb", "sh", "sw", "swl",
                         "swr"])
        align = {"lh": 2, "lhu": 2, "sh": 2, "lw": 4, "sw": 4}.get(op, 1)
        off = rng.randrange(0, 500) // align * align
        reg = dst(rng) if op.startswith("l") else src(rng)
        return f"{op} ${reg}, {off}(${BASE})"
    if k < 0.97:
        return fp_op(rng)
    return f"movt ${dst(rng)}, ${src(rng)}, $fcc{rng.randrange(8)}" if rng.random() < .5 else \
        f"movf ${dst(rng)}, ${src(rng)}, $fcc{rng.randrange(8)}"


def fp_op(rng) -> str:
    d = rng.random() < 0.5
    fmt = "d" if d else "s"
    k = rng.random()
    if k < 0.25:
        op = rng.choice(["add", "sub", "mul", "div"])
        return f"{op}.{fmt} $f{fpr(rng, d)}, $f{fpr(rng, d)}, $f{fpr(rng, d)}"
    if k < 0.35:
        op = rng.choice(["sqrt", "abs", "neg", "mov"])
        return f"{op}.{fmt} $f{fpr(rng, d)}, $f{fpr(rng, d)}"
    if k < 0.45:
        op = rng.choice(["round.w", "trunc.w", "ceil.w", "floor.w", "cvt.w"])
        return f"{op}.{fmt} $f{fpr(rng, False)}, $f{fpr(rng, d)}"
    if k < 0.52:
        conv = rng.choice(["cvt.s.d", "cvt.d.s", "cvt.s.w", "cvt.d.w"])
        dd = conv.startswith("cvt.d")
        sd = conv.endswith(".d")
        return f"{conv} $f{fpr(rng, dd)}, $f{fpr(rng, sd)}"
    if k < 0.62:
        cond = rng.choice(["f", "un", "eq", "ueq", "olt", "ult", "ole", "ule", "sf", "ngle",
                           "seq", "ngl", "lt", "nge", "le", "ngt"])
        return f"c.{cond}.{fmt} $fcc{rng.randrange(8)}, $f{fpr(rng, d)}, $f{fpr(rng, d)}"
    if k < 0.70:
        op = rng.choice(["madd", "msub", "nmadd", "nmsub"])
        return f"{op}.{fmt} $f{fpr(rng, d)}, $f{fpr(rng, d)}, $f{fpr(rng, d)}, $f{fpr(rng, d)}"
    if k < 0.76:
        op = rng.choice(["movt", "movf"])
        return f"{op}.{fmt} $f{fpr(rng, d)}, $f{fpr(rng, d)}, $fcc{rng.randrange(8)}"
    if k < 0.80:
        return f"{rng.choice(['movz', 'movn'])}.{fmt} $f{fpr(rng, d)}, $f{fpr(rng, d)}, ${src(rng)}"
    if k < 0.90:
        op = rng.choice(["mtc1", "mfc1", "mthc1", "mfhc1"])
        reg = dst(rng) if op.startswith("mf") else src(rng)
        return f"{op} ${reg}, $f{fpr(rng, op.endswith('hc1'))}"
    op = rng.choice(["lwc1", "swc1", "ldc1", "sdc1", "lwxc1", "swxc1", "ldxc1", "sdxc1"])
    dbl = op[1] == "d"
    if "x" in op:
        return f"{op} $f{fpr(rng, dbl)}, ${INDEX}(${BASE})"
    off = rng.randrange(0, 480) // (8 if dbl else 4) * (8 if dbl else 4)
    return f"{op} $f{fpr(rng, dbl)}, {off}(${BASE})"


BRANCHES = ["beq", "bne", "blez", "bgtz", "bltz", "bgez", "beql", "bnel", "blezl", "bgtzl",
            "bltzl", "bgezl", "bc1t", "bc1f", "bc1tl", "bc1fl"]


def generate(seed: int, length: int = 160) -> str:
    rng = random.Random(seed)
    body: list[tuple[str, bool]] = []  # (text, is_control)
    i = 0
    while i < length:
        if rng.random() < 0.12 and i + 3 < length:
            op = rng.choice(BRANCHES + ["j", "jal", "jalr"])
            body.append((op, True))
            body.append((simple_op(rng), False))
            i += 2
        else:
            body.append((simple_op(rng), False))
            i += 1
    out = [".set noreorder", ".set noat", ".text", ".globl main", ".ent main",
           "main:", "addiu $sp, $sp, -64", "sw $ra, 60($sp)", "sw $s7, 56($sp)",
           "sw $s6, 52($sp)", "lui $s7, %hi(fz_buf)", "addiu $s7, $s7, %lo(fz_buf)",
           f"li $s6, {rng.randrange(0, 60) * 8}"]
    for reg in POOL:
        out.append(f"li ${reg}, {rng.randrange(-2**31, 2**31)}")
    out.append("lui $at, %hi(fz_fpinit)")
    out.append("addiu $at, $at, %lo(fz_fpinit)")
    for n in range(0, 32, 2):
        out.append(f"ldc1 $f{n}, {n * 4}($at)")
    out.append("mthi $3")
    out.append("mtlo $4")
    n_ins = len(body)
    for idx, (text, ctl) in enumerate(body):
        out.append(f"L{idx}:")
        if not ctl:
            out.append(text)
            continue
        # forward target: 2..10 instructions ahead, possibly a delay slot
        tgt = min(n_ins, idx + rng.randrange(2, 10))
        label = f"L{tgt}"
        if text in ("j",):
            out.append(f"j {label}")
        elif text == "jal":
            out.append("jal fz_helper")
        elif text == "jalr":
            out.append("lui $25, %hi(fz_helper)")
            out.append("addiu $25, $25, %lo(fz_helper)")
            out.append("jalr $25")
        elif text.startswith("bc1"):
            out.append(f"{text} $fcc{rng.randrange(8)}, {label}")
        elif text in ("beq", "bne", "beql", "bnel"):
            out.append(f"{text} ${src(rng)}, ${src(rng)}, {label}")
        else:
            out.append(f"{text} ${src(rng)}, {label}")
    out.append(f"L{n_ins}:")
    # dump state
    out += ["lui $at, %hi(fz_dump)", "addiu $at, $at, %lo(fz_dump)"]
    for reg in range(1, 32):
        if reg in (1, 29):
            continue
        out.append(f"sw ${reg}, {reg * 4}($at)")
    out += ["mfhi $2", "sw $2, 128($at)", "mflo $2", "sw $2, 132($at)", "cfc1 $2, $31",
            "sw $2, 136($at)"]
    for n in range(32):
        out.append(f"swc1 $f{n}, {144 + n * 4}($at)")
    out += ["addiu $a0, $0, 1", "move $a1, $at", "jal g_write", "li $a2, 272",
            "addiu $a0, $0, 1", "move $a1, $s7", "jal g_write", "li $a2, 512",
            "lw $ra, 60($sp)", "lw $s7, 56($sp)", "lw $s6, 52($sp)", "move $v0, $0",
            "jr $ra", "addiu $sp, $sp, 64", ".end main", "",
            ".globl fz_helper", ".ent fz_helper", "fz_helper:"]
    for _ in range(rng.randrange(1, 6)):
        out.append(simple_op(rng))
    out += ["jr $ra", simple_op(rng), ".end fz_helper", "", ".data", ".align 3", "fz_fpinit:"]
    for _ in range(16):
        out.append(f".double {rng.choice(FP_VALUES)!r}" if rng.random() < .5 else
                   f".float {rng.choice(FP_VALUES)!r}, {rng.choice(FP_VALUES)!r}")
    out += [".align 3", "fz_buf:"]
    out += [f".word {rng.randrange(2**32)}" for _ in range(128)]
    out += [".bss", ".align 3", "fz_dump:", ".space 512"]
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    seed = int(sys.argv[1])
    sys.stdout.write(generate(seed, int(sys.argv[2]) if len(sys.argv) > 2 else 160))
