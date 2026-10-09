# God of War Collection (PS Vita) → PC nativo: arquitetura do recomp

*Alvo: God of War Collection (PS Vita, 2014; GoW I e II). Plataforma: PC
(Linux/Windows). Estado: infraestrutura em construção, sem o dump do jogo ainda.*

## 1. Visão geral

```
eboot.bin (+ módulos .suprx do jogo)  ← dump descriptografado do próprio usuário
      │  vitarecomp (offline)
      ├─ carregador SCE: segmentos, relocações, imports/exports por NID   [feito]
      ├─ descoberta de funções ARM/Thumb (exports, relocações, BL/BLX, EXIDX)
      ├─ ARMv7 (ARM + Thumb-2 + VFPv3 + NEON) → C
      └─ shaders GXP do jogo → SPIR-V (tradutor do Vita3K, offline)
      ▼
gow_recomp.exe
  ├─ código do jogo recompilado (C → x86-64 / AArch64)
  ├─ runtime: memória do guest, contexto de CPU, despacho indireto, threads
  ├─ HLE do sistema (SceLibKernel, SceThreadmgr, SceIofilemgr, SceCtrl, SceAudio…)
  │     reaproveitando a implementação do Vita3K como biblioteca
  └─ renderizador Vulkan nativo: sceGxm* → Vulkan direto
```

## 2. Decisões e porquês

| Decisão | Motivo |
|---|---|
| **AOT recompilação para C** (mesmo modelo do `mipsrecomp`) | Já validado aqui: fidelidade bit a bit com fuzzing e lockstep. O C é portável (x86-64, AArch64, Windows, Linux). |
| **Fallback para interpretação/JIT** onde a análise estática não alcança | Jogos comerciais têm ponteiros de função, vtables C++ e possivelmente código gerado. O dynarmic (já integrado ao Vita3K) cobre o que não foi recompilado e registra o endereço para a próxima rodada de recompilação. |
| **HLE do Vita3K como biblioteca** | O Vita3K já roda o jogo e implementa centenas de NIDs. Reescrever isso seria anos de trabalho. O recomp troca só a CPU (e, por fim, a GPU). |
| **Renderizador Vulkan nativo (sceGxm → Vulkan)** | O `sceGxm` já é uma API explícita: render targets, programas de vértice e fragmento, uniform buffers, depth/stencil, sync objects. Dá para mapear chamada por chamada para Vulkan, sem emular a GPU PowerVR. Isso permite resolução interna maior, widescreen, MSAA e fps desbloqueado. |
| **Shaders GXP → SPIR-V offline** | O Vita3K tem o tradutor (`Vita3K --recompile-shader`). Os shaders do jogo podem ser extraídos e compilados uma vez no build, o que elimina travadas de compilação durante o jogo. |
| **Oráculo = Vita3K** | É o papel que o QEMU teve no `mipsrecomp`: comparar estado e quadros entre o jogo no emulador e a versão recompilada. |

## 3. Renderizador Vulkan nativo

O jogo chama `sceGxm*` (importações por NID). No recomp, essas chamadas viram
chamadas C para o nosso backend:

| sceGxm | Vulkan |
|---|---|
| `sceGxmCreateRenderTarget`, `sceGxmColorSurfaceInit`, `sceGxmDepthStencilSurfaceInit` | `VkImage` + `VkImageView` + `VkFramebuffer`/dynamic rendering; resolução escalável |
| `sceGxmBeginScene` / `sceGxmEndScene` | `vkCmdBeginRendering` / `vkCmdEndRendering` |
| `sceGxmShaderPatcherCreateVertexProgram/FragmentProgram` | `VkPipeline` (vertex input + blend + SPIR-V pré-compilado), cache por hash |
| `sceGxmSetVertexStream`, `sceGxmSetUniformDataF`, `sceGxmReserve*DefaultUniformBuffer` | vertex buffers e UBOs em ring buffer por quadro |
| `sceGxmSetFragmentTexture` | `VkImageView` + sampler; formatos com swizzle/tiling convertidos no upload (cache por endereço e hash) |
| `sceGxmDraw` | `vkCmdDrawIndexed` |
| `sceGxmSetFrontDepthFunc`, stencil, culling, `sceGxmSetRegionClip` | estado dinâmico (`VK_EXT_extended_dynamic_state`) |
| `sceGxmDisplayQueueAddEntry` / `sceDisplaySetFrameBuf` | blit para a swapchain |
| `sceGxmTransfer*` | `vkCmdCopyImage`/compute |

A parte difícil é a **coerência de memória**. No Vita, CPU e GPU compartilham a memória, e o jogo
pode escrever direto em texturas e vertex buffers. Estratégia: rastrear regiões
mapeadas (`sceGxmMapMemory`) e sincronizar por *dirty tracking* e hash, como o Vita3K
já faz no backend Vulkan dele, que serve de referência.

## 4. Fases e critérios de saída

| Fase | Entrega | Critério |
|---|---|---|
| 1 | Carregador de módulos SCE | **Feito**: imagem relocada igual byte a byte ao ELF linkado; imports com nome e status HLE |
| 2 | Recompilador ARM/Thumb → C (inteiro + VFP), com homebrews de teste | Saída e estado idênticos ao Vita3K/dynarmic nas homebrews; fuzzing de instruções ARM |
| 3 | Runtime + HLE do Vita3K ligado ao código recompilado | Homebrew recompilada roda como `.exe` com a mesma saída que no Vita3K |
| 4 | NEON, exclusões (LDREX/STREX), threads | Fuzzing NEON; testes multithread |
| 5 | Dump do GoW: inventário (`vitarecomp inspect`), cobertura de funções, fallback | Jogo chega ao menu com o fallback registrando o que falta |
| 6 | Renderizador Vulkan nativo | Quadros comparáveis (por hash/SSIM) ao Vita3K nas mesmas cenas |
| 7 | Melhorias (resolução, widescreen, 60 fps) | Opcionais, depois da fidelidade |

## 5. Pré-requisitos e limites legais

* É preciso um **dump descriptografado da cópia do próprio usuário**: o `eboot.bin` do jogo
  (SELF com segmentos em claro, por exemplo via NoNpDrm/FAGDec) e os dados. O projeto não
  distribui código nem dados do jogo. O recomp gera C a partir do binário do usuário
  na máquina dele, como fazem N64Recomp/Zelda64Recomp.
* O carregador rejeita segmentos ainda criptografados, com mensagem explícita.
