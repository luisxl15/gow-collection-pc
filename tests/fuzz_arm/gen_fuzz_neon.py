#!/usr/bin/env python3
"""Random NEON program generator (Thumb-2). Dumps d0-d31 and a 256-byte buffer.

Inputs mix random integers with float specials (denormals, NaNs, infinities,
signed zeros) so the "standard FPSCR" rules (flush-to-zero, default NaN) are
exercised.
"""
from __future__ import annotations

import random
import struct
import sys

SPECIAL_F32 = [0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000, 0xff800000, 0x7fc00000,
               0x7f800001, 0xffc12345, 0x00000001, 0x80400000, 0x007fffff, 0x00800000, 0x7f7fffff,
               0x40490fdb, 0x4b000000, 0xcf000000, 0x4f800000, 0x3eaaaaab, 0x47800000]
I_DT = ["i8", "i16", "i32"]
S_DT = ["s8", "s16", "s32", "u8", "u16", "u32"]


def q(rng):
    return f"q{rng.randrange(16)}"


def d(rng):
    return f"d{rng.randrange(32)}"


def vec(rng):
    return (q, "q") if rng.random() < 0.5 else (d, "d")


def instr(rng) -> str:
    R, kind = vec(rng)
    k = rng.randrange(30)
    if k == 0:
        return f"{rng.choice(['vadd', 'vsub', 'vmul', 'vmla', 'vmls'])}.{rng.choice(I_DT)} {R(rng)}, {R(rng)}, {R(rng)}"
    if k == 1:
        return f"{rng.choice(['vadd', 'vsub', 'vmul', 'vmla', 'vmls', 'vmax', 'vmin', 'vabd'])}.f32 {R(rng)}, {R(rng)}, {R(rng)}"
    if k == 2:
        return f"{rng.choice(['vmax', 'vmin', 'vabd', 'vhadd', 'vrhadd', 'vhsub', 'vqadd', 'vqsub', 'vaba'])}.{rng.choice(S_DT)} {R(rng)}, {R(rng)}, {R(rng)}"
    if k == 3:
        return f"{rng.choice(['vand', 'vorr', 'veor', 'vbic', 'vorn', 'vbsl', 'vbit', 'vbif'])} {R(rng)}, {R(rng)}, {R(rng)}"
    if k == 4:
        return f"{rng.choice(['vceq', 'vtst'])}.{rng.choice(I_DT).replace('i', 'i' if True else '')} {R(rng)}, {R(rng)}, {R(rng)}".replace("vtst.i", "vtst.") \
            if False else f"{rng.choice(['vceq.i8', 'vceq.i16', 'vceq.i32', 'vtst.8', 'vtst.16', 'vtst.32'])} {R(rng)}, {R(rng)}, {R(rng)}"
    if k == 5:
        return f"{rng.choice(['vcge', 'vcgt'])}.{rng.choice(S_DT)} {R(rng)}, {R(rng)}, {R(rng)}"
    if k == 6:
        return f"{rng.choice(['vceq', 'vcge', 'vcgt', 'vacge', 'vacgt'])}.f32 {R(rng)}, {R(rng)}, {R(rng)}"
    if k == 7:
        return f"{rng.choice(['vceq.i32', 'vcge.s16', 'vcgt.s8', 'vcle.s32', 'vclt.s16', 'vceq.f32', 'vcge.f32', 'vclt.f32'])} {R(rng)}, {R(rng)}, #0"
    if k == 8:
        dt = rng.choice(["s8", "s16", "s32", "f32"])
        return f"{rng.choice(['vabs', 'vneg'])}.{dt} {R(rng)}, {R(rng)}"
    if k == 9:
        return f"{rng.choice(['vqabs', 'vqneg'])}.{rng.choice(['s8', 's16', 's32'])} {R(rng)}, {R(rng)}"
    if k == 10:
        sz = rng.choice([8, 16, 32])
        return f"{rng.choice(['vclz.i', 'vcls.s'])}{sz} {R(rng)}, {R(rng)}" if rng.random() < .7 else f"vcnt.8 {R(rng)}, {R(rng)}"
    if k == 11:
        dt = rng.choice(S_DT + ["s64", "u64"])
        n = int(dt[1:])
        op = rng.choice(["vshr", "vrshr", "vsra", "vrsra"])
        return f"{op}.{dt} {R(rng)}, {R(rng)}, #{rng.randint(1, n)}"
    if k == 12:
        n = rng.choice([8, 16, 32, 64])
        op = rng.choice(["vshl", "vsli", "vsri"])
        if op == "vsri":
            return f"vsri.{n} {R(rng)}, {R(rng)}, #{rng.randint(1, n)}"
        return f"{op}.{'i' if op == 'vshl' else ''}{n} {R(rng)}, {R(rng)}, #{rng.randrange(n)}"
    if k == 13:
        dt = rng.choice(S_DT)
        op = rng.choice(["vshl", "vrshl", "vqshl", "vqrshl"])
        return f"{op}.{dt} {R(rng)}, {R(rng)}, {R(rng)}"
    if k == 14:
        dt = rng.choice(S_DT)
        n = int(dt[1:])
        return f"vqshl.{dt} {R(rng)}, {R(rng)}, #{rng.randrange(n)}"
    if k == 15:
        dt = rng.choice(["i16", "i32", "i64"])
        n = int(dt[1:])
        op = rng.choice(["vshrn", "vrshrn"])
        return f"{op}.{dt} {d(rng)}, {q(rng)}, #{rng.randint(1, n // 2)}"
    if k == 16:
        dt = rng.choice(["s16", "s32", "s64", "u16", "u32", "u64"])
        n = int(dt[1:])
        op = rng.choice(["vqshrn", "vqrshrn"] + (["vqshrun", "vqrshrun"] if dt[0] == "s" else []))
        return f"{op}.{dt} {d(rng)}, {q(rng)}, #{rng.randint(1, n // 2)}"
    if k == 17:
        dt = rng.choice(["i16", "i32", "i64"])
        op = rng.choice(["vmovn", "vqmovn", "vqmovun"])
        dtt = dt if op == "vmovn" else ("s" if op == "vqmovun" else rng.choice("su")) + dt[1:]
        return f"{op}.{dtt} {d(rng)}, {q(rng)}"
    if k == 18:
        dt = rng.choice(S_DT)
        op = rng.choice(["vmovl", "vaddl", "vsubl", "vmull", "vmlal", "vmlsl", "vabdl", "vabal"])
        if op == "vmovl":
            return f"vmovl.{dt} {q(rng)}, {d(rng)}"
        return f"{op}.{dt} {q(rng)}, {d(rng)}, {d(rng)}"
    if k == 19:
        dt = rng.choice(S_DT)
        return f"{rng.choice(['vaddw', 'vsubw'])}.{dt} {q(rng)}, {q(rng)}, {d(rng)}"
    if k == 20:
        dt = rng.choice(S_DT)
        n = int(dt[1:])
        return f"vshll.{dt} {q(rng)}, {d(rng)}, #{rng.randint(1, n - 1)}"
    if k == 21:
        dt = rng.choice(["i8", "i16", "i32", "f32"])
        if dt == "f32":
            return f"{rng.choice(['vpadd', 'vpmax', 'vpmin'])}.f32 {d(rng)}, {d(rng)}, {d(rng)}"
        return f"vpadd.{dt} {d(rng)}, {d(rng)}, {d(rng)}"
    if k == 22:
        dt = rng.choice(S_DT)
        if rng.random() < 0.5:
            return f"{rng.choice(['vpmax', 'vpmin'])}.{dt} {d(rng)}, {d(rng)}, {d(rng)}"
        return f"{rng.choice(['vpaddl', 'vpadal'])}.{dt} {R(rng)}, {R(rng)}"
    if k == 23:
        sz = rng.choice([8, 16, 32])
        op = rng.choice(["vrev16", "vrev32", "vrev64", "vtrn", "vzip", "vuzp"])
        if op == "vrev16":
            sz = 8
        if op == "vrev32":
            sz = rng.choice([8, 16])
        if op in ("vzip", "vuzp") and kind == "d" and sz == 32:
            sz = 16
        a, b = R(rng), R(rng)
        if op in ("vtrn", "vzip", "vuzp"):
            while b == a:
                b = R(rng)
            return f"{op}.{sz} {a}, {b}"
        return f"{op}.{sz} {a}, {b}"
    if k == 24:
        sz = rng.choice([8, 16, 32, 64])
        lanes = (16 if kind == "q" else 8) // (sz // 8)
        return f"vext.{sz} {R(rng)}, {R(rng)}, {R(rng)}, #{rng.randrange(lanes)}"
    if k == 25:
        n = rng.randint(1, 4)
        base = rng.randrange(0, 32 - n + 1)
        lst = ", ".join(f"d{base + j}" for j in range(n))
        return f"{rng.choice(['vtbl', 'vtbx'])}.8 {d(rng)}, {{{lst}}}, {d(rng)}"
    if k == 26:
        sz = rng.choice([8, 16, 32])
        lanes = 64 // sz
        if rng.random() < 0.5:
            return f"vdup.{sz} {R(rng)}, d{rng.randrange(32)}[{rng.randrange(lanes)}]"
        return f"vdup.{sz} {R(rng)}, r{rng.randrange(8)}"
    if k == 27:
        dt = rng.choice(["i16", "i32", "f32", "s16", "s32", "u16", "u32"])
        op = rng.choice(["vmul", "vmla", "vmls"] if dt[0] in "if" else ["vmull", "vmlal", "vqdmulh", "vqrdmulh"])
        if dt in ("u16", "u32") and op in ("vqdmulh", "vqrdmulh"):
            op = "vmull"
        lanes = 2 if dt.endswith("32") else 4
        reg_lim = 16 if dt.endswith("32") else 8
        sc = f"d{rng.randrange(reg_lim)}[{rng.randrange(lanes)}]"
        if op in ("vmull", "vmlal"):
            return f"{op}.{dt} {q(rng)}, {d(rng)}, {sc}"
        return f"{op}.{dt} {R(rng)}, {R(rng)}, {sc}"
    if k == 28:
        c = rng.randrange(6)
        if c == 0:
            return f"vcvt.s32.f32 {R(rng)}, {R(rng)}"
        if c == 1:
            return f"vcvt.u32.f32 {R(rng)}, {R(rng)}"
        if c == 2:
            return f"vcvt.f32.{rng.choice(['s32', 'u32'])} {R(rng)}, {R(rng)}"
        if c == 3:
            return f"vcvt.{rng.choice(['s32', 'u32'])}.f32 {R(rng)}, {R(rng)}, #{rng.randint(1, 32)}"
        if c == 4:
            return f"{rng.choice(['vrecpe', 'vrsqrte'])}.{rng.choice(['f32', 'u32'])} {R(rng)}, {R(rng)}"
        return f"{rng.choice(['vrecps', 'vrsqrts'])}.f32 {R(rng)}, {R(rng)}, {R(rng)}"
    # loads / stores on the buffer at r11 (+r10 small index)
    sz = rng.choice([8, 16, 32])
    n = rng.randint(1, 4)
    form = rng.randrange(4)
    if form == 0:
        base = rng.randrange(0, 32 - n + 1)
        lst = ", ".join(f"d{base + j}" for j in range(n))
        return f"{rng.choice(['vld1', 'vst1'])}.{sz} {{{lst}}}, [r11]"
    if form == 1:
        m = rng.randint(2, 4)
        base = rng.randrange(0, 32 - m + 1)
        lst = ", ".join(f"d{base + j}" for j in range(m))
        return f"{rng.choice(['vld', 'vst'])}{m}.{sz} {{{lst}}}, [r11]"
    if form == 2:
        m = rng.randint(1, 4)
        base = rng.randrange(0, 32 - m + 1)
        lane = rng.randrange(64 // sz)
        lst = ", ".join(f"d{base + j}[{lane}]" for j in range(m))
        return f"{rng.choice(['vld', 'vst'])}{m}.{sz} {{{lst}}}, [r11]"
    m = rng.randint(1, 4)
    base = rng.randrange(0, 32 - m + 1)
    lst = ", ".join(f"d{base + j}[]" for j in range(m))
    return f"vld{m}.{sz} {{{lst}}}, [r11]"


def generate(seed: int, length: int = 80) -> str:
    rng = random.Random(seed)
    out = [".syntax unified", ".fpu neon", ".thumb", ".text", ".globl main", ".type main, %function",
           ".thumb_func", "main:", " push {r4-r11, lr}", " vpush {d8-d15}",
           " movw r11, #:lower16:fz_buf", " movt r11, #:upper16:fz_buf",
           " movw r0, #:lower16:fz_init", " movt r0, #:upper16:fz_init",
           " vldmia r0!, {d0-d15}", " vldmia r0, {d16-d31}"]
    for r in range(8):
        v = rng.randrange(2 ** 32)
        out += [f" movw r{r}, #{v & 0xFFFF}", f" movt r{r}, #{v >> 16}"]
    for _ in range(length):
        out.append(" " + instr(rng))
    out += [" movw ip, #:lower16:fz_dump", " movt ip, #:upper16:fz_dump", " mov r0, ip",
            " vstmia r0!, {d0-d15}", " vstmia r0, {d16-d31}",
            " movs r0, #1", " mov r1, ip", " mov r2, #256", " bl g_write",
            " movs r0, #1", " mov r1, r11", " mov r2, #256", " bl g_write",
            " movs r0, #0", " vpop {d8-d15}", " pop {r4-r11, pc}",
            ".data", ".align 4", "fz_init:"]
    for _ in range(64):  # 64 words of float specials / random bits per d-register pair
        words = [rng.choice(SPECIAL_F32) if rng.random() < 0.35 else rng.randrange(2 ** 32) for _ in range(2)]
        out.append(".word " + ", ".join(str(w) for w in words))
    out += [".align 4", "fz_buf:"] + [f".word {rng.randrange(2 ** 32)}" for _ in range(80)]
    out += [".bss", ".align 4", "fz_dump:", ".space 256"]
    return "\n".join(out) + "\n"


if __name__ == "__main__":
    sys.stdout.write(generate(int(sys.argv[1])))
