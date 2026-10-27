#!/usr/bin/env bash
# Do PKG ao jogo rodando no PC: instala e decifra o jogo, recompila os módulos para DLL, compila o vitart, gera os
# shaders e o menu de opções do PC. Passo a passo e requisitos em docs/BUILD.md.
#
#   scripts/build-all.sh <gowHD_PSV.pkg> <work.bin> <pasta do Vita3K.exe> <saída>
#
# Variáveis: VITA3K_SRC (fonte do Vita3K, só na 1ª vez, para importar as chaves de SELF), JOBS (compilações em
# paralelo, padrão 4), VULKAN_SDK (padrão C:/VulkanSDK/latest).
set -euo pipefail
[ $# -eq 4 ] || { sed -n 2,9p "$0"; exit 1; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PKG="$1"; RIF="$2"; V3K="$(cd "$3" && pwd)"; OUT="$(mkdir -p "$4" && cd "$4" && pwd)"
export PYTHONPATH="$HERE/vitarecomp" PYTHONIOENCODING=utf-8
VK="${VULKAN_SDK:-C:/VulkanSDK/latest}"
CXX="${CXX:-clang++}"; command -v "$CXX" >/dev/null || CXX="/c/Program Files/LLVM/bin/clang++.exe"
CC="${CC:-clang}"; command -v "$CC" >/dev/null || CC="/c/Program Files/LLVM/bin/clang.exe"
APP="$V3K/portable/fs/ux0/app/PCSA00126"
RT="$OUT/rt"; DATA="$OUT/vitart-data"
mkdir -p "$OUT/elf" "$RT" "$DATA"

echo "== 1/7 chaves de SELF"
if [ ! -f "${VITARECOMP_KEYS:-$HOME/.vitarecomp/keys.json}" ]; then
  [ -n "${VITA3K_SRC:-}" ] || { echo "falta VITA3K_SRC (fonte do Vita3K) para importar as chaves"; exit 1; }
  python -P -m vitarecomp keys-import "$VITA3K_SRC/vita3k/packages/src/sce_utils.cpp"
fi

echo "== 2/7 instalar o PKG no Vita3K (decifra o PFS com a licença)"
if [ ! -f "$APP/eboot.bin" ]; then
  ZRIF="$(python -P "$HERE/tools/zrif.py" encode "$RIF")"
  "$V3K/Vita3K.exe" --pkg "$PKG" --zrif "$ZRIF"
fi
[ -f "$APP/eboot.bin" ] || { echo "o Vita3K não instalou em $APP (o Vita3K precisa estar em modo portátil)"; exit 1; }

echo "== 3/7 SELF -> ELF"
for f in eboot.bin gow1.self gow2.self sce_module/libc.suprx sce_module/libfios2.suprx; do
  n="$(basename "$f")"; n="${n%.*}"
  [ -f "$OUT/elf/$n.elf" ] || python -P -m vitarecomp decrypt "$APP/$f" --rif "$RIF" -o "$OUT/elf/$n.elf"
done

echo "== 4/7 ARM -> C -> DLL"
for p in gow1 gow2 eboot; do cp "$HERE/docs/$p-plan.json" "$OUT/"; done
JOBS="${JOBS:-4}" "$HERE/scripts/build-gow-dlls.sh" "$OUT/gow1-plan.json" "$OUT/dll" gow1 libc libfios2
JOBS="${JOBS:-4}" "$HERE/scripts/build-gow-dlls.sh" "$OUT/gow2-plan.json" "$OUT/dll" gow2
JOBS="${JOBS:-4}" "$HERE/scripts/build-gow-dlls.sh" "$OUT/eboot-plan.json" "$OUT/dll" eboot
cp "$OUT"/dll/*.dll "$RT/"
cp "$OUT/dll/link.hlevars.txt" "$RT/link.hlevars.txt"

echo "== 5/7 vitart.exe"
AT9OBJ="$OUT/obj/libatrac9"; mkdir -p "$AT9OBJ"
for c in "$HERE"/third_party/libatrac9/src/*.c; do
  "$CC" -std=c99 -O2 -w -D_CRT_SECURE_NO_WARNINGS -c "$c" -o "$AT9OBJ/$(basename "${c%.c}").o"
done
"$CXX" -std=c++20 -O2 -w -D_CRT_SECURE_NO_WARNINGS -I"$HERE/runtime/arm" -I"$HERE/runtime/vitart" \
  -I"$HERE/third_party/libatrac9/src" -I"$HERE/third_party/stb" -I"$VK/Include" -o "$RT/vitart.exe" \
  "$HERE"/runtime/vitart/*.cpp "$HERE"/games/gow/*.cpp "$AT9OBJ"/*.o "$VK/Lib/vulkan-1.lib" \
  -Xlinker /DELAYLOAD:vulkan-1.dll -ldelayimp

echo "== 6/7 shaders (Vulkan e Direct3D 11)"
mkdir -p "$RT/spv/miss"
cp -n "$HERE"/spv/miss/*.gxp "$RT/spv/miss/" 2>/dev/null || true
python -P "$HERE/tools/usse_build_cache.py" "$RT/spv" "$RT/spv/miss" --d3d11 --jobs "${JOBS:-4}"

echo "== 7/7 menu de opções do PC (Options do menu de pausa)"
mkdir -p "$DATA/mods"
for g in gow1 gow2; do
  python -P "$HERE/tools/gow_menu_mod.py" "$APP/exec/${g}_psp2_SCEA.psarc" "$DATA/mods/${g}_psp2_SCEA.psarc.vtpatch"
done

cat > "$RT/God of War Collection.cmd" <<EOF
@echo off
cd /d "%~dp0"
set VITART_VULKAN=1
set VITART_SPV_DIR=spv
start "" vitart.exe --app "$(cygpath -w "$APP" 2>/dev/null || echo "$APP")" --data "$(cygpath -w "$DATA" 2>/dev/null || echo "$DATA")" --hle-vars link.hlevars.txt --log vitart.log eboot.dll libc.dll libfios2.dll
EOF
echo "pronto: $RT/God of War Collection.cmd"
