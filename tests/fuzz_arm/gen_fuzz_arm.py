#!/usr/bin/env python3
"""Random ARMv7 (Thumb-2 or ARM) + VFP program generator for differential fuzzing.

The program seeds registers, VFP registers and a buffer, runs a random sequence
(data processing with/without S, every shift type, modified immediates, IT
blocks, all addressing modes, LDM/STM, multiplies, bitfield/extend ops, forward
branches, calls, VFP arithmetic/conversions/compares) and writes
r0-r12, APSR, FPSCR, d0-d15 and the buffer to stdout.
"""
from __future__ import annotations

import random
import struct
import sys

POOL = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 12]  # r10 = small index, r11 = buffer base: never written
CONDS = ["eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le"]
FP_VALUES = [0.0, -0.0, 1.0, -1.0, 0.5, 1.5, 2.5, -2.5, 3.75, 1e-3, 1e10, -1e10, 3.0e38, 1e-40,
             123456.789, -7.25, 2147483647.5, -2147483648.75, 4294967296.0, 0.1, 65535.5, -32768.5]


def reg(rng):
    return f"r{rng.choice(POOL)}".replace("r12", "ip")


def modimm(rng, thumb: bool) -> int:
    b = rng.randrange(256)
    if thumb:
        k = rng.randrange(5)
        if k == 0:
            return b
        if k == 1:
            return b | b << 16
        if k == 2:
            return (b << 8) | (b << 24)
        if k == 3:
            return b * 0x01010101
        return ((0x80 | (b & 0x7F)) << rng.randrange(1, 25)) & 0xFFFFFFFF
    rot = rng.randrange(16) * 2
    return ((b >> rot) | (b << (32 - rot))) & 0xFFFFFFFF if rot else b


def shift_suffix(rng, thumb: bool, allow_reg: bool) -> str:
    k = rng.randrange(6)
    if k == 0:
        return ""
    if k == 5:
        return ", rrx"
    kind = rng.choice(["lsl", "lsr", "asr", "ror"])
    if allow_reg and not thumb and rng.random() < 0.3:
        return f", {kind} {reg(rng)}"
    lo, hi = (1, 31) if kind in ("lsl", "ror") else (1, 32)
    return f", {kind} #{rng.randint(lo, hi)}"


def dp(rng, thumb: bool, cond: str = "", in_it: bool = False) -> str:
    s = "s" if rng.random() < 0.5 else ""
    op = rng.choice(["add", "adc", "sub", "sbc", "rsb", "and", "orr", "eor", "bic", "orn", "mov", "mvn",
                     "cmp", "cmn", "tst", "teq", "lsl", "lsr", "asr", "ror"])
    if not thumb and op == "orn":
        op = "orr"
    d, a, b = reg(rng), reg(rng), reg(rng)
    wide = ""
    if op in ("cmp", "cmn", "tst", "teq"):
        if rng.random() < 0.5:
            return f"{op}{cond}{wide} {a}, #{modimm(rng, thumb)}"
        return f"{op}{cond}{wide} {a}, {b}{shift_suffix(rng, thumb, True)}"
    if op in ("mov", "mvn"):
        if rng.random() < 0.5:
            return f"{op}{s}{cond}{wide} {d}, #{modimm(rng, thumb)}"
        return f"{op}{s}{cond}{wide} {d}, {b}{shift_suffix(rng, thumb, True)}"
    if op in ("lsl", "lsr", "asr", "ror"):
        if rng.random() < 0.5:
            lo, hi = (1, 31) if op in ("lsl", "ror") else (1, 32)
            return f"{op}{s}{cond}{wide} {d}, {a}, #{rng.randint(lo, hi)}"
        return f"{op}{s}{cond}{wide} {d}, {a}, {b}"
    if rng.random() < 0.4:
        return f"{op}{s}{cond}{wide} {d}, {a}, #{modimm(rng, thumb)}"
    return f"{op}{s}{cond}{wide} {d}, {a}, {b}{shift_suffix(rng, thumb, True)}"


def misc(rng, thumb: bool, cond: str = "", allow_mem: bool = True) -> str:
    d, a, b, c = reg(rng), reg(rng), reg(rng), reg(rng)
    k = rng.randrange(12)
    if k == 0:
        if rng.random() < 0.5:   # parte alta com sinal: o clang usa em ((int64)a*b)>>32
            op = rng.choice(["smmul", "smmulr", "smmla", "smmlar", "smmls", "smmlsr"])
            return f"{op}{cond} {d}, {a}, {b}" + (f", {c}" if op.startswith(("smmla", "smmls")) else "")
        return f"mul{cond} {d}, {a}, {b}"
    if k == 1:
        return f"{rng.choice(['mla', 'mls'])}{cond} {d}, {a}, {b}, {c}"
    if k == 2:
        lo, hi = reg(rng), reg(rng)
        while hi == lo:
            hi = reg(rng)
        return f"{rng.choice(['umull', 'smull', 'umlal', 'smlal'])}{cond} {lo}, {hi}, {a}, {b}"
    if k == 3:
        return f"{rng.choice(['clz', 'rbit', 'rev', 'rev16', 'revsh'])}{cond} {d}, {a}"
    if k == 4:
        rot = rng.choice(["", ", ror #8", ", ror #16", ", ror #24"])
        return f"{rng.choice(['uxtb', 'uxth', 'sxtb', 'sxth'])}{cond} {d}, {a}{rot}"
    if k == 5:
        rot = rng.choice(["", ", ror #8", ", ror #16", ", ror #24"])
        return f"{rng.choice(['uxtab', 'uxtah', 'sxtab', 'sxtah'])}{cond} {d}, {a}, {b}{rot}"
    if k == 6:
        lsb = rng.randrange(32)
        w = rng.randint(1, 32 - lsb)
        return f"{rng.choice(['ubfx', 'sbfx'])}{cond} {d}, {a}, #{lsb}, #{w}"
    if k == 7:
        lsb = rng.randrange(32)
        w = rng.randint(1, 32 - lsb)
        return f"bfi{cond} {d}, {a}, #{lsb}, #{w}" if rng.random() < 0.7 else f"bfc{cond} {d}, #{lsb}, #{w}"
    if k == 8:
        return f"movw{cond} {d}, #{rng.randrange(65536)}"
    if k == 9:
        return f"movt{cond} {d}, #{rng.randrange(65536)}"
    if k == 10:
        sat = rng.randint(1, 31)
        return f"{rng.choice(['usat', 'ssat'])}{cond} {d}, #{sat if rng.random() < .5 else max(1, sat)}, {a}"
    return mem(rng, thumb, cond) if allow_mem else f"mul{cond} {d}, {a}, {b}"


def mem(rng, thumb: bool, cond: str = "") -> str:
    op = rng.choice(["ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "str", "strb", "strh"])
    rt = reg(rng)
    align = {"ldrh": 2, "ldrsh": 2, "strh": 2, "ldr": 4, "str": 4}.get(op, 1)
    off = rng.randrange(0, 200) // align * align
    k = rng.randrange(4)
    if k == 0:
        return f"{op}{cond} {rt}, [r11, #{off}]"
    if k == 1:
        return f"{op}{cond} {rt}, [r11, #-{rng.randrange(0, 60) // align * align}]".replace("#-0]", "#0]")
    if k == 2:  # register offset via the fixed index register r10 (kept small)
        halfword_arm = not thumb and op in ("ldrh", "ldrsh", "ldrsb", "strh")
        sh = "" if halfword_arm or rng.random() < 0.3 else f", lsl #{rng.randrange(0, 3)}"
        return f"{op}{cond} {rt}, [r11, r10{sh}]"
    # writeback forms on a scratch base (ip) that we re-point at the buffer first
    if rt == "ip":
        rt = "r0"
    pre = f"add{cond} ip, r11, #{rng.randrange(64, 160) // 4 * 4}"
    form = rng.choice([f"[ip, #{off % 32}]!", f"[ip], #{rng.choice([4, -4, 8, -8])}"])
    return f"{pre}\n {op}{cond} {rt}, {form}"


def multi(rng, thumb: bool) -> str:
    # Thumb-2 LDM/STM.W com um só registrador é UNPREDICTABLE (o GNU as o reescreve como LDR/STR;
    # o clang não, e o Unicorn rejeita): em Thumb, no mínimo dois registradores.
    regs = sorted(rng.sample([0, 1, 2, 3, 4, 5, 6, 7, 8, 9], rng.randint(2 if thumb else 1, 4)))
    lst = ", ".join(f"r{r}" for r in regs)
    off = rng.randrange(64, 128) // 4 * 4
    op = rng.choice(["ldmia", "stmia", "ldmdb", "stmdb"])
    wb = "!" if rng.random() < 0.5 else ""
    return f"add ip, r11, #{off}\n {op} ip{wb}, {{{lst}}}"


def vfp(rng) -> str:
    d = rng.random() < 0.5
    R = (lambda: f"d{rng.randrange(16)}") if d else (lambda: f"s{rng.randrange(32)}")
    t = ".f64" if d else ".f32"
    k = rng.randrange(11)
    if k <= 2:
        return f"{rng.choice(['vadd', 'vsub', 'vmul', 'vdiv', 'vnmul'])}{t} {R()}, {R()}, {R()}"
    if k == 3:
        return f"{rng.choice(['vmla', 'vmls', 'vnmla', 'vnmls'])}{t} {R()}, {R()}, {R()}"
    if k == 4:
        return f"{rng.choice(['vabs', 'vneg', 'vsqrt'])}{t} {R()}, {R()}"
    if k == 5:
        to = rng.choice(["s32", "u32"])
        r = rng.choice(["vcvt", "vcvtr"])
        return f"{r}.{to}{t} s{rng.randrange(32)}, {R()}"
    if k == 6:
        frm = rng.choice(["s32", "u32"])
        return f"vcvt{t}.{frm} {R()}, s{rng.randrange(32)}"
    if k == 7:
        return f"vcvt.f64.f32 d{rng.randrange(16)}, s{rng.randrange(32)}" if rng.random() < 0.5 else \
            f"vcvt.f32.f64 s{rng.randrange(32)}, d{rng.randrange(16)}"
    if k == 8:
        a = R()
        return f"{rng.choice(['vcmp', 'vcmpe'])}{t} {a}, {R() if rng.random() < 0.8 else '#0'}\n vmrs APSR_nzcv, fpscr"
    if k == 9:
        x = rng.randrange(32)
        fix = rng.choice(["s32", "u32", "s16", "u16"])
        bits = rng.randint(1, 16 if fix.endswith("16") else 32)
        if rng.random() < 0.5:
            return f"vcvt.{fix}.f32 s{x}, s{x}, #{bits}"
        return f"vcvt.f32.{fix} s{x}, s{x}, #{bits}"
    form = rng.randrange(6)
    if form == 0:
        return f"vmov {reg(rng)}, s{rng.randrange(32)}"
    if form == 1:
        return f"vmov s{rng.randrange(32)}, {reg(rng)}"
    if form == 2:
        a, b = reg(rng), reg(rng)
        while b == a:
            b = reg(rng)
        return f"vmov {a}, {b}, d{rng.randrange(16)}"
    if form == 3:
        return f"vmov d{rng.randrange(16)}, {reg(rng)}, {reg(rng)}"
    if form == 4:
        return f"vldr {R()}, [r11, #{rng.randrange(0, 50) * 4}]"
    return f"vstr {R()}, [r11, #{rng.randrange(0, 50) * 4}]"


def it_block(rng, thumb: bool) -> list[str]:
    cond = rng.choice(CONDS)
    inv = {"eq": "ne", "ne": "eq", "hs": "lo", "lo": "hs", "mi": "pl", "pl": "mi", "vs": "vc", "vc": "vs",
           "hi": "ls", "ls": "hi", "ge": "lt", "lt": "ge", "gt": "le", "le": "gt"}
    pattern = "".join(rng.choice("te") for _ in range(rng.randrange(0, 4)))
    lines = [f"it{pattern} {cond}"] if thumb else []
    conds = [cond] + [cond if p == "t" else inv[cond] for p in pattern]
    for c in conds:
        k = rng.random()
        if k < 0.6:
            lines.append(dp(rng, thumb, c, True))
        elif k < 0.85:
            lines.append(misc(rng, thumb, c, allow_mem=False))
        else:
            lines.append(mem(rng, thumb, c) if False else dp(rng, thumb, c, True))
    return lines


def generate(seed: int, thumb: bool = True, length: int = 120) -> str:
    rng = random.Random(seed)
    out = [".syntax unified", ".fpu neon", ".thumb" if thumb else ".arm", ".text", ".globl main",
           ".type main, %function"] + ([".thumb_func"] if thumb else []) + [
        "main:", "push {r4-r11, lr}", "vpush {d8-d15}",
        "movw r11, #:lower16:fz_buf", "movt r11, #:upper16:fz_buf",
        "movw r0, #:lower16:fz_fp", "movt r0, #:upper16:fz_fp", "vldmia r0, {d0-d15}"]
    for r in range(11):
        if r == 10:
            out.append(f"movw r10, #{rng.randrange(0, 16)}")
            continue
        v = rng.randrange(2 ** 32)
        out.append(f"movw r{r}, #{v & 0xFFFF}")
        out.append(f"movt r{r}, #{v >> 16}")
    out.append(f"movw ip, #{rng.randrange(65536)}")
    nz = rng.randrange(16)
    out.append(f"movw r0, #0\nmovt r0, #{nz << 12}\nmsr APSR_nzcvq, r0")
    labels = 0
    i = 0
    while i < length:
        k = rng.random()
        if k < 0.40:
            out.append(dp(rng, thumb))
        elif k < 0.58:
            out.append(misc(rng, thumb))
        elif k < 0.63:
            out.append(multi(rng, thumb))
        elif k < 0.80:
            out.append(vfp(rng))
        elif k < 0.88:
            out += it_block(rng, thumb) if thumb else [dp(rng, thumb, rng.choice(CONDS)) for _ in range(2)]
        elif k < 0.95:
            labels += 1
            cond = rng.choice(CONDS + [""])
            out.append(f"b{cond} 9{labels}f")
            for _ in range(rng.randrange(1, 4)):
                out.append(dp(rng, thumb))
            out.append(f"9{labels}:")
        else:
            out.append("bl fz_helper")
        i += 1
    # dump
    out += ["movw ip, #:lower16:fz_dump", "movt ip, #:upper16:fz_dump",
            "stmia ip, {r0-r10}", "str r11, [ip, #44]", "mrs r0, APSR", "str r0, [ip, #48]",
            "vmrs r0, fpscr", "str r0, [ip, #52]", "add r0, ip, #56", "vstmia r0, {d0-d15}",
            "movs r0, #1", "mov r1, ip", "movs r2, #184", "bl g_write",
            "movs r0, #1", "mov r1, r11", "movs r2, #200", "bl g_write",
            "movs r0, #0", "vpop {d8-d15}", "pop {r4-r11, pc}", ""]
    out += [".type fz_helper, %function"] + ([".thumb_func"] if thumb else []) + ["fz_helper:"]
    for _ in range(rng.randrange(1, 5)):
        out.append(dp(rng, thumb))
    out += ["bx lr", ".data", ".align 3", "fz_fp:"]
    for _ in range(16):
        if rng.random() < 0.5:
            out.append(f".double {rng.choice(FP_VALUES)!r}")
        else:
            a, b = (struct.unpack('<f', struct.pack('<f', rng.choice(FP_VALUES)))[0] for _ in range(2))
            out.append(f".float {a!r}, {b!r}")
    out += [".align 3", "fz_buf:"] + [f".word {rng.randrange(2 ** 32)}" for _ in range(64)]
    out += [".bss", ".align 3", "fz_dump:", ".space 256"]
    return "\n".join(o if o.endswith(":") or o.startswith(".") else " " + o for o in out) + "\n"


if __name__ == "__main__":
    sys.stdout.write(generate(int(sys.argv[1]), sys.argv[2] != "arm" if len(sys.argv) > 2 else True))
