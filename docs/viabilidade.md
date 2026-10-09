# Relatório de viabilidade: recompilação estática para ports nativos no PS Vita

*Estado em 2026-10-09. Nenhum jogo-alvo foi fornecido. Tudo abaixo foi medido
sobre binários guest construídos no próprio laboratório e vale como infraestrutura
genérica mais uma prova de conceito reproduzível.*

## 1. Resposta curta

**É viável**, e foi demonstrado de ponta a ponta, recompilar estaticamente código
MIPS32 para C, compilar para o CPU do Vita e rodar como homebrew com fidelidade
bit a bit em relação à referência. O gargalo para um *jogo real* não é a tradução
de CPU. É **(a)** reconstruir as APIs da plataforma de origem (GPU, áudio, E/S,
threads) e **(b)** caber no orçamento de CPU/memória do Vita.

## 2. Pipeline

```
ELF guest (MIPS32r2, FR=0)
  └─ mipsrecomp: parse ELF → decodifica → descobre funções/jump tables → emite C
       └─ C + runtime (recomp_runtime.c) + plataforma (rc_platform_*)
            ├─ host x86-64 / ARMv7 Linux  → testes diferenciais contra qemu-mipsel
            └─ arm-none-eabi-gcc (Thumb-2, NEON, hard-float, -Wl,-q)
                 → vita-elf-create → vita-make-fself → eboot.bin
                 → vita-mksfoex → param.sfo → vita-pack-vpk → .vpk → Vita3K / console
```

| Pergunta do projeto | Resposta |
|---|---|
| O que depende da arquitetura de origem? | Registradores, delay slots, FPU FR=0, semântica de NaN/divisão, ABI de syscalls. Tudo isso foi isolado em `rc_ctx` + `recomp.h` + `recomp_runtime.c`. |
| Há estratégia de recompilação funcional para ARMv7-A? | Sim: C intermediário. O GCC gera Thumb-2 + VFP para o Cortex-A9. Validado sob `qemu-arm` com as mesmas flags de ABI do Vita e no Vita3K. |
| O que precisa ser substituído? | Syscalls/serviços do SO (feito: write/exit/present/buttons/ticks), GPU (para jogos reais: tradução para `sceGxm`), áudio (`sceAudioOut`), E/S de arquivos (`sceIo*`). |
| O que pode ser reaproveitado? | 100% do código de lógica do jogo (traduzido), dados do executável (embutidos) e a libgcc do guest (também recompilada). |
| Como usar o Vita3K? | Compilado do fonte (revisão pré-Qt `a01f2b15`), headless com Xvfb + lavapipe; `scripts/run-vita3k.sh` instala, roda e captura a tela. O log do Vita3K mostra imports não implementados (foi assim que achei o bug do `__rtabi_d2lz`). |
| Como usar o REA? | `rea inspect-artifact` no `.vpk` (grafo + SHA-256) e `rea inspect-binary-layout` no binário recompilado x86-64 (seções, símbolos, mitigações). Limite: o REA 6.2 não aceita ELF32/MIPS/SELF; a análise nativa de função exige Ghidra/Hopper, cujos downloads estão bloqueados aqui. |
| Como produzir um homebrew válido? | `scripts/build-vita-port.sh` → `.vpk` com SELF *fake-signed* que carrega no Vita3K (segmentos em `0x80005000`, NIDs resolvidos). |

## 3. Medições

**Fidelidade**

* 25/25 casos (4 suítes C + raycaster) × níveis -O0/-O1/-O2/-O3/-Os: saída idêntica
  entre QEMU, x86-64 e ARMv7.
* Fuzzing de instruções: mais de 650 programas aleatórios (até 400 instruções cada,
  cobrindo ALU, HI/LO, loads/stores desalinhados, branches/likely, saltos para delay
  slot, chamadas, COP1/COP1X) com comparação de todos os GPRs (exceto `$sp`), HI/LO,
  FPRs, FCSR (exceto flags) e memória: 0 divergências.
* `tools/lockstep.py` aponta a 1ª instrução divergente comparando o log single-step
  do QEMU com o trace do código recompilado. Foi com ele que achei a semântica
  real de NaN do QEMU.

**Desempenho** (raycaster 320×240, 600 quadros, x86-64, tempo de usuário)

| Variante | Tempo |
|---|---|
| `qemu-mipsel` (JIT) | 0,97 s |
| recompilado, registradores em `ctx` | 0,44 s |
| recompilado, registradores promovidos a locais + `-fno-thread-jumps` | **0,33 s** |

Nota: com promoção de registradores, o *jump threading* do GCC deixava o código 40%
**mais lento** (duplicação de blocos em funções grandes com `goto`); o clang não
sofre disso. `-fno-thread-jumps` resolve.

**Tamanho**: raycaster (2,2 mil instruções MIPS) → `eboot.bin` de 30 KB, VPK de 27 KB.

## 4. O que ainda **não** foi demonstrado

1. **Desempenho em hardware real.** O Vita3K não é medidor de ciclos. Falta rodar o
   VPK num console desbloqueado e medir quadros/s. O Cortex-A9 a 444 MHz não tem
   divisão inteira em hardware (`div` vira chamada à libgcc) e tem VFPv3 sem FMA.
2. **Um jogo real.** Requer o executável e a especificação da plataforma de origem.
   Para um título de PS1/N64, a CPU é a parte resolvida aqui. O esforço principal
   seria: GPU/RSP (renderizar via `sceGxm` com shaders GXP), SPU/áudio, DMA,
   interrupções/VSync e a pilha de E/S.
3. **Big-endian.** N64/PS2-EE com dados BE e PowerPC (GameCube/Wii/Xbox 360/PS3)
   precisam de byte-swap nos acessos (`RC_LW`…); o desenho dos macros já isola isso.
4. **Multithreading / co-processadores** (VU0/VU1 do PS2, SPUs do PS3): fora de escopo.

## 5. Critérios de viabilidade para um alvo real

Um título é bom candidato se:

* o executável é estático, sem geração de código em tempo de execução (ou com
  overlays enumeráveis);
* a CPU de origem é ≤ ~100 MHz MIPS/ARM, ou o jogo tem folga de CPU (o Vita tem
  4×A9 a 444 MHz, 512 MB de RAM e 128 MB de VRAM);
* a renderização pode ser mapeada para `sceGxm` (rasterização triangular, texturas
  compatíveis, sem efeitos dependentes de framebuffer em leitura constante);
* existe um oráculo (emulador de referência) para testes diferenciais e lockstep.

Critério de "port funcional" (≠ "compilou"): saída idêntica à referência nos testes
de regressão, quadros de referência com hash igual, sem imports não implementados no
log do Vita3K, e quadros/s estáveis medidos em hardware.

## 6. Bloqueios do ambiente (e como foram contornados)

| Bloqueio | Contorno |
|---|---|
| Downloads de *releases* do GitHub (VitaSDK, Vita3K, Ghidra) → 403 | Toolchain e Vita3K compilados do fonte via `git clone` |
| libelf 0.8.13 indisponível | espelho git da 0.8.12 (API compatível); requisito relaxado localmente |
| Vita3K atual exige Qt ≥ 6.11 (Ubuntu tem 6.4; espelhos Qt bloqueados) | revisão imediatamente anterior à migração para Qt |
| newlib do VitaSDK indisponível | runtime *freestanding* próprio (`vita_crt0.c`, console de framebuffer) |
| Firmware do Vita (fontes, registry) ausente | não necessário para este homebrew; o Vita3K só emite avisos |
| Docker indisponível | não necessário |

## 7. Próximos passos recomendados

1. Receber/definir o jogo-alvo e a arquitetura de origem e rodar o `mipsrecomp` nele
   (ele falha de forma explícita em instruções/fluxos não suportados).
2. Camada de GPU: tradutor de display lists → `sceGxm`, começando por um renderizador
   por software no framebuffer (já existe) para validar a lógica.
3. Áudio (`sceAudioOut`) e E/S de arquivos (`sceIoOpen`…) no `platform_vita.c`.
4. Medir em hardware; se necessário, otimizações: *inlining* de folhas, eliminação de
   *sync* de registradores mortos e `RC_MEM_MASK` desligável em builds de release.
5. Backends adicionais da camada `rc_platform_*` (Android/Windows), que reaproveitam
   o mesmo C gerado.
