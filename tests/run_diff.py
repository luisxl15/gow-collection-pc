#!/usr/bin/env python3
"""Differential tests: reference (qemu-mipsel) vs recompiled (x86-64 host, ARMv7 under qemu-arm).

For every guest test and optimisation level:
  1. build the MIPS guest ELF
  2. run it under qemu-mipsel  -> reference stdout + exit code
  3. recompile with mipsrecomp -> C
  4. build + run on the host, and build for ARMv7-A (Cortex-A9 flags, Thumb-2,
     NEON, hard-float: the PS Vita CPU ABI) + run under qemu-arm
  5. require byte-identical stdout and identical exit codes
Writes build/diff/report.json with timings.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / "runtime"
GEN_FLAGS = ["-fno-thread-jumps", "-fwrapv", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-math-errno", "-w"]
ARM_CC = ["arm-linux-gnueabihf-gcc", "-static", "-mthumb", "-march=armv7-a+simd",
          "-mtune=cortex-a9", "-mfloat-abi=hard"]


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, **kw)


def timed(cmd, timeout=120):
    t = time.perf_counter()
    p = sh(cmd, timeout=timeout)
    return p, time.perf_counter() - t


def run_case(src: Path, opt: str, archs: list[str]) -> dict:
    name = f"{src.stem}{opt}"
    work = ROOT / "build" / "diff" / name
    work.mkdir(parents=True, exist_ok=True)
    elf = work / f"{name}.elf"
    res = {"case": name, "ok": False}

    p = sh([str(ROOT / "scripts/build-guest.sh"), str(elf), str(src)], env={
        "GUEST_OPT": opt, "PATH": "/usr/bin:/bin"})
    if p.returncode:
        res["error"] = "guest build failed: " + p.stderr.decode()[-2000:]
        return res

    ref, t_ref = timed(["qemu-mipsel", str(elf)])
    res["reference"] = {"exit": ref.returncode, "seconds": round(t_ref, 4)}

    gen = work / "gen"
    p = sh([sys.executable, "-m", "mipsrecomp", str(elf), "-o", str(gen)],
           env={"PYTHONPATH": str(ROOT / "recomp"), "PATH": "/usr/bin:/bin"})
    if p.returncode:
        res["error"] = "recompile failed: " + p.stderr.decode()[-2000:]
        return res
    manifest = json.loads((gen / "manifest.json").read_text())
    res["functions"] = len(manifest["functions"])
    res["instructions"] = manifest["instruction_count"]
    srcs = [str(gen / s) for s in manifest["sources"]] + [str(RT / "recomp_runtime.c"),
                                                          str(RT / "host/platform_host.c")]
    ok = True
    for arch in archs:
        exe = work / f"{name}.{arch}"
        cc = ["gcc"] if arch == "x86_64" else ARM_CC
        p = sh(cc + ["-O2", *GEN_FLAGS, f"-I{RT}", "-o", str(exe), *srcs])
        if p.returncode:
            res[arch] = {"error": "host build failed: " + p.stderr.decode()[-2000:]}
            ok = False
            continue
        cmd = [str(exe)] if arch == "x86_64" else ["qemu-arm", str(exe)]
        out, t = timed(cmd)
        same = out.stdout == ref.stdout and out.returncode == ref.returncode
        res[arch] = {"exit": out.returncode, "seconds": round(t, 4), "match": same}
        if not same:
            ok = False
            (work / f"{arch}.stdout").write_bytes(out.stdout)
            (work / f"{arch}.stderr").write_bytes(out.stderr)
            (work / "reference.stdout").write_bytes(ref.stdout)
    res["ok"] = ok
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tests", nargs="*", help="guest test sources (default: tests/guest/*.c)")
    ap.add_argument("--opts", default="-O0,-O1,-O2,-O3,-Os")
    ap.add_argument("--archs", default="x86_64,armv7")
    ap.add_argument("-j", type=int, default=4)
    args = ap.parse_args()
    tests = [Path(t) for t in args.tests] or sorted((ROOT / "tests/guest").glob("*.c"))
    cases = [(t, o) for t in tests for o in args.opts.split(",")]
    archs = args.archs.split(",")
    with ThreadPoolExecutor(args.j) as ex:
        results = list(ex.map(lambda c: run_case(c[0], c[1], archs), cases))
    for r in results:
        detail = " ".join(f"{a}={'ok' if r.get(a, {}).get('match') else 'FAIL'}" for a in archs)
        status = "PASS" if r["ok"] else "FAIL"
        print(f"{status} {r['case']:<20} fns={r.get('functions', '-'):<4} insns={r.get('instructions', '-'):<6} {detail}")
        if "error" in r:
            print("   ", r["error"].strip().splitlines()[-1] if r["error"].strip() else r["error"])
    out = ROOT / "build" / "diff" / "report.json"
    out.write_text(json.dumps(results, indent=2) + "\n")
    failed = sum(not r["ok"] for r in results)
    print(f"\n{len(results) - failed}/{len(results)} cases passed; report: {out.relative_to(ROOT)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
