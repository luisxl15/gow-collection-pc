#!/usr/bin/env bash
# Build a freestanding MIPS32 (little-endian, o32, hard-float, non-PIC) guest ELF.
# usage: build-guest.sh <out.elf> <src.c>... [-- extra cflags]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
out="$1"; shift
srcs=() extra=()
while (($#)); do
  if [[ "$1" == "--" ]]; then shift; extra=("$@"); break; fi
  srcs+=("$1"); shift
done
CC=mipsel-linux-gnu-gcc
OPT="${GUEST_OPT:--O2}"
mkdir -p "$(dirname "$out")"
"$CC" -march=mips32r2 -mabi=32 -mhard-float -mfp32 -mno-abicalls -fno-pic -G0 \
  -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns -fno-math-errno \
  -nostdlib -static -Wall -Wno-unused-function "$OPT" "${extra[@]}" \
  -Wl,-T,"$ROOT/guest/guest.ld" -Wl,--build-id=none -o "$out" \
  "$ROOT/guest/crt0.S" "$ROOT/guest/glibc.c" "${srcs[@]}" -lgcc
