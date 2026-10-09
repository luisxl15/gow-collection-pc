# Tradutor ARMv7 → C (`vitarecomp/vitarecomp/arm`)

Recompilador estático do código do PS Vita (Cortex-A9: ARM + Thumb-2 + VFPv3 + NEON)
para C portável, base do port do *God of War Collection* para PC.

## Como funciona

| Etapa | Arquivo | Detalhe |
|---|---|---|
| Imagem do guest | `image.py` | ELF ARM Linux (testes) ou módulo Vita (`eboot.bin`/`.suprx`, via `module.py`). As relocações ABS32 viram evidência exata de ponteiros de código. |
| Decodificação | `analysis.py` | Usa o capstone 5, com correções: os blocos IT são decodificados inteiros (o estado IT é sequencial) e o bit S vem da codificação bruta, porque o capstone erra, por exemplo, `ADC.W`. |
| Descoberta de funções | `analysis.py` | Descida recursiva a partir de entry, símbolos/exports, alvos de BL/BLX e ponteiros de código. Tabelas TBB/TBH são resolvidas pelo `cmp` de guarda. Saltos calculados disparam varredura linear da função. |
| Emissão C | `emit.py` | r0–r14 e flags NZCV em variáveis locais, escritos de volta no contexto em chamadas, syscalls e retornos. Carry do shifter e de imediatos modificados. Todos os modos de endereçamento, LDM/STM, execução condicional e IT. Distinção BranchWritePC × BXWritePC. VFP escalar com regras de NaN do ARM e conversões saturadas, incluindo ponto fixo. |
| NEON | `neon.py` | Cada instrução copia as fontes para buffers locais (sobreposição tratada como no hardware) e calcula lane a lane. Floats seguem o *Standard FPSCR* (flush-to-zero + default NaN). Estimativas `vrecpe`/`vrsqrte` pelo pseudocódigo do ARMv7. |
| Runtime | `runtime/arm/arc.h`, `arc_runtime.c` | Janela plana de 4 GiB para a memória do guest, despacho indireto por tabela e helpers de semântica. |
| Personalidade Linux | `runtime/arm/arc_linux.c` | Syscalls ARM EABI, usadas só nos testes contra o `qemu-arm`. |

```
python -m vitarecomp recomp --linux guest.elf -o gen/      # testes
python -m vitarecomp recomp eboot.bin -o gen/              # módulo do Vita
python -m vitarecomp recomp --trace-insn ...               # trace por instrução (lockstep)
```

## Verificação (oráculo: `qemu-arm`, mesmo ISA do Vita)

| Teste | Resultado |
|---|---|
| 4 suítes C (inteiro, controle, float, memória) × -O0/-O1/-O2/-O3/-Os, Thumb-2, com NEON autovetorizado | **20/20 idênticas** |
| As mesmas suítes compiladas em **modo ARM** | **4/4** |
| Raycaster (guest "jogo") em -O2 e -O3 (com NEON) | idêntico |
| Fuzzing Thumb-2 + VFP (estado completo + memória) | **620/620** |
| Fuzzing modo ARM + VFP | **420/420** |
| Fuzzing NEON (d0–d31 + memória; entradas com denormais/NaN/inf) | **460/460** |
| `tools/lockstep_arm.py` | aponta a 1ª instrução divergente contra o log *single-step* do QEMU |

### Bugs encontrados pelo método (e corrigidos)
* Estado de bloco IT perdido ao decodificar instrução por instrução.
* Capstone 5 marca `ADC.W` (e outras) como alterando flags sem o bit S → bit S lido da codificação.
* Em ARM, o sinal do pós-índice em `LDRH/LDRSB/STRH` vem só do bit U (o capstone o descarta).
* Alias `lsl rd, rm, #n` em modo ARM (= `mov` com shift no operando).
* `mov pc, rX` em Thumb não troca de modo (BranchWritePC).
* Modo de `vstmia`/`vldmia` extraído errado do nome.
* `vabs.f32`/`vneg.f32` do NEON são operações de bit, sem processar NaN.

## Lacunas conhecidas (não modeladas ainda)

| Item | Impacto | Plano |
|---|---|---|
| Flags acumuladas: Q (APSR), QC e exceções IEEE (FPSCR) | raramente lidas por jogos | modelar se o jogo ler |
| `vcvt` meia precisão (f16), `vmull.p8` | raras | sob demanda |
| LDREX/STREX com várias threads | single-thread hoje (STREX sempre sucede) | atômicos C11 na fase de threads |
| Modos de arredondamento ≠ RN na aritmética VFP | idem MIPS | `fesetround` se necessário |
| Faltas de alinhamento/acesso | não geram trap | modo checado de depuração |
| Imports do Vita (NIDs) | `arc_import` ainda sem HLE | **próxima fase**: ligar ao HLE do Vita3K |
