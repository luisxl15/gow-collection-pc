#!/usr/bin/env bash
# Plano de link -> uma DLL por módulo do Vita, para o runtime próprio (vitart).
#   scripts/build-gow-dlls.sh <plano.json> <saída> [módulos...]
# Cada módulo é traduzido na base do plano, com as variáveis importadas já ligadas.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
PLAN="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
OUT="$(mkdir -p "$2" && cd "$2" && pwd)"; shift 2
CC="${CC:-clang}"
command -v "$CC" >/dev/null || CC="/c/Program Files/LLVM/bin/clang.exe"
JOBS="${JOBS:-4}"
export PYTHONPATH="$HERE/vitarecomp" PYTHONIOENCODING=utf-8 CC
CFLAGS="-c -O1 -w -fwrapv -fno-strict-aliasing -ffp-contract=off -fno-math-errno -I $HERE/runtime/arm"
export CFLAGS

python -P -m vitarecomp link "$PLAN" -o "$OUT/link.json"
MODS=("$@")
if [ ${#MODS[@]} -eq 0 ]; then
  # Uma linha só, separada por espaços: no Windows o Python escreve CRLF em quebras de linha.
  names="$(python -P -c "import json,sys; sys.stdout.write(' '.join(m['name'] for m in json.load(open(sys.argv[1],encoding='utf-8'))['modules']))" "$OUT/link.json")"
  read -r -a MODS <<< "$names"
fi
for m in "${MODS[@]}"; do
  gen="$OUT/gen/$m"; obj="$OUT/obj/$m"
  rm -rf "$gen" "$obj"; mkdir -p "$obj"
  t0=$(date +%s)
  python -P -m vitarecomp recomp --link "$OUT/link.json" --module "$m" -o "$gen" 2>&1 | tail -4
  t1=$(date +%s)
  cp "$HERE/runtime/arm/arc_runtime.c" "$HERE/runtime/arm/arc_dll.c" "$gen/"
  ( cd "$gen" && ls *.c | xargs -P "$JOBS" -I{} sh -c \
      '"$CC" $CFLAGS {} -o "'"$obj"'/$(basename {} .c).o" 2> "'"$obj"'/$(basename {} .c).err" || { echo "   FALHOU {}"; head -5 "'"$obj"'/$(basename {} .c).err"; }' )
  "$CC" -shared -o "$OUT/$m.dll" "$obj"/*.o
  t2=$(date +%s)
  echo "== $m: tradução $((t1-t0))s, compilação+link $((t2-t1))s, $(du -h "$OUT/$m.dll" | cut -f1)"
done
