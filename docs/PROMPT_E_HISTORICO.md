# Prompt original e histórico do projeto

*Projeto: **vita-recomp-lab**, recompilação estática e ports nativos com foco no PS Vita.
Registro gerado em 2026-10-09.*

---

## 1. Prompt original

> **Aviso:** o prompt chegou a esta sessão **truncado**. Ele veio de uma sessão anterior
> e o conteúdo após o item 4 da "Primeira etapa" não estava disponível. Abaixo está
> transcrito exatamente o trecho preservado. Para ter o documento completo, cole aqui o
> restante do texto original.

Mensagem inicial:

> instale https://github.com/morluto/rea e vamos fazer um projeto ambicioso

Prompt anexado:

> **PROMPT MESTRE — PS VITA NATIVE RECOMPILATION & PORTING LAB**
>
> **1. PAPEL E RESPONSABILIDADE**
>
> Atue como um engenheiro principal de software especializado em:
>
> * Engenharia reversa de binários e sistemas operacionais.
> * Recompilação estática de jogos e tradução de conjuntos de instruções.
> * Análise de executáveis ARMv7, PowerPC, x86 e x86-64.
> * Desenvolvimento de runtimes nativos e camadas de compatibilidade.
> * Arquiteturas de CPU, otimização SIMD e paralelismo.
> * GPUs PowerVR, shaders Cg/GXP e programação por meio da API `sceGxm`.
> * VitaSDK, homebrew, ELF/SELF, módulos, bibliotecas importadas, NIDs e ABI.
> * Emulação, recompilação, depuração, análise diferencial e testes de regressão.
> * CMake, compiladores cruzados, ferramentas de binários e integração contínua.
> * Desenvolvimento de jogos sob restrições rigorosas de memória, processamento e largura de banda.
>
> Trabalhe como uma equipe multidisciplinar de engenharia, não como um assistente que apenas sugere possibilidades.
> Seu objetivo é investigar, projetar e implementar uma infraestrutura tecnicamente viável para portar ou executar nativamente, no PlayStation Vita, jogos que exijam recompilação de código, adaptação de APIs ou reconstrução de subsistemas.
> Use o repositório de trabalho como fonte inicial da verdade. Identifique o jogo, a arquitetura de origem, o estágio atual do projeto e as ferramentas realmente disponíveis antes de escolher a implementação.
> Não abandone uma tarefa simplesmente porque ela é demorada, difícil, complexa ou exige várias etapas. Divida o problema, priorize as dependências, implemente incrementos verificáveis e prossiga até obter um resultado técnico útil.
> Não transforme essa determinação em justificativa para inventar resultados. Quando uma etapa depender de equipamento indisponível, um binário inexistente, uma dependência incompatível ou uma permissão que não foi concedida, demonstre o bloqueio, tente uma alternativa tecnicamente válida e continue nas etapas independentes.
>
> **2. MISSÃO PRINCIPAL**
>
> Investigue a viabilidade e desenvolva progressivamente um pipeline para ports nativos de jogos no PS Vita, com ênfase em jogos recompilados.
> O trabalho deverá responder e, sempre que os recursos disponíveis permitirem, demonstrar:
>
> 1. Como o código original é executado e quais componentes dependem da arquitetura de origem.
> 2. Se existe uma estratégia de recompilação que produza código funcional para ARMv7-A.
> 3. Quais APIs, bibliotecas, shaders, serviços e formatos de recursos precisam ser substituídos.
> 4. Quais componentes podem ser reaproveitados e quais precisam ser reconstruídos.
> 5. Como aproveitar o Vita3K para observar e validar o comportamento de executáveis Vita.
> 6. Como aproveitar o REA e outros analisadores para produzir evidências sobre binários e funções.
> 7. Como produzir um executável homebrew válido com VitaSDK.
> 8. Como medir desempenho, memória, estabilidade e fidelidade, sem confundir uma compilação bem-sucedida com um port funcional.
>
> O resultado deve ser um projeto de engenharia evolutivo, com código, ferramentas, documentação, testes e critérios claros de viabilidade.
> Não escolha arbitrariamente um jogo que não esteja presente no workspace ou não tenha sido fornecido. Se houver um projeto existente, investigue-o primeiro. Se não houver um jogo-alvo disponível, desenvolva a infraestrutura genérica e uma prova de conceito nativa reproduzível, deixando explícito o que ainda depende de um alvo real.
>
> **3. PRIMEIRA ETAPA: INSPECIONE O WORKSPACE**
>
> Antes de alterar arquivos:
>
> 1. Identifique o diretório de trabalho, o sistema operacional, o shell, as ferramentas instaladas e as limitações do ambiente.
> 2. Execute `git status` e examine o histórico recente. Preserve alterações preexistentes do usuário.
> 3. Faça um inventário de diretórios, submódulos, scripts de build, arquivos de configuração, dependências, testes e documentação.
> 4. Identifique a arquitetura de or
>
> *[o texto termina aqui no contexto disponível]*

### Instruções posteriores, na ordem em que chegaram

1. "responda somente em portugues brasileiro"
2. "sobre o qemu, nao precisa caso for usar o android para criar um apk, ou o windows para criar um .exe de determinado jogo"
3. "nao temos o vita real. o projeto é para criar um recomp do god of war do ps vita"
4. Respostas ao questionário:
   - Jogo: **God of War Collection (Vita)**
   - Plataforma: **PC (.exe / Linux)**
   - Dump: **ainda não**
5. "e nesse caminho fazer um renderizador vulkan nativo"
6. "está na minha area de trabalho o pkg do jogo"
7. "gere um .md com o prompt que enviei anteriormente e adicione tudo o que fez" *(este documento)*

---

## 2. Ambiente encontrado

| Item | Situação |
|---|---|
| Máquina | Container na nuvem, Ubuntu 24.04 x86-64, 4 núcleos, 15 GB de RAM, sem GPU |
| Workspace inicial | Vazio, sem repositório nem jogo |
| Rede | `git clone` do GitHub funciona; **downloads de releases do GitHub bloqueados (403)**; espelhos do Qt, Debian archive e outros sites bloqueados; apt, PyPI e npm liberados |
| Docker | Instalado, mas sem daemon |
| Hardware Vita | Não disponível (confirmado: "não temos o vita real") |
| Jogo | Não presente; o PKG está no computador do usuário, que esta sessão na nuvem não alcança |

---

## 3. O que foi feito

### 3.1 REA instalado e usado
* `rea-agents` 6.2.0 instalado globalmente (`npm install --global rea-agents`), repositório clonado.
* O provedor de ELF exige Python com pwntools 4.15.0, pyelftools 0.33 e unicorn 2.1.2: foi criado um venv dedicado (`REA_PWNTOOLS_PYTHON`).
* Evidências geradas em `docs/evidence/`:
  - `rea inspect-artifact` no `.vpk`: grafo do artefato com SHA-256.
  - `rea inspect-binary-layout` no binário recompilado x86-64: 32 seções, 131 símbolos, mitigações.
* **Limites do REA medidos:** não aceita ELF32 (MIPS e ARM do Vita) nem SELF. A análise de funções exige Ghidra ou Hopper, cujos downloads estão bloqueados.

### 3.2 Toolchain do PS Vita construída do código-fonte
Sem acesso às releases do VitaSDK, tudo foi montado via apt e `git clone` (`scripts/bootstrap-toolchain.sh`):
* `arm-none-eabi-gcc` 13 do Ubuntu, que tem o multilib exato do Vita (Thumb-2, ARMv7-A, NEON, hard-float).
* **vita-toolchain** compilada: `vita-elf-create`, `vita-make-fself`, `vita-mksfoex`, `vita-pack-vpk`, `vita-libs-gen-2`…
* libelf 0.8.12 via espelho git, no lugar da 0.8.13 exigida (API compatível; requisito relaxado localmente).
* **496 bibliotecas de stubs** de NIDs geradas a partir do vita-headers.
* Runtime *freestanding* próprio (crt0, mini-libc, console de framebuffer com fonte 8×8 de domínio público), já que a newlib do VitaSDK não estava disponível.

### 3.3 Recompilador estático MIPS32 → C (`recomp/mipsrecomp`)
Prova de conceito escolhida porque não havia jogo. MIPS é a CPU do PS1, N64, PS2 e PSP.
* Leitor de ELF32, decodificador MIPS32r2 + COP1/COP1X, descoberta de funções e jump tables.
* Emissor C com **delay slots exatos**: branch-likely, saltos para dentro de delay slot, chamadas, despacho indireto.
* **Promoção de registradores** para variáveis locais em C, com sincronização em chamadas e syscalls.
* Runtime portável com semântica idêntica ao QEMU: divisão por zero, FP→int, NaN legado do MIPS, FPU em modo FR=0.
* Camadas de plataforma: Linux (testes) e **PS Vita** (framebuffer, `sceCtrl`, timer, memória).
* Guest de demonstração: um **raycaster 3D** em MIPS que exercita FPU, texturas, entrada e framebuffer.

### 3.4 Verificação
| Teste | Resultado |
|---|---|
| Diferencial C: QEMU × x86-64 × ARMv7 (flags do Vita), níveis -O0…-Os | **25/25 casos idênticos bit a bit** |
| Fuzzing de instruções com estado arquitetural completo | **650+ programas, 0 divergências** (x86-64 e ARMv7) |
| `tools/lockstep.py`: primeira instrução divergente contra o log single-step do QEMU | Usado para descobrir a semântica real de NaN |

**Bugs reais encontrados e corrigidos:**
* registrador de FPU lido do campo errado em `lwc1/ldc1`;
* `add.d` em delay slot traduzido como soma inteira;
* modelo de NaN do QEMU (default-NaN) diferente do esperado;
* **comportamento indefinido**: `mul` com sinal, que o GCC explorou;
* o linker script descartava `.MIPS.abiflags`, o que mudava a própria referência;
* o stub `SceRtabi` sombreava a libgcc (`__aeabi_d2lz`).

### 3.5 Desempenho (raycaster, 600 quadros, x86-64)
| Variante | Tempo de usuário |
|---|---|
| QEMU (JIT) | 0,97 s |
| Recompilado, registradores em `ctx` | 0,44 s |
| Recompilado, registradores promovidos + `-fno-thread-jumps` | **0,33 s (2,9×)** |

Achado: o *jump threading* do GCC deixava o código promovido 40% mais lento.

### 3.6 Homebrew do Vita e execução no Vita3K
* `scripts/build-vita-port.sh`: ELF guest → C → `eboot.bin` (SELF) + `param.sfo` → **`.vpk` de 27 KB**.
* **Vita3K compilado do código-fonte.** A versão atual exige Qt ≥ 6.11, indisponível, então foi usada a revisão `a01f2b15`, anterior à migração para Qt. Ele roda headless com Xvfb e Mesa lavapipe (Vulkan por software).
* Configuração inicial, usuário e instalação automatizados (`scripts/run-vita3k.sh`).
* **Resultado:** o jogo-guest recompilado roda no Vita3K, renderiza o mesmo quadro da referência e responde a entrada (`docs/vita3k_raycast.png`, `docs/vita3k_input.png`).

### 3.7 Redirecionamento: God of War Collection → PC
* **Arquitetura** (`docs/gow-arquitetura.md`):
  - recompilação AOT do código ARMv7/Thumb-2 para C;
  - fallback para JIT no que não for coberto;
  - HLE do sistema reaproveitando o Vita3K;
  - **renderizador Vulkan nativo** (`sceGxm` → Vulkan), com tradução offline dos shaders GXP para SPIR-V;
  - Vita3K como oráculo de testes.
* **`vitarecomp` fase 1, carregador de módulos do Vita:**
  - lê SELF/FSELF/ELF SCE e segmentos zlib;
  - implementa **os 10 formatos de relocação SCE** e registra cada relocação aplicada;
  - lê `sceModuleInfo` e imports/exports, com nomes de NIDs do vita-headers e status de HLE no Vita3K (implementado, stub ou ausente);
  - **validação: imagem relocada idêntica byte a byte ao ELF linkado** (0 divergências);
  - comando: `python -m vitarecomp inspect eboot.bin`.

### 3.8 Tradutor ARMv7 → C (fase 2 do `vitarecomp`)
Pedido: "e continue com o tradutor". Detalhes em `docs/tradutor-arm.md`.
* Cobre **ARM + Thumb-2 + VFPv3 + NEON**, a CPU do Vita (Cortex-A9). O oráculo é o `qemu-arm`, que tem o mesmo ISA.
* **20/20** suítes C (-O0…-Os, com NEON autovetorizado), 4/4 em modo ARM e o raycaster: idênticos.
* **Fuzzing: 620/620 Thumb-2, 420/420 ARM e 460/460 NEON**, comparando o estado completo.
* `tools/lockstep_arm.py` encontra a primeira instrução divergente. Revelou o estado IT perdido e bugs do
  capstone (bit S em `ADC.W`, sinal pós-índice em modo ARM), todos corrigidos.

---

## 4. Commits

| Commit | Conteúdo |
|---|---|
| `8eea696` | Recompilador MIPS32 → C com testes diferenciais e fuzzing |
| `974ca57` | Promoção de registradores, plataforma Vita, raycaster |
| `a3626af` | Execução no Vita3K, documentação e relatório de viabilidade |
| `6dd4544` | `vitarecomp` fase 1 (carregador Vita) e arquitetura do recomp do GoW |
| `46cdb81` | `docs/PROMPT_E_HISTORICO.md` |
| `62be87f` | Tradutor ARMv7 (ARM + Thumb-2 + VFP) → C |
| `3e90fed` | Fuzzer ARM/Thumb/VFP e correções |
| *(este)* | NEON completo para o subconjunto do Cortex-A9 + fuzzer NEON + docs |

O repositório tem 58 arquivos versionados e cerca de 3 mil linhas de código próprio entre Python, C e scripts.

---

## 5. O que ainda não foi feito, e por quê

| Pendência | Bloqueio |
|---|---|
| Analisar o God of War Collection | O PKG está no computador do usuário, fora do alcance desta sessão na nuvem. Também é preciso a **zRIF** (licença) para descriptografar, ou um dump já descriptografado. |
| Testar os formatos compactos de relocação do SDK oficial | Só é possível com o binário do jogo |
| Fase 3: ligar os imports (NIDs) do Vita ao HLE do Vita3K | Próxima etapa |
| Renderizador `sceGxm` → Vulkan | Depende das fases 2 e 3; projetado em `docs/gow-arquitetura.md` |
| Medir em hardware real | Não há Vita físico. O alvo agora é PC, então a medição passa a ser no próprio PC. |
| Persistir o trabalho | O repositório existe só neste container temporário. **É preciso indicar um repositório do GitHub para o push.** |

## 6. Próximos passos sugeridos
1. Informar um repositório do GitHub (pode ser privado) para salvar o projeto.
2. Trazer o jogo, de preferência rodando o Claude Code localmente no PC (WSL2 + Ubuntu 24.04), com o PKG e a zRIF da própria conta, ou um dump NoNpDrm/FAGDec.
3. `vitarecomp inspect eboot.bin`: inventário de imports e cobertura de HLE do jogo.
4. Fase 2 (ARM → C) e fase 3 (runtime + HLE), depois o renderizador Vulkan nativo.
