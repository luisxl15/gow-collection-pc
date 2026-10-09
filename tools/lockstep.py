#!/usr/bin/env python3
"""Lockstep divergence finder: qemu-mipsel single-step CPU log vs recompiled trace.

usage: lockstep.py <guest.elf> [--keep]
Recompiles the guest with --trace-insn, runs both, and reports the first
instruction after which architectural state differs ($sp and FCSR flag bits
are ignored because stack placement and IEEE flags are not modelled).
"""
from __future__ import annotations

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / "runtime"
GPR_RE = re.compile(r"GPR\d\d: (.*)")
FPR_RE = re.compile(r"\s*f(\d+): w:([0-9a-f]{8}) d:([0-9a-f]{16})")
FCSR_FLAGS = 0x0003F07C
SP = 29


def parse_qemu(log: str):
    states, cur = [], None
    for line in log.splitlines():
        if line.startswith("pc="):
            if cur:
                states.append(cur)
            m = re.match(r"pc=0x([0-9a-f]+) HI=0x([0-9a-f]+) LO=0x([0-9a-f]+)", line)
            cur = {"pc": int(m[1], 16), "hi": int(m[2], 16), "lo": int(m[3], 16), "r": [], "f": [0] * 32}
        elif cur is not None and (m := GPR_RE.match(line)):
            toks = m[1].split()
            cur["r"] += [int(v, 16) for v in toks[1::2]]
        elif cur is not None and "FCR31" in line:
            cur["fcr31"] = int(re.search(r"FCR31 0x([0-9a-f]+)", line)[1], 16)
        elif cur is not None and (m := FPR_RE.match(line)):
            n, d = int(m[1]), int(m[3], 16)
            cur["f"][n], cur["f"][n + 1] = d & 0xFFFFFFFF, d >> 32
    if cur:
        states.append(cur)
    return states


def parse_rc(log: str):
    states = []
    for line in log.splitlines():
        if not line.startswith("T "):
            continue
        v = [int(x, 16) for x in line.split()[1:]]
        states.append({"pc": v[0], "hi": v[1], "lo": v[2], "r": [0] + v[3:34], "fcr31": v[34],
                       "f": v[35:67]})
    return states


def diff(a, b):
    out = []
    for k in ("pc", "hi", "lo"):
        if a[k] != b[k]:
            out.append(f"{k}: qemu={a[k]:08x} rc={b[k]:08x}")
    if (a["fcr31"] ^ b["fcr31"]) & ~FCSR_FLAGS:
        out.append(f"fcr31: qemu={a['fcr31']:08x} rc={b['fcr31']:08x}")
    for i in range(32):
        if i != SP and a["r"][i] != b["r"][i]:
            out.append(f"${i}: qemu={a['r'][i]:08x} rc={b['r'][i]:08x}")
        if a["f"][i] != b["f"][i]:
            out.append(f"$f{i}: qemu={a['f'][i]:08x} rc={b['f'][i]:08x}")
    return out


def main() -> int:
    elf = Path(sys.argv[1]).resolve()
    tmp = Path(tempfile.mkdtemp(prefix="lockstep-"))
    q = subprocess.run(["qemu-mipsel", "-one-insn-per-tb", "-d", "cpu,fpu,nochain", "-D",
                        str(tmp / "qemu.log"), str(elf)], capture_output=True)
    gen = tmp / "gen"
    subprocess.run([sys.executable, "-m", "mipsrecomp", str(elf), "-o", str(gen), "--trace-insn"],
                   check=True, env={"PYTHONPATH": str(ROOT / "recomp")}, capture_output=True)
    exe = tmp / "rc"
    subprocess.run(["gcc", "-O1", "-w", "-fno-strict-aliasing", "-ffp-contract=off",
                    "-fno-math-errno", f"-I{RT}", "-o", str(exe), *map(str, gen.glob("*.c")),
                    str(RT / "recomp_runtime.c"), str(RT / "host/platform_host.c")], check=True)
    r = subprocess.run([str(exe)], capture_output=True)
    qs = parse_qemu((tmp / "qemu.log").read_text())
    rs = parse_rc(r.stderr.decode())
    print(f"qemu states: {len(qs)}  recompiled states: {len(rs)}")
    # QEMU logs a step for an annulled branch-likely delay slot; the recompiled
    # code never visits it. Drop such qemu-only states (pc mismatch, next matches).
    aligned, j = [], 0
    for b in rs:
        while j + 1 < len(qs) and qs[j]["pc"] != b["pc"] and qs[j + 1]["pc"] == b["pc"]:
            j += 1
        if j >= len(qs):
            break
        aligned.append(qs[j])
        j += 1
    qs = aligned
    for n, (a, b) in enumerate(zip(qs, rs)):
        d = diff(a, b)
        if d:
            prev = qs[n - 1]["pc"] if n else None
            print(f"first divergence at step {n}: before pc={a['pc']:#010x}"
                  + (f" (after executing the instruction(s) at {prev:#010x})" if prev is not None else ""))
            for line in d:
                print("   ", line)
            dis = subprocess.run(["mipsel-linux-gnu-objdump", "-d", str(elf)], capture_output=True,
                                 text=True).stdout
            if prev is not None:
                for line in dis.splitlines():
                    if line.strip().startswith(f"{prev:x}:") or line.strip().startswith(f"{prev + 4:x}:"):
                        print("    insn:", line.strip())
            return 1
    print("no divergence in common prefix" + ("" if len(qs) == len(rs) else " (lengths differ)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
