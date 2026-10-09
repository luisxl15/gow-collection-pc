#!/usr/bin/env bash
# Build a freestanding PS Vita eboot + VPK from C sources.
#
# usage: vita-build.sh <name> <TITLEID> <outdir> <src.c>... [-- extra cflags]
#
# Pipeline: arm-none-eabi-gcc (Thumb-2, ARMv7-A, NEON, hard-float, -Wl,-q)
#   -> vita-elf-create (ELF -> Vita ELF with SCE import tables)
#   -> vita-make-fself (fake-signed SELF, eboot.bin)
#   -> vita-mksfoex (param.sfo) -> vita-pack-vpk (installable .vpk)
set -euo pipefail

VITASDK="${VITASDK:-/home/user/deps/vitasdk}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
name="$1" titleid="$2" out="$3"
shift 3
srcs=() extra=()
while (($#)); do
  if [[ "$1" == "--" ]]; then shift; extra=("$@"); break; fi
  srcs+=("$1"); shift
done
[[ "$titleid" =~ ^[A-Z]{4}[0-9]{5}$ ]] || { echo "TITLEID must be 4 letters + 5 digits" >&2; exit 2; }

mkdir -p "$out"
CC=arm-none-eabi-gcc
ARCH_FLAGS=(-mthumb -march=armv7-a+simd -mtune=cortex-a9 -mfloat-abi=hard)
CFLAGS=("${ARCH_FLAGS[@]}" -O2 -ffreestanding -fno-builtin -fno-common -ffunction-sections
        -Wall -Wno-unused-function -I"$VITASDK/arm-vita-eabi/include" -I"$ROOT/runtime/vita"
        -I"$ROOT/runtime")
STUBS=(-lSceLibKernel_stub -lSceSysmem_stub -lSceDisplay_stub -lSceDriverUser_stub
       -lSceCtrl_stub -lSceKernelThreadMgr_stub -lSceIofilemgr_stub -lSceProcessmgr_stub)
# Note: SceRtabi is deliberately not linked: its __aeabi_* exports would shadow
# libgcc's soft helpers (e.g. __aeabi_d2lz) and Vita3K does not implement them.

"$CC" "${CFLAGS[@]}" "${extra[@]}" -nostartfiles -nostdlib -Wl,-q -Wl,-e,_start \
  -Wl,--gc-sections -o "$out/$name.elf" \
  "$ROOT/runtime/vita/vita_crt0.c" "$ROOT/runtime/vita/vita_console.c" "${srcs[@]}" \
  -L"$VITASDK/arm-vita-eabi/lib" -Wl,--start-group "${STUBS[@]}" -lgcc -Wl,--end-group

"$VITASDK/bin/vita-elf-create" "$out/$name.elf" "$out/$name.velf" >/dev/null
"$VITASDK/bin/vita-make-fself" -c "$out/$name.velf" "$out/eboot.bin" >/dev/null
"$VITASDK/bin/vita-mksfoex" -s TITLE_ID="$titleid" "$name" "$out/param.sfo"
rm -f "$out/$name.vpk"
"$VITASDK/bin/vita-pack-vpk" -s "$out/param.sfo" -b "$out/eboot.bin" "$out/$name.vpk" >/dev/null
echo "built $out/$name.vpk"
