#!/usr/bin/env python3
"""Differential fuzzing: qemu-mipsel vs recompiled code over random programs.

usage: run_fuzz.py [--start N] [--count N] [--len N] [--arm] [-j N]
On mismatch, prints which architectural state words differ and keeps the case
under build/fuzz/<seed>/ for reproduction.
"""
from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).parent))
from gen_fuzz import generate  # noqa: E402

RT = ROOT / "runtime"
FLAGS = ["-O1", "-fno-thread-jumps", "-fwrapv", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-math-errno", "-w",
         f"-I{RT}"]
ARM_CC = ["arm-linux-gnueabihf-gcc", "-static", "-mthumb", "-march=armv7-a+simd",
          "-mtune=cortex-a9", "-mfloat-abi=hard"]


def names(i: int) -> str:
    if i < 32:
        return f"${i}"
    return {32: "hi", 33: "lo", 34: "fcr31", 35: "pad"}.get(i, f"$f{i - 36}" if i < 68 else f"buf+{(i - 68) * 4}")


FCSR_WORD = 34
FCSR_FLAG_BITS = 0x0003F07C  # cause + sticky flags: not modelled (docs/fidelity.md)


def normalize(out: bytes) -> bytes:
    if len(out) < (FCSR_WORD + 1) * 4:
        return out
    words = list(struct.unpack(f"<{len(out) // 4}I", out))
    words[FCSR_WORD] &= ~FCSR_FLAG_BITS
    return struct.pack(f"<{len(words)}I", *words)


def explain(ref: bytes, got: bytes) -> str:
    if len(ref) != len(got):
        return f"length {len(got)} != reference {len(ref)}"
    a = struct.unpack(f"<{len(ref) // 4}I", ref)
    b = struct.unpack(f"<{len(got) // 4}I", got)
    diffs = [f"{names(i)}: ref={x:08x} got={y:08x}" for i, (x, y) in enumerate(zip(a, b)) if x != y]
    return "; ".join(diffs[:8]) + (f" (+{len(diffs) - 8} more)" if len(diffs) > 8 else "")


def run(seed: int, length: int, arm: bool) -> tuple[int, str | None]:
    work = ROOT / "build" / "fuzz" / str(seed)
    work.mkdir(parents=True, exist_ok=True)
    asm = work / "fuzz.S"
    asm.write_text(generate(seed, length))
    elf = work / "fuzz.elf"
    p = subprocess.run([str(ROOT / "scripts/build-guest.sh"), str(elf), str(asm)],
                       capture_output=True)
    if p.returncode:
        return seed, "assemble failed: " + p.stderr.decode()[-500:]
    ref = subprocess.run(["qemu-mipsel", str(elf)], capture_output=True, timeout=30)
    gen = work / "gen"
    p = subprocess.run([sys.executable, "-m", "mipsrecomp", str(elf), "-o", str(gen)],
                       capture_output=True, env={"PYTHONPATH": str(ROOT / "recomp")})
    if p.returncode:
        return seed, "recompile failed: " + p.stderr.decode()[-500:]
    srcs = sorted(str(s) for s in gen.glob("*.c")) + [str(RT / "recomp_runtime.c"),
                                                      str(RT / "host/platform_host.c")]
    targets = [("x86_64", ["gcc"], [])] + ([("armv7", ARM_CC, ["qemu-arm"])] if arm else [])
    for name, cc, runner in targets:
        exe = work / f"fuzz.{name}"
        p = subprocess.run(cc + FLAGS + ["-o", str(exe), *srcs], capture_output=True)
        if p.returncode:
            return seed, f"{name} build failed: " + p.stderr.decode()[-500:]
        got = subprocess.run(runner + [str(exe)], capture_output=True, timeout=30)
        a, b = normalize(ref.stdout), normalize(got.stdout)
        if a != b or got.returncode != ref.returncode:
            return seed, f"{name}: exit {got.returncode} vs {ref.returncode}; " + explain(a, b) + (" stderr=" + got.stderr.decode()[:200] if got.stderr else "")
    shutil.rmtree(work)
    return seed, None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", type=int, default=1)
    ap.add_argument("--count", type=int, default=200)
    ap.add_argument("--len", type=int, default=160)
    ap.add_argument("--arm", action="store_true", help="also run the ARMv7 build under qemu-arm")
    ap.add_argument("-j", type=int, default=4)
    args = ap.parse_args()
    seeds = range(args.start, args.start + args.count)
    with ThreadPoolExecutor(args.j) as ex:
        results = list(ex.map(lambda s: run(s, args.len, args.arm), seeds))
    fails = [(s, e) for s, e in results if e]
    for s, e in fails[:25]:
        print(f"seed {s}: {e}")
    print(f"{len(results) - len(fails)}/{len(results)} fuzz programs matched the reference")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
