#!/usr/bin/env bash
# Guest MIPS ELF -> recompiled C -> PS Vita eboot.bin + .vpk
# usage: build-vita-port.sh <guest.elf> <name> <TITLEID> [outdir]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
elf="$1" name="$2" titleid="$3" out="${4:-$ROOT/build/vita/$name}"
mkdir -p "$out"
PYTHONPATH="$ROOT/recomp" python3 -m mipsrecomp "$elf" -o "$out/gen"
mapfile -t gen < <(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print("\n".join(sys.argv[2]+"/"+s for s in m["sources"]))' "$out/gen/manifest.json" "$out/gen")
"$ROOT/scripts/vita-build.sh" "$name" "$titleid" "$out" "${gen[@]}" \
  "$ROOT/runtime/recomp_runtime.c" "$ROOT/runtime/vita/platform_vita.c" \
  -- -I"$out/gen" -fno-thread-jumps -fwrapv -fno-strict-aliasing -ffp-contract=off -fno-math-errno -Wno-unused-label \
     -Wno-unused-variable -Wno-unused-but-set-variable
