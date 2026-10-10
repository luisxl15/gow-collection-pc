#!/usr/bin/env python3
"""Aplica o backend de código recompilado a um checkout do Vita3K (revisão a01f2b15).

    python integrations/vita3k/apply.py <checkout do Vita3K>

Idempotente. Sem -DVITA3K_RECOMP_RUNTIME=<vita-recomp-lab> no CMake o build fica idêntico ao
original: as mudanças nos fontes estão atrás de #ifdef VITA3K_RECOMP.
"""
from __future__ import annotations

import sys
from pathlib import Path

MARK = "vita-recomp-lab"


def patch(path: Path, old: str, new: str) -> str:
    s = path.read_text(encoding="utf-8")
    if new in s:
        return "já aplicado"
    if s.count(old) != 1:
        raise SystemExit(f"{path}: trecho esperado não encontrado ({s.count(old)} ocorrências): {old[:60]!r}")
    path.write_text(s.replace(old, new), encoding="utf-8", newline="")
    return "aplicado"


def main() -> int:
    root = Path(sys.argv[1]) / "vita3k"
    if not (root / "kernel/src/thread.cpp").exists():
        raise SystemExit(f"{root}: não parece um checkout do Vita3K")
    changes = []

    # 1. O backend precisa de call_level/to_do/kernel/mem, que são privados.
    changes.append(("thread_state.h", patch(
        root / "kernel/include/kernel/thread/thread_state.h",
        "struct ThreadState {\n    std::mutex mutex;",
        f"struct ThreadState {{\n    friend struct RecompAccess; // {MARK}: backend de código recompilado\n    std::mutex mutex;")))

    # 2. Gancho no laço da thread: antes de entregar ao JIT, tenta o código nativo.
    t = root / "kernel/src/thread.cpp"
    changes.append(("thread.cpp include", patch(
        t, "#include <kernel/thread/thread_state.h>\n",
        "#include <kernel/thread/thread_state.h>\n"
        f"#ifdef VITA3K_RECOMP // {MARK}\n#include \"recomp_backend.h\"\n#endif\n")))
    changes.append(("thread.cpp run", patch(
        t,
        "                if (do_step)\n                    res = step(*cpu);\n                else\n                    res = run(*cpu);",
        "                if (do_step)\n                    res = step(*cpu);\n"
        f"#ifdef VITA3K_RECOMP // {MARK}: entrada de função recompilada roda nativa\n"
        "                else if (recomp_try_run(*this))\n                    res = 0;\n#endif\n"
        "                else\n                    res = run(*cpu);")))

    # 3. CMake: só com VITA3K_RECOMP_RUNTIME apontando para o vita-recomp-lab.
    changes.append(("kernel/CMakeLists.txt", patch(
        root / "kernel/CMakeLists.txt",
        "source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_LIST})",
        "source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR} FILES ${SOURCE_LIST})\n\n"
        f"# {MARK}: backend de código recompilado (-DVITA3K_RECOMP_RUNTIME=<vita-recomp-lab>)\n"
        "if(VITA3K_RECOMP_RUNTIME)\n"
        "\ttarget_sources(kernel PRIVATE ${VITA3K_RECOMP_RUNTIME}/integrations/vita3k/recomp_backend.cpp)\n"
        "\ttarget_include_directories(kernel PRIVATE ${VITA3K_RECOMP_RUNTIME}/integrations/vita3k"
        " ${VITA3K_RECOMP_RUNTIME}/runtime/arm)\n"
        "\ttarget_compile_definitions(kernel PRIVATE VITA3K_RECOMP=1)\n"
        "endif()")))

    for name, status in changes:
        print(f"  {name:24} {status}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
