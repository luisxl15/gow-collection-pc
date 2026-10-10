# God of War Collection: extração, inventário e cobertura do tradutor

*Sessão de 2026-10-09, executada localmente no Windows 11 do usuário (não mais no container).*

Classificação usada: **TESTADA** (comando executado aqui, resultado observado), **CONFIRMADA**
(código/documentação), **INFERIDA**, **DESCONHECIDA**.

## 1. Entrada

| Artefato | SHA-256 |
|---|---|
| `gowHD_PSV.pkg` (3.336.914.560 bytes) | `e00a3198376e2e1f99c70786f7d5454a8b9bdf3f8021aaa503e2199e95547417` |
| Licença RIF (512 bytes, mesmo content ID) | — (não registrado: é a licença do usuário) |

Content ID `UP9000-PCSA00126_00-GOWCUS030100SHIP`, PKG key type 2, 279 itens.
Hashes de todos os SELFs (camada PFS já removida) e dos ELFs decifrados: `gow-dump/sha256.txt`.

## 2. Cadeia de decifragem (TESTADA)

São três camadas, e cada uma tem sua ferramenta:

| Camada | Ferramenta | Evidência de que está certa |
|---|---|---|
| PKG (AES-128-CTR, chave pública do formato) | `tools/vitapkg.py` (novo) | 279 nomes de arquivo legíveis; `param.sfo` com magic `\0PSF` |
| PFS (arquivos do app) | Vita3K `continuous` 2026-10-09, modo portátil, `--pkg --zrif` | SELFs passam a começar com `SCE\0`, `.psarc` com `PSAR` |
| NPDRM (SELF → ELF) | `python -m vitarecomp decrypt --rif` (novo, `selfdec.py`) | `MetadataInfo` passa no teste de padding zero; cada segmento descomprime por zlib exatamente em `p_filesz` |

* `tools/zrif.py`: converte o `work.bin` em zRIF (deflate com o dicionário do libzrif) e confere ida e volta.
* `selfdec.py` é um porte de `decrypt_fself`/`get_segments` do Vita3K (GPL-2.0). As chaves de sistema
  **não** ficam no repositório: `vitarecomp keys-import <Vita3K>/vita3k/packages/src/sce_utils.cpp`
  grava `~/.vitarecomp/keys.json`.
* Armadilha do Vita3K (TESTADA): com `config.yml` recém-criado, `pref-path: ""` faz a instalação pela
  linha de comando falhar com `create_directories: Invalid argument`. Basta definir o caminho.

## 3. Inventário dos módulos (TESTADA, `vitarecomp inspect`)

| Módulo | Nome interno | Código RX | Relocações | Imports | Funções |
|---|---|---|---|---|---|
| `eboot.bin` | `Launcher_PSP2_Disk_Ship` | 0x85c1c | 18.449 | 259 / 21 libs | 1.411 |
| `gow1.self` | `Internal3_PSP2_Ship_AMERIC` | 0x214444 | 57.787 | 359 / 25 libs | 8.111 |
| `gow2.self` | `Game_PS2_PSP2_Ship_AMERICA` | 0x2345e0 | 57.757 | 348 / 25 libs | 8.016 |
| `libc.suprx` | `SceLibc` | 0x4d3bc | 9.405 | 58 | 1.465 |
| `libfios2.suprx` | `SceLibFios2` | 0x2b12c | 6.752 | 67 | 474 |
| `libsmart.suprx` | `SceSmart` | 0x1fee9c | 10.803 | 74 | 1.496 |
| `libult.suprx` | `SceUlt` | 0x13a9c | 3.986 | 28 | 135 |
| `libface.suprx` | `SceFace` | 0x17ed8 | 996 | 27 | 443 |

* **Todo o código é Thumb-2** (24.551 funções, nenhuma em modo ARM).
* O `eboot.bin` é só o lançador; cada jogo é um SELF à parte. O nome `Game_PS2_PSP2` do `gow2`
  confirma (INFERIDA quanto ao histórico) que é o código do PS2 portado.
* `gow1`: 85 imports de `SceGxm` (81 com corpo no HLE do Vita3K, 3 *stubbed*, 1 ausente).
  As faltas no HLE ficam em `SceLibc`/`SceLibm`/`SceLibstdcxx`/`SceFios2`, que o jogo traz em
  `sce_module/` e o Vita3K executa por LLE. Relatórios JSON: `gow-dump/*-inspect.json`.

## 4. Tradutor ARM → C no jogo real (TESTADA)

`scripts/build-gow-host.sh` traduz e compila os 8 módulos (clang 22, x86-64, `-O1`, 4 em paralelo):

| Módulo | Instruções | Não traduzidas | C | Tradução | Compilação | Objetos |
|---|---|---|---|---|---|---|
| gow1 | 633.287 | 0 | 112 MB | 35 s | 30 s | 30/30 |
| gow2 | 673.074 | 0 | 115 MB | 39 s | 31 s | 29/29 |
| eboot | 99.788 | 0 | 19 MB | 6 s | 5 s | 7/7 |
| libc | 78.925 | 5 → 0 | 15 MB | 5 s | 4 s | 7/7 |
| libfios2 | 45.808 | 0 | 8,7 MB | 4 s | 3 s | 4/4 |
| libsmart | 221.735 | 3 → 0 | 40 MB | 12 s | 12 s | 7/7 |
| libult | 16.388 | 0 | 3,0 MB | 1 s | 2 s | 3/3 |
| libface | 28.749 | 2 → 0 | 4,8 MB | 2 s | 4 s | 4/4 |

"Compila" **não** é "funciona": nada disso foi linkado nem executado ainda, porque falta o runtime/HLE
(seção 7).

### Defeitos encontrados pelo jogo real e corrigidos

| # | Defeito | Como apareceu | Correção |
|---|---|---|---|
| 1 | `sceModuleInfo` guarda `module_start` **sem** o bit Thumb | entrada decodificada em ARM (lixo) | usa o endereço do export correspondente (`0x810983cd`) |
| 2 | ABS32 com alvo par no segmento RX é quase sempre `.rodata` | 716 alvos pares contra 7.077 ímpares | só alvos com bit Thumb; pares MOVW/MOVT com alvo ímpar (1.622) entram como ponteiros |
| 3 | Alvo de BL fora do código ia para a fila → `KeyError` | crash da análise | alvo validado e registrado como aviso |
| 4 | Limite de `tbb`/`tbh` olhava as 6 últimas instruções **em ordem de exploração** | 14 tabelas sem limite | caminho contíguo por endereço, cópias e deslocamentos (`subs r0,r1,#0xf0`), constantes `movw/movt`, desvio de guarda tomado ou não |
| 5 | Qualquer `cmp` era aceito como guarda | `cmp lr,#0` dava 1 caso a um switch de 14 | só `b{hi,gt,hs,ge}` não tomado ou `b{ls,le,lo,lt}` tomado |
| 6 | Emissor dependia da queda implícita do C entre instruções | decodificações sobrepostas executariam bytes errados | `goto` explícito quando a próxima instrução emitida não é a seguinte real |
| 7 | Fluxo sequencial atravessava a entrada da função seguinte | funções de 17 mil linhas no gow2; clang sem memória | fronteira na entrada conhecida + análise em duas passadas; **fator de duplicação 2,1× → 1,00×** |
| 8 | `mrc p15,0,Rt,c13,c0,3` (TPIDRURO) sem tradução | libc | lê `ctx->tpidruro` (e TPIDRURW por `mrc`/`mcr`) |
| 9 | `vcvtb`/`vcvtt` f16↔f32 sem tradução | libc | `arc_f16_to_f32`/`arc_f32_to_f16` com AHP/DN/FZ/RMode |
| 10 | `vmov` NEON com `sb`/`fp` (apelidos de r9/r11) | libsmart, libface | apelidos aceitos |

### Controles

* **Tabelas de salto** (`tools/check_tables.py`): o limite provado pela guarda é comparado com o
  limite estrutural (a tabela termina onde começa o menor alvo). Em 396 tabelas dos 8 módulos,
  385 coincidem e 11 têm guarda menor que o layout, todas explicadas (bloco `default` ou de outro
  switch logo após a tabela). Em **nenhuma** a guarda ultrapassa o layout.
* **Invariante de decodificação** (`tools/check_overlap.py`): nenhuma instrução pode começar no meio
  de outra na mesma função. Começou em **542 sobreposições**; terminou em **0** nos 8 módulos.
* Unicidade: cada endereço é emitido uma única vez (633.287 emitidas = 633.287 únicas no gow1).

## 5. Oráculo no Windows (TESTADA)

Sem `qemu-arm` nem `arm-linux-gnueabihf-gcc` nesta máquina, os testes diferenciais passaram a usar:

* guest: clang `--target=armv7a-linux-gnueabihf` + `ld.lld` + `libclang_rt.builtins-arm-android.a`
  do NDK (os `__aeabi_*` usam a convenção base em qualquer ABI);
* referência: `tests/oracle_unicorn.py` (Unicorn 2.1.4, Cortex-A9, VFP ligado);
* recompilado: clang x86-64 + `runtime/arm/arc_win.c` (janela de 4 GiB só reservada; páginas
  confirmadas sob demanda por um tratador de exceção vetorizado).

`run_fuzz_arm.py` escolhe sozinho qemu ou Unicorn; `tests/run_diff_arm.py` é o diferencial de
programas inteiros.

| Teste | Resultado |
|---|---|
| `t_half.c`: 4.096 conversões f16↔f32 × 32 combinações de FPSCR | **4.096/4.096** (a primeira rodada achou 32 erros: NaN com AHP deve dar `FPZero(sign)`) |
| Suítes de programa (`t_arith/control/float/half/mem` × -O0..-Os), Thumb | **25/25** |
| Suítes de programa, modo ARM | **25/25** |
| Fuzzer Thumb-2 + VFP (sementes 1-200, 1001-1150, 5001-5060) | **410/410** |
| Fuzzer ARM + VFP (sementes 1-100, 1001-1150) | **250/250** |
| Fuzzer NEON (sementes 1001-1150) | **150/150** |

O oráculo novo achou quatro coisas que o qemu/gcc escondiam: `ldm.w` de um registrador
(UNPREDICTABLE; o gerador agora usa 2+), `vext.64` em D recusado pelo clang, `smmul` (o clang o usa em
`((int64)a*b)>>32`; tradução nova para `smmul/smmla/smmls[r]`) e o `bx ip` calculado do `pc` na
`__udivsi3` do compiler-rt (a varredura linear agora cobre esse caso).

### Retorno para outro lugar que não o chamador

Todo retorno grava em `ctx->r[15]` o alvo real (o `lr` ou o `pc` desempilhado). Depois de cada chamada,
o código gerado confere `r[15]` contra o endereço seguinte; se divergir (longjmp do guest, troca de
contexto), chama `arc_ret_mismatch`, que nos testes é fatal e no backend do Vita3K desempilha o C.

## 6. REA neste ambiente (TESTADA)

* `rea-agents` 6.2.0 instalado e registrado no Claude Code (`rea setup --client claude_code`):
  entrada MCP em `~/.claude.json` (com backup) e a skill `reverse-engineer-anything`.
* `rea inspect-artifact gow1.elf`: funciona (inventário + SHA-256, formato `elf`, arquitetura `null`).
  Evidência: `gow-dump/rea/artifact-gow1.json`.
* `rea inspect-binary-layout`: `capability_unavailable` — o perfil pwntools só existe em Linux x64.
* Funções/pseudocódigo exigem Ghidra/IDA/Hopper. O provedor Ghidra do REA no Windows aceita
  **apenas PE x86/x86-64** (docs/windows-ghidra-p0.md do REA), portanto não serve para ARMv7.
  Alternativa válida: Ghidra direto (lê ELF ARM) como controle independente da descoberta de funções.

## 7. O que falta e o próximo incremento

| Pendência | Situação |
|---|---|
| **Fase 3 perdida** | os commits `a673d4c..a1e1ecb` (backend no Vita3K, fallback para JIT, LDREX/STREX atômicos, multithread) não estão no bundle. Precisam ser refeitos sobre o Vita3K atual. |
| Atômicos | `ldrex/strex` hoje são não atômicos e `dmb/dsb` são no-op: **errado** para o jogo, que é multithread. Refazer antes de executar. |
| Executar o jogo | depende da fase 3 (HLE do Vita3K) e de firmware para módulos de sistema, se o jogo pedir. |
| Renderizador Vulkan nativo | depois que o caminho de execução existir; ponto de partida: o renderizador do Vita3K. |
| Ghidra como controle | comparar as 8.111 entradas de função do gow1 com as de uma análise independente. |
