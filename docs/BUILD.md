# Build: do PKG do Vita ao God of War Collection rodando no PC

Este repositório não traz nada do jogo. Tudo sai do PKG e da licença de quem compila: o código ARM do jogo é
recompilado para C e as DLLs rodam sobre o runtime **vitart**, que implementa as APIs do Vita. Os gráficos usam
Vulkan ou Direct3D 11.

Tudo de uma vez:

```
scripts/build-all.sh <gowHD_PSV.pkg> <work.bin> <pasta do Vita3K.exe> <saída>
```

Abaixo, o que cada etapa faz e como rodar cada uma à mão.

## Requisitos

| O quê | Para quê |
|---|---|
| `gowHD_PSV.pkg` americano (PCSA00126, `UP9000-PCSA00126_00-GOWCUS030100SHIP`) | o jogo |
| `work.bin` da sua conta (licença RIF de 512 bytes) | decifrar o PFS e os SELFs |
| Vita3K em **modo portátil** (há `scripts/build-vita3k-win.cmd`) | instalar o PKG decifrando o PFS |
| Fonte do Vita3K (só na 1ª vez) | importar as chaves de SELF do sistema (não ficam no repositório) |
| LLVM/clang (Windows) | compilar o C gerado, o vitart e a libatrac9 |
| Vulkan SDK | cabeçalhos, `vulkan-1.lib` e o `spirv-cross` (shaders do Direct3D 11) |
| Python 3 com `capstone`, `numpy`, `Pillow` | recompilador e ferramentas |
| Git Bash | os scripts `.sh` |

Hashes do PKG conferido: SHA-256 `e00a3198376e2e1f99c70786f7d5454a8b9bdf3f8021aaa503e2199e95547417`, 3.336.914.560
bytes (`docs/gow-extracao-e-cobertura.md`).

## 1. Chaves de SELF

```
python -m vitarecomp keys-import <fonte do Vita3K>/vita3k/packages/src/sce_utils.cpp
```

Grava `~/.vitarecomp/keys.json`, ou o caminho de `VITARECOMP_KEYS`. São as chaves que o próprio Vita3K usa no
`decrypt_fself`; o `selfdec.py` é um porte desse código.

## 2. Instalar o PKG

```
python tools/zrif.py encode work.bin          # imprime o zRIF
Vita3K.exe --pkg gowHD_PSV.pkg --zrif <zRIF>
```

O Vita3K tira a camada do PKG e a do PFS. O app fica em `<Vita3K>/portable/fs/ux0/app/PCSA00126`:
- `eboot.bin`, `gow1.self` e `gow2.self`;
- `sce_module/*.suprx`;
- `exec/*.psarc`, os dados do jogo.

O `tools/vitapkg.py` lê e extrai só a camada externa do PKG, para conferir o conteúdo.

## 3. SELF -> ELF

```
python -m vitarecomp decrypt <app>/gow1.self --rif work.bin -o elf/gow1.elf
```

Repita para `gow2.self`, `eboot.bin` (o menu da coletânea), `sce_module/libc.suprx` e `sce_module/libfios2.suprx`.
`python -m vitarecomp inspect elf/gow1.elf` mostra os módulos, importações e exportações.

## 4. Recompilação ARM -> C -> DLL

Os planos de link (`docs/gow1-plan.json`, `gow2-plan.json`, `eboot-plan.json`) dão a base de cada módulo:
- jogo em 0x81000000;
- libc em 0x82000000;
- libfios2 em 0x82100000;
- variáveis do runtime em 0x80F00000.

Os ELFs são procurados em `elf/`, ao lado do plano.

```
scripts/build-gow-dlls.sh gow1-plan.json dll gow1 libc libfios2
scripts/build-gow-dlls.sh gow2-plan.json dll gow2
scripts/build-gow-dlls.sh eboot-plan.json dll eboot
```

Para cada módulo:
1. `python -m vitarecomp link` resolve as importações (`link.json`) e lista as variáveis que o runtime cria
   (`link.hlevars.txt`).
2. `python -m vitarecomp recomp` traduz cada função ARM/Thumb para C (`vitarecomp/`, `docs/tradutor-arm.md`).
3. O clang compila os C em paralelo (`JOBS`, padrão 4) e liga a DLL com `runtime/arm/arc_runtime.c`.

## 5. vitart.exe

O runtime está em `runtime/vitart`:
- kernel, E/S e FIOS;
- GXM, com os renderizadores Vulkan e Direct3D 11;
- áudio NGS/ATRAC9, entrada e troféus.

O código específico do God of War fica em `games/gow`, ligado pela interface `runtime/vitart/game.h`.

```
clang++ -std=c++20 -O2 -I runtime/arm -I runtime/vitart -I third_party/libatrac9/src -I third_party/stb
        -I $VULKAN_SDK/Include runtime/vitart/*.cpp games/gow/*.cpp <objetos da libatrac9>
        $VULKAN_SDK/Lib/vulkan-1.lib -Xlinker /DELAYLOAD:vulkan-1.dll -ldelayimp -o vitart.exe
```

O `vulkan-1.dll` é carregado só se existir: sem Vulkan, o vitart usa o Direct3D 11 (`docs/renderizadores.md`).
`scripts/run-gow-vitart.sh` compila e já roda um teste.

## 6. Shaders

Os programas de shader do Vita (GXP, USSE) são traduzidos offline. O vitart não roda Python.

```
python tools/usse_build_cache.py rt/spv rt/spv/miss --d3d11
```

O tradutor `vitarecomp/usse` gera os arquivos:
- `<hash>.spv` (Vulkan);
- `<hash>.dx11` (via HLSL e DXBC);
- `<hash>.pbr.*` (variantes de PBR).

`spv/miss/` traz os 4.746 programas já capturados. Um programa que aparecer sem tradução é gravado pelo vitart em
`spv/miss` e entra na próxima rodada.

## 7. Menu de opções do PC

```
python tools/gow_menu_mod.py <app>/exec/gow2_psp2_SCEA.psarc vitart-data/mods/gow2_psp2_SCEA.psarc.vtpatch
```

Repita para o GoW1. O patch acrescenta as opções do PC à página Options do menu de pausa: resolução, FPS, VSync,
modo de janela, texturas, áudio do PS2, cheats, mods e barras de vida. O psarc original não é tocado: o vitart
aplica o patch na leitura (`docs/modding/README.md`, seção 9).

## Rodar

```
vitart.exe --app <app> --data <vitart-data> --hle-vars link.hlevars.txt eboot.dll libc.dll libfios2.dll
```

Use `VITART_VULKAN=1` e `VITART_SPV_DIR=spv`. O `build-all.sh` deixa pronto o `God of War Collection.cmd`. O
renderizador se escolhe com `renderer=auto|vulkan|d3d11` no `vitart_options.ini`.

## Opcionais (dependem de arquivos que não são deste repositório)

| O quê | Ferramenta | Entrada |
|---|---|---|
| Pack de texturas em HD | `tools/texpack_build.py` | ISO do PS2 + pack de texturas do PCSX2 |
| Materiais PBR | `tools/pbr_generate.py` | o pack de texturas |
| Áudio do PS2 | `tools/ps2_audio_extract.py` | ISO do GoW1 (ou GoW2) de PS2 |
| Mods de PS2 | Mod Studio, aba "Converter mod do PS2" | patch XDelta do mod + ISO ([conversor](modding/conversor-ps2.md)) |
| Troféus e Mod Studio em exe | `tools/build_exes.py` | — |
