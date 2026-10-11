#!/usr/bin/env bash
# Compila o vitart e roda o God of War nativo (sem emulador) por N segundos.
#   scripts/run-gow-vitart.sh <log> [segundos] [opções extras do vitart]
set -uo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
RT="${RT:-$HERE/../gow-dump/rt}"
APP="${APP:-C:/Users/Luis/Desktop/projeto_CLAUDE/_deps/vita3k-win/bin/portable/fs/ux0/app/PCSA00126}"
DATA="${DATA:-C:/Users/Luis/Desktop/projeto_CLAUDE/gow-dump/vitart-data}"
LOG="$1"; SECS="${2:-60}"; shift; [ $# -gt 0 ] && shift
CXX="${CXX:-/c/Program Files/LLVM/bin/clang++.exe}"
"$CXX" -std=c++20 -O1 -g -w -D_CRT_SECURE_NO_WARNINGS -I"$HERE/runtime/arm" -I"$HERE/runtime/vitart" \
  -I"${VULKAN_SDK:-C:/VulkanSDK/latest}/Include" -o "$RT/vitart.exe" "$HERE"/runtime/vitart/*.cpp \
  "${VULKAN_SDK:-C:/VulkanSDK/latest}/Lib/vulkan-1.lib" || exit 1
cd "$RT" && timeout "$SECS" ./vitart.exe --app "$APP" --data "$DATA" --hle-vars link.hlevars.txt --log "$LOG" "$@" \
  gow1.dll libc.dll libfios2.dll > /dev/null 2>&1
echo "rc=$?"
