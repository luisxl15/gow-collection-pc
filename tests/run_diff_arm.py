#!/usr/bin/env python3
"""Diferencial em nível de programa do tradutor ARM: cada tests/guest/t_*.c compilado para ARMv7
(Thumb-2, NEON, hard-float) em vários níveis de otimização roda no oráculo (qemu-arm ou Unicorn)
e recompilado no host; exige stdout idêntico byte a byte e o mesmo código de saída.

usage: run_diff_arm.py [--opts -O0,-O1,-O2,-O3,-Os] [--isa thumb|arm] [-j N] [testes...]
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "fuzz_arm"))
import run_fuzz_arm as F  # noqa: E402


def case(src: Path, opt: str, isa: str):
    work = ROOT / "build" / "diff_arm" / f"{src.stem}{opt}_{isa}"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    t0 = time.perf_counter()
    if F.build_guest(work / "g.elf", src, isa, opt):
        return src.stem, opt, "falha ao compilar o guest", 0
    ref = F.run_reference(work / "g.elf")
    t1 = time.perf_counter()
    p = subprocess.run([F.PY, "-P", "-m", "vitarecomp", "recomp", "--linux", str(work / "g.elf"), "-o", str(work / "gen")],
                       capture_output=True, env={**os.environ, "PYTHONPATH": str(ROOT / "vitarecomp")})
    if p.returncode:
        return src.stem, opt, "recomp falhou: " + p.stderr.decode()[-300:], 0
    p = F.build_recompiled(work / f"rc{F.EXE}", work / "gen")
    if p.returncode:
        return src.stem, opt, "C não compilou: " + p.stderr.decode()[-300:], 0
    got = subprocess.run([str(work / f"rc{F.EXE}")], capture_output=True, timeout=300)
    if got.stdout != ref.stdout or got.returncode != ref.returncode:
        n = next((k for k, (a, b) in enumerate(zip(ref.stdout, got.stdout)) if a != b), min(len(ref.stdout), len(got.stdout)))
        return src.stem, opt, (f"DIFERENTE: rc {ref.returncode}/{got.returncode}, {len(ref.stdout)}/{len(got.stdout)} bytes, "
                               f"1º byte diferente {n}; stderr={got.stderr.decode()[:200]}"), 0
    return src.stem, opt, None, t1 - t0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--opts", default="-O0,-O1,-O2,-O3,-Os")
    ap.add_argument("--isa", default="thumb", choices=["thumb", "arm"])
    ap.add_argument("-j", type=int, default=3)
    ap.add_argument("tests", nargs="*")
    a = ap.parse_args()
    srcs = [Path(t) for t in a.tests] or sorted((ROOT / "tests" / "guest").glob("t_*.c"))
    jobs = [(s, o) for s in srcs for o in a.opts.split(",")]
    with ThreadPoolExecutor(a.j) as ex:
        res = list(ex.map(lambda so: case(so[0], so[1], a.isa), jobs))
    bad = [r for r in res if r[2]]
    for name, opt, err, _ in res:
        print(f"  {name:10} {opt:4} {'ok' if not err else err}")
    oracle = "qemu-arm" if F.HAVE_QEMU else "unicorn"
    print(f"{a.isa}: {len(res) - len(bad)}/{len(res)} casos idênticos ao {oracle}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
