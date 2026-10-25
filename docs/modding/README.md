# Modding do God of War Collection no vitart

Guia do kit de mods do vitart: como usar mods, converter mods feitos para o PS2 e criar mods novos. Os formatos
por dentro estão em [formatos.md](formatos.md).

## Sumário

1. [Visão geral](#1-visão-geral)
2. [Mod Studio (a interface gráfica)](#2-mod-studio-a-interface-gráfica)
3. [Usar mods](#3-usar-mods)
4. [Converter um mod do PS2](#4-converter-um-mod-do-ps2)
5. [Criar um mod de texturas](#5-criar-um-mod-de-texturas)
6. [Estrutura de um mod](#6-estrutura-de-um-mod)
7. [Linha de comando](#7-linha-de-comando)
8. [Como o carregador funciona](#8-como-o-carregador-funciona)
9. [Problemas comuns](#9-problemas-comuns)

## 1. Visão geral

O jogo do Vita guarda tudo em dois arquivos `.psarc` (`exec/gow1_psp2_SCEA.psarc` e `exec/gow2_psp2_SCEA.psarc`).
Dentro deles, cada fase e cada personagem é um **WAD** (`wad/r_hero01.wad_psp2`, por exemplo), uma lista de
registros com texturas, modelos, materiais, animações, scripts e a interface (FLP).

O vitart nunca altera o psarc. Ele monta as trocas na hora de ler:

- **Patch do vitart** (`vitart-data/mods/<psarc>.vtpatch`): o menu de pausa com as opções do PC, os cheats, os ícones de
  teclado e controle e a página Mods. É gerado pelo próprio kit (Reconstruir).
- **Mods** (`vitart-data/mods/<id>/`): cada mod troca arquivos inteiros do psarc (WADs, normalmente). Vários mods podem
  ficar ligados ao mesmo tempo; se dois trocarem o mesmo arquivo, vale o que estiver mais abaixo na lista.

Os mods são aplicados quando o jogo abre: ligar ou desligar pede reiniciar (o menu de pausa faz isso sozinho).

**Versão PS TV.** Cada WAD do jogo existe duas vezes: `wad/` (Vita portátil) e `wad/_VitaTV/` (PS TV, com os textos e ícones
para controle). O vitart roda no modo PS TV, então é o `_VitaTV/` que conta. As ferramentas sempre geram as duas
versões.

## 2. Mod Studio (a interface gráfica)

```bash
python tools/vitart_modstudio.py
```

Ou o atalho `vitart Mod Studio.cmd` na pasta do vitart. Precisa de Python 3 com Pillow e numpy
(`pip install pillow numpy`).

| Aba | Para quê |
|---|---|
| **Mods** | Lista dos mods instalados: ligar/desligar (duplo clique), ordem, abrir a pasta, remover, reconstruir |
| **Converter mod do PS2** | Patch XDelta de um mod do PS2 + ISO original -> mod do vitart, instalado |
| **Texturas** | Ver as texturas de qualquer WAD, exportar PNG, trocar por PNG e salvar as trocas como mod |
| **Configurações** | Pastas do jogo e do vitart-data, xdelta3, 7-Zip |
| **Ajuda** | Este guia |

Na primeira vez, confira a aba Configurações:

- **Pasta do jogo**: a que tem `exec/gow2_psp2_SCEA.psarc` (no projeto: `_deps/vita3k-win/.../app/PCSA00126`).
- **vitart-data**: a pasta de dados do vitart (a mesma do `--data` do atalho do jogo).
- **xdelta3**: só para converter mods do PS2. O oficial é o `xdelta3-3.1.0-x86_64.exe` de
  github.com/jmacd/xdelta-gpl/releases. O kit procura em `tools/bin/xdelta3.exe`.
- **7-Zip**: só se a ISO estiver dentro de um `.7z`.

## 3. Usar mods

**No jogo:** menu de pausa > **Options > Mods**. Cada mod instalado tem uma linha **On/Off**. Depois de escolher, use
**Apply & Restart**: o vitart salva, fecha e abre de novo na tela de título (continue do último save).

**No Mod Studio:** aba Mods, duplo clique para ligar/desligar. Vale na próxima vez que o jogo abrir.

- **Ordem:** os botões Subir/Descer mudam a prioridade (o de baixo ganha) e a ordem na página Mods. Depois de mudar,
  **Reconstruir** (o menu do jogo é gerado com a lista).
- **Remover:** a pasta vai para `vitart-data/mods_removidos/` (nada é apagado).
- **Instalar um mod recebido de outra pessoa:** copie a pasta dele para `vitart-data/mods/` e clique em **Reconstruir**.
- O menu mostra até 16 mods por jogo.

## 4. Converter um mod do PS2

Muitos mods do God of War II do PS2 são distribuídos como **patch XDelta** da ISO americana (SCUS-974.81), para não
distribuir o jogo. O kit aplica o patch e converte o que mudou para o formato do Vita.

1. Tenha a **ISO original** do PS2 (a mesma que o mod pede; um `.7z` com ela também serve).
2. No Mod Studio, aba **Converter mod do PS2**: escolha o patch (`.xdelta` solto ou o `.zip` do mod, sem extrair), a
   ISO, dê um **nome** (aparece no menu do jogo) e, se quiser, autor e descrição.
3. **Converter e instalar.** Leva alguns minutos. A ISO com o patch nunca é gravada: o patch é lido em fluxo e só os
   arquivos que mudam são guardados (é preciso espaço só para eles e, se a ISO estiver num `.7z`, para a ISO extraída,
   apagada no fim).
4. Ligue o mod (aba Mods ou no jogo).

O que é convertido:

| No PS2 | No Vita |
|---|---|
| Texturas (GFX + PAL, 4/8 bits) | BC3 com mipmaps, swizzle do Vita |
| Malhas (`MDL_*_0`, pacotes VIF) | GMDL (`~MD_*`, buffers de vértice), com o mesmo esqueleto |
| Materiais, animações, scripts, efeitos, esqueleto | iguais (o Vita usa os mesmos registros do PS2) |
| Vídeos (`.PSS`, `.PSW`) | ignorados (o Vita tem os próprios) |

Limitações:

- **Só o GoW2.** O GoW1 do PS2 usa outro formato de malha; o conversor dele ainda não existe (as texturas do GoW1 já
  funcionam pelo editor de texturas).
- Os textos (`MSGS_TXT`) do mod não entram: o Vita tem os dele (com os textos do PS TV).
- Se o mod mudar arquivos que o Vita também mudou (o `r_perma` e o `r_shella` têm registros só do Vita), entra só o que
  o mod alterou em relação ao PS2 original.

Exemplo: o **GoW3 Kratos (PS2 Edition)** (Nexus Mods, godofwar2/mods/11) foi convertido assim:
[mod-gow3-ps2-edition.md](../mod-gow3-ps2-edition.md).

## 5. Criar um mod de texturas

1. Aba **Texturas**: escolha o jogo, **Carregar lista de WADs** (o filtro ajuda: `hero` para o Kratos, `perma` para o
   HUD e os itens, `shell` para o menu principal).
2. Clique num WAD: aparecem as texturas dele (nome do registro, tamanho, formato) e a prévia.
3. **Exportar PNG** (ou **Exportar todas do WAD**), edite no seu programa de imagem e **Trocar por PNG**. Pode trocar
   várias texturas de vários WADs.
4. Em **Salvar as trocas como mod**, dê um nome e **Criar mod**.

Dicas:

- Lados em **potência de 2** (64, 128, 256, 512, 1024...). Outros tamanhos são ajustados.
- Pode aumentar a resolução (uma textura 256x256 pode virar 1024x1024): o modelo usa coordenadas de 0 a 1.
- A transparência (alfa) do PNG vale. Atenção: partes transparentes num material que grava profundidade podem esconder
  o que fica atrás (veja [Problemas comuns](#9-problemas-comuns)).
- O **pack de texturas HD** do vitart troca as texturas originais pelo conteúdo. Uma textura trocada por um mod tem
  conteúdo novo, então o pack não a substitui e vale a do mod.

## 6. Estrutura de um mod

```
vitart-data/mods/
  modlist.tsv                  ordem dos mods: "<jogo>\t<índice no menu>\t<id>\t<nome>" (gerado)
  gow2_psp2_SCEA.psarc.vtpatch patch do vitart (gerado pelo Reconstruir)
  meu_mod/
    mod.ini
    files/wad/r_hero01.wad_psp2          arquivos que substituem os do jogo (o caminho é o do psarc)
    files/wad/_VitaTV/r_hero01.wad_psp2
    src/wad/r_perma.wad_psp2             (só se o mod troca r_perma/r_shella; veja abaixo)
    entries.tsv                          gerado pelo Reconstruir
```

**mod.ini**

```ini
name=Meu Mod
game=gow2
author=Fulano
version=1.0
description=O que o mod faz
```

- `name`: aparece no menu de pausa (até 24 caracteres) e no Mod Studio.
- `game`: `gow1` ou `gow2`.

**src/**: o `r_perma` (HUD, menu de pausa) e o `r_shella` (menu principal) também são mudados pelo patch do vitart. Um
mod que troca esses WADs guarda a versão dele em `src/`; o Reconstruir aplica por cima o menu e os ícones do vitart e
grava o resultado em `files/`.

**entries.tsv**: o que o vitart lê. Uma linha por arquivo: `<psarc>\t<número da entrada na TOC>\tfiles/<caminho>`.
Gerado pelo Reconstruir a partir de `files/`.

**Distribuir um mod:** compacte a pasta do mod (sem precisar do entries.tsv). Quem recebe copia para
`vitart-data/mods/` e clica em Reconstruir. Distribua só o que você fez: o mod contém os WADs inteiros, então inclui
dados do jogo. Para mods públicos, prefira distribuir as ferramentas e as imagens, ou a fonte do mod (como os mods do
PS2 fazem com XDelta).

## 7. Linha de comando

```bash
python tools/vitart_modkit.py list
python tools/vitart_modkit.py enable meu_mod
python tools/vitart_modkit.py disable meu_mod
python tools/vitart_modkit.py rebuild
python tools/vitart_modkit.py convert-ps2 gow2 "mod.zip" "God of War II (USA).iso" meu_mod "Meu Mod" --author Fulano
python tools/vitart_modkit.py textures gow2 wad/_VitaTV/r_hero01.wad_psp2
python tools/vitart_modkit.py export-tex gow2 wad/_VitaTV/r_hero01.wad_psp2 "~000kratos01_1~pal_krat" kratos.png
python tools/vitart_modkit.py texture-mod gow2 kratos_azul "Kratos Azul" "wad/r_hero01.wad_psp2=~000kratos01_1~pal_krat=kratos.png"
```

Ferramentas de baixo nível (usadas pelo kit):

| Ferramenta | Faz |
|---|---|
| `tools/ps2_xdelta_wads.py` | aplica um xdelta numa ISO do PS2 em fluxo e guarda só os arquivos do TOC que mudam |
| `tools/gow2_mod_wad.py` | WAD do GoW2 de PS2 -> WAD do Vita (`--all`, `--merge`) |
| `tools/ps2_mesh.py` | lê a malha de PS2 (partes, LODs, objetos, pacotes VIF) |
| `tools/gmdl.py` | lê o GMDL do Vita |
| `tools/gow_menu_mod.py` | gera o patch do vitart (menu de pausa, opções, cheats, página Mods, ícones) |
| `tools/texpack_ps2.py`, `tools/psarc_gxp.py` | ISO/TOC do PS2, psarc do Vita |

## 8. Como o carregador funciona

No runtime (`runtime/vitart/hle_io.cpp`, `psarc_patch`), quando o jogo abre o psarc:

1. Carrega o patch do vitart (`<psarc>.vtpatch`): a TOC remendada e os dados novos que ficam "depois do fim" do psarc.
2. Para cada mod ligado (`mod_<id>=1` no `vitart_options.ini`), na ordem do `modlist.tsv`, lê o `entries.tsv` e troca cada
   entrada da TOC: entradas novas no fim da tabela de blocos (blocos sem compressão) e os dados no fim dos extras.
3. Se a TOC cresceu por cima de dados do psarc (o manifesto vem logo depois dela), esses dados são copiados para os extras.

O jogo continua lendo o psarc normalmente; o vitart junta TOC remendada, psarc original e extras na leitura.

A página **Mods** do menu de pausa é gerada pelo `gow_menu_mod.py --modlist` com uma linha por mod. Para mostrar o estado de
cada mod, o script do menu faz uma consulta síncrona ao vitart (`fscommand VT_modq_1`): o vitart põe no campo do estado um bit
por mod ligado, o script lê na hora e devolve (`VT_modq_2`). Ligar um mod manda `VT_moda_2` (a..p = mods 0 a 15), e
**Apply & Restart** manda `VT_restart_2`, que salva as opções e reabre o vitart (`game_options.cpp: restart_process`).

## 9. Problemas comuns

| Sintoma | Causa e solução |
|---|---|
| O mod não aparece no menu de pausa | Faltou **Reconstruir** depois de instalar ou copiar o mod |
| Ligado mas nada muda | Ligar só vale ao abrir o jogo: use **Apply & Restart** ou reabra |
| "não existe no psarc" no Reconstruir | O caminho em `files/` está errado (é o caminho dentro do psarc, ex.: `wad/_VitaTV/r_hero01.wad_psp2`) |
| Peças transparentes escondem o que está atrás (ex.: uma saia que some com a perna) | Ordem de desenho: o conversor desenha por último os objetos com textura transparente (`gow2_mod_wad.py: gmdl_record`). Em mods de textura, evite alfa em materiais opacos |
| A conversão falha com "xdelta3 falhou" | ISO diferente da que o mod pede (região/versão) ou xdelta3 errado |
| Pouco espaço em disco | A conversão precisa de espaço para os WADs mudados (e para a ISO, se vier em `.7z`) |

Teste sem arriscar a instalação: `VITART_MODS_DIR=<outra pasta>` faz o vitart usar outra pasta de mods.
