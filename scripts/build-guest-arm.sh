#!/usr/bin/env bash
# Build a freestanding ARMv7 guest with the PS Vita CPU ABI (Cortex-A9, Thumb-2,
# NEON, hard-float) as a static Linux ELF, so qemu-arm can serve as the reference.
# usage: build-guest-arm.sh <out.elf> <src>... [-- extra cflags]   (GUEST_OPT, GUEST_ISA=thumb|arm)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
out="$1"; shift
srcs=() extra=()
while (($#)); do
  if [[ "$1" == "--" ]]; then shift; extra=("$@"); break; fi
  srcs+=("$1"); shift
done
isa="-m${GUEST_ISA:-thumb}"
mkdir -p "$(dirname "$out")"
arm-linux-gnueabihf-gcc $isa -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard \
  -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns \
  -fno-math-errno -fno-pic -no-pie -nostdlib -static -Wall -Wno-unused-function \
  "${GUEST_OPT:--O2}" "${extra[@]}" -Wl,-T,"$ROOT/guest/guest_arm.ld" -Wl,--build-id=none -Wl,-z,noexecstack \
  -o "$out" "$ROOT/guest/crt0_arm.S" "$ROOT/guest/glibc.c" "${srcs[@]}" -lgcc
