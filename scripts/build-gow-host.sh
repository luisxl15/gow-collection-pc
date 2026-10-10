#!/usr/bin/env bash
# Recompila os módulos do God of War Collection (ELFs já decifrados) para C e compila o C
# para o host (x86-64) com clang. Não linka: falta o runtime/HLE (fase 3).
#
#   scripts/build-gow-host.sh [DUMP_DIR] [módulos...]
#   DUMP_DIR padrão: ../gow-dump   (espera DUMP_DIR/elf/<módulo>.elf)
#   JOBS (padrão 4): compilações em paralelo. Com 8 GB de RAM, 12 estoura a memória do clang.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DUMP="$(cd "${1:-$HERE/../gow-dump}" && pwd)"; shift || true
MODS=("$@"); [ ${#MODS[@]} -eq 0 ] && MODS=(gow1 gow2 eboot libc libfios2 libsmart libult libface)
CC="${CC:-clang}"
command -v "$CC" >/dev/null || CC="/c/Program Files/LLVM/bin/clang.exe"
JOBS="${JOBS:-4}"
CFLAGS="-c -O1 -w -fwrapv -fno-strict-aliasing -ffp-contract=off -fno-math-errno -I $HERE/runtime/arm"
export PYTHONPATH="$HERE/vitarecomp" CC CFLAGS

for m in "${MODS[@]}"; do
  gen="$DUMP/gen/$m"; obj="$DUMP/obj/$m"
  rm -rf "$gen" "$obj"; mkdir -p "$obj"
  echo "== $m"
  t0=$(date +%s)
  python -P -m vitarecomp recomp "$DUMP/elf/$m.elf" -o "$gen" 2>&1 | tail -6
  t1=$(date +%s)
  ( cd "$gen" && ls *.c | xargs -P "$JOBS" -I{} sh -c \
      '"$CC" $CFLAGS {} -o "'"$obj"'/$(basename {} .c).o" 2> "'"$obj"'/$(basename {} .c).err" || echo "   FALHOU {}"' )
  t2=$(date +%s)
  nsrc=$(ls "$gen"/*.c | wc -l); nobj=$(ls "$obj"/*.o 2>/dev/null | wc -l)
  echo "   tradução $((t1-t0))s, compilação $((t2-t1))s, $nobj/$nsrc objetos, C $(du -sh "$gen" | cut -f1), obj $(du -sh "$obj" | cut -f1)"
done
