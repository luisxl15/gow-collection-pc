#!/usr/bin/env python3
"""Differential fuzzing of the ARM translator: qemu-arm vs vitarecomp output (x86-64).

usage: run_fuzz_arm.py [--start N] [--count N] [--isa thumb|arm] [-j N]
Programs the assembler rejects (random operand combos) are skipped and counted.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from gen_fuzz_arm import generate  # noqa: E402
from gen_fuzz_neon import generate as generate_neon  # noqa: E402

PY = "/home/user/deps/rea-py/bin/python"
RT = ROOT / "runtime/arm"
APSR_MASK = 0xF0000000           # Q/GE not modelled yet
FPSCR_MASK = 0xF7C00000          # NZCV + mode bits; cumulative exception flags not modelled


NEON = False


def normalize(b: bytes) -> bytes:
    if NEON or len(b) < 56:
        return b
    w = list(struct.unpack(f"<{len(b) // 4}I", b))
    w[12] &= APSR_MASK
    w[13] &= FPSCR_MASK
    return struct.pack(f"<{len(w)}I", *w)


def label(k: int) -> str:
    if NEON:
        return f"d{k // 2}.{'lo' if k % 2 == 0 else 'hi'}" if k < 64 else f"buf+{(k - 64) * 4}"
    if k < 11:
        return f"r{k}"
    return {11: "r11", 12: "APSR", 13: "FPSCR"}.get(k, f"d{(k - 14) // 2}.{'lo' if (k - 14) % 2 == 0 else 'hi'}"
                                                    if k < 46 else f"buf+{(k - 46) * 4}")


def run(seed: int, isa: str):
    work = ROOT / "build" / "fuzz_arm" / f"{isa}{seed}"
    work.mkdir(parents=True, exist_ok=True)
    (work / "f.S").write_text(generate_neon(seed) if isa == "neon" else generate(seed, isa == "thumb"))
    env = {**os.environ, "GUEST_ISA": "arm" if isa == "arm" else "thumb"}
    p = subprocess.run([str(ROOT / "scripts/build-guest-arm.sh"), str(work / "f.elf"), str(work / "f.S")],
                       capture_output=True, env=env)
    if p.returncode:
        shutil.rmtree(work)
        return seed, "skip"
    ref = subprocess.run(["qemu-arm", str(work / "f.elf")], capture_output=True, timeout=30)
    if ref.returncode != 0:  # the random program itself faulted on the reference
        shutil.rmtree(work)
        return seed, "skip"
    p = subprocess.run([PY, "-m", "vitarecomp", "recomp", "--linux", str(work / "f.elf"), "-o", str(work / "gen")],
                       capture_output=True, env={**os.environ, "PYTHONPATH": str(ROOT / "vitarecomp")})
    if p.returncode:
        return seed, "recomp failed: " + p.stderr.decode()[-300:]
    p = subprocess.run(["gcc", "-O1", "-w", "-fwrapv", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-math-errno",
                        f"-I{RT}", "-o", str(work / "rc"), *map(str, (work / "gen").glob("*.c")),
                        str(RT / "arc_runtime.c"), str(RT / "arc_linux.c")], capture_output=True)
    if p.returncode:
        return seed, "C build failed: " + p.stderr.decode()[-300:]
    got = subprocess.run([str(work / "rc")], capture_output=True, timeout=30)
    a, b = normalize(ref.stdout), normalize(got.stdout)
    if a != b or ref.returncode != got.returncode:
        if len(a) == len(b):
            wa = struct.unpack(f"<{len(a) // 4}I", a)
            wb = struct.unpack(f"<{len(b) // 4}I", b)
            diffs = [f"{label(k)}: ref={x:08x} got={y:08x}" for k, (x, y) in enumerate(zip(wa, wb)) if x != y]
        else:
            diffs = [f"len {len(b)} vs {len(a)}; stderr={got.stderr.decode()[:160]}"]
        return seed, "; ".join(diffs[:6])
    shutil.rmtree(work)
    return seed, None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--count", type=int, default=100)
    ap.add_argument("--isa", default="thumb", choices=["thumb", "arm", "neon"])
    ap.add_argument("-j", type=int, default=4)
    args = ap.parse_args()
    global NEON
    NEON = args.isa == "neon"
    with ThreadPoolExecutor(args.j) as ex:
        res = list(ex.map(lambda s: run(s, args.isa), range(args.start, args.start + args.count)))
    skipped = sum(r == "skip" for _, r in res)
    fails = [(s, r) for s, r in res if r and r != "skip"]
    for s, r in fails[:20]:
        print(f"seed {s}: {r}")
    ran = len(res) - skipped
    print(f"{args.isa}: {ran - len(fails)}/{ran} programs matched qemu-arm ({skipped} skipped: rejected by assembler or faulting on qemu)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
