// Backend de código recompilado (vitarecomp) para o Vita3K.
//
// Chamado pelo laço da thread no lugar de run(*cpu): se o PC atual é a entrada de uma função
// recompilada, executa o C nativo e devolve true (o PC fica no alvo real de retorno). Caso
// contrário devolve false e o JIT (dynarmic) segue como sempre.
//
// Ativação: VITA3K_RECOMP_LIB=<módulo recompilado .dll/.so>. VITA3K_RECOMP_DISABLE=1 força o JIT
// no mesmo binário (referência). VITA3K_RECOMP_LOG=1 registra cada fallback para o JIT.
#pragma once

struct ThreadState;

bool recomp_try_run(ThreadState &thread);
