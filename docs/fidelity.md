# Modelo de fidelidade

O oráculo é o `qemu-mipsel` 8.2 (modo usuário). Tudo o que a arquitetura MIPS deixa
como *UNPREDICTABLE* segue o comportamento do QEMU, porque é contra ele que os testes
comparam. Diferenças de hardware real (R3000/R4300/R5900) podem ser adicionadas por
jogo, como "quirks" no runtime.

## Reproduzido exatamente (verificado por fuzzing e lockstep)

| Aspecto | Como |
|---|---|
| Delay slots, branch-likely (slot anulado), salto para dentro de um delay slot | emissor (`control()`), rótulos `L_`/`N_` |
| `div`/`divu` por zero e `INT_MIN / -1` | divisor trocado por 1 (`RC_DIV`) |
| `lwl/lwr/swl/swr` (little-endian) | `rc_lwl`… em `recomp.h` |
| FPU modo **FR=0**: doubles em pares par/ímpar | `RC_FD`/`RC_SET_FD`; ELF com FP64 é recusado |
| `madd/msub/nmadd/nmsub.fmt` **não fundidos** (mul e add arredondados separadamente) | medido no QEMU: `madd` dá 0x0, a versão fundida daria 0x28800000 |
| NaN legado (pré-NaN2008): todo NaN gerado vira o NaN padrão `0x7fbfffff` / `0x7ff7ffff_ffffffff` | caminho frio `RC_FIX_*` |
| `abs/neg.fmt` como operação de bit (preserva payload de NaN) | emissor |
| FP→int inválido/fora da faixa = `0x7fffffff`; modos RN/RZ/RP/RM | `rc_to_w` |
| Traps de overflow (`add/sub/addi`), `teq`… | `rc_overflow`/`rc_trap` (encerram com diagnóstico) |

## Não modelado (divergências conhecidas)

| Aspecto | Impacto | Como resolver se um jogo precisar |
|---|---|---|
| Flags IEEE do FCSR (bits *cause* e *sticky*) | jogos raramente leem | `fetestexcept` após cada operação (caro) |
| Modo de arredondamento ≠ RN para **aritmética** (`ctc1`) | `cvt.w` respeita o modo; `add.s`… não | `fesetround` no host / FPSCR.RMode no Vita |
| Exceções de FPU habilitadas, exceções de endereço desalinhado | o processo não sofre trap | instrumentar loads/stores em modo de depuração |
| Endereços fora dos 32 MiB do guest | sofrem *wrap* (máscara) | modo checado opcional |
| Código automodificável / gerado em tempo de execução | não suportado (estático) | fallback para interpretador |
| `jr $ra` para um endereço diferente do de retorno do chamador | assume a convenção de retorno | verificação opcional de `$ra` |

## Lições registradas

* **UB em C quebra a fidelidade.** `mul` emitido como multiplicação `int32_t` com sinal:
  o GCC assumiu que não havia overflow (`x*x >= 0`) e o resultado ficou errado só com
  `-O1` e registradores promovidos. Correção: aritmética sem sinal + `-fwrapv`.
* **Stubs de ABI do sistema podem sombrear a libgcc.** Linkar `SceRtabi` fez
  `__aeabi_d2lz` virar uma importação que o Vita3K não implementa, e toda conversão
  FP→int64 do runtime saiu errada. Ela deve vir da libgcc.
* **O linker script não pode descartar `.MIPS.abiflags`:** sem ela o QEMU escolhe outro
  modo de FPU e a própria referência fica errada.
