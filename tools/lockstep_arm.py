#!/usr/bin/env python3
"""Lockstep divergence finder for ARM: qemu-arm single-step log vs vitarecomp trace.

usage: lockstep_arm.py <guest.elf>
Both sides start from identical registers (including sp); reports the first
instruction after which r0-r14, NZCV, FPSCR or D0-D31 differ.
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PY = "/home/user/deps/rea-py/bin/python"


def parse_qemu(text: str):
    states, cur = [], None
    for line in text.splitlines():
        if line.startswith("R00="):
            if cur:
                states.append(cur)
            cur = {"r": {}, "d": [0] * 32}
        if cur is None:
            continue
        for m in re.finditer(r"R(\d\d)=([0-9a-f]{8})", line):
            cur["r"][int(m[1])] = int(m[2], 16)
        if line.startswith("PSR="):
            cur["nzcv"] = int(line[4:12], 16) & 0xF0000000
        for m in re.finditer(r"d(\d\d)=([0-9a-f]{16})", line):
            cur["d"][int(m[1])] = int(m[2], 16)
        if line.startswith("FPSCR:"):
            cur["fpscr"] = int(line.split()[1], 16)
    if cur:
        states.append(cur)
    out = []
    for s in states:
        out.append({"pc": s["r"][15], "r": [s["r"][k] for k in range(15)], "nzcv": s["nzcv"],
                    "fpscr": s.get("fpscr", 0), "d": s["d"]})
    return out


def parse_rc(text: str):
    out = []
    for line in text.splitlines():
        if line.startswith("T "):
            v = line.split()[1:]
            w = [int(x, 16) for x in v[:18]]
            out.append({"pc": w[0] & ~1, "r": w[1:16], "nzcv": w[16], "fpscr": w[17],
                        "d": [int(x, 16) for x in v[18:50]]})
    return out


def main() -> int:
    elf = Path(sys.argv[1]).resolve()
    tmp = Path(tempfile.mkdtemp(prefix="lockstep-arm-"))
    subprocess.run(["qemu-arm", "-one-insn-per-tb", "-d", "cpu,fpu,nochain", "-D", str(tmp / "q.log"), str(elf)],
                   capture_output=True)
    qs = parse_qemu((tmp / "q.log").read_text())
    gen = tmp / "gen"
    subprocess.run([PY, "-m", "vitarecomp", "recomp", "--linux", "--trace-insn", str(elf), "-o", str(gen)],
                   env={**os.environ, "PYTHONPATH": str(ROOT / "vitarecomp")}, check=True, capture_output=True)
    exe = tmp / "rc"
    subprocess.run(["gcc", "-O1", "-w", "-fwrapv", "-fno-strict-aliasing", "-ffp-contract=off", "-fno-math-errno",
                    f"-I{ROOT / 'runtime/arm'}", "-o", str(exe), *map(str, gen.glob("*.c")),
                    str(ROOT / "runtime/arm/arc_runtime.c"), str(ROOT / "runtime/arm/arc_linux.c")], check=True)
    init = " ".join(f"{v:x}" for v in qs[0]["r"])
    r = subprocess.run([str(exe)], capture_output=True, env={**os.environ, "ARC_INIT": init})
    rs = parse_rc(r.stderr.decode())
    print(f"qemu states: {len(qs)}  recompiled states: {len(rs)}")
    for n, (a, b) in enumerate(zip(qs, rs)):
        diffs = []
        if a["pc"] != b["pc"]:
            diffs.append(f"pc: qemu={a['pc']:08x} rc={b['pc']:08x}")
        for k in range(15):
            if a["r"][k] != b["r"][k]:
                diffs.append(f"r{k}: qemu={a['r'][k]:08x} rc={b['r'][k]:08x}")
        if a["nzcv"] != b["nzcv"]:
            diffs.append(f"NZCV: qemu={a['nzcv'] >> 28:04b} rc={b['nzcv'] >> 28:04b}")
        if (a["fpscr"] ^ b["fpscr"]) & 0xF0000000:
            diffs.append(f"FPSCR.NZCV: qemu={a['fpscr']:08x} rc={b['fpscr']:08x}")
        for k in range(32):
            if a["d"][k] != b["d"][k]:
                diffs.append(f"d{k}: qemu={a['d'][k]:016x} rc={b['d'][k]:016x}")
        if diffs:
            prev = qs[n - 1]["pc"] if n else None
            print(f"first divergence at step {n} (before pc={a['pc']:#x}; previous insn at {prev:#x})")
            for d in diffs[:12]:
                print("   ", d)
            dis = subprocess.run(["arm-linux-gnueabihf-objdump", "-d", str(elf)], capture_output=True, text=True).stdout
            for line in dis.splitlines():
                if prev is not None and line.strip().startswith(f"{prev:x}:"):
                    print("    insn:", line.strip())
            return 1
    print("no divergence in common prefix" + ("" if len(qs) == len(rs) else " (lengths differ)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
