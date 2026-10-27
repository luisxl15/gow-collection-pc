# Modding do God of War Collection no vitart

Guia do kit de mods do vitart: como usar mods, converter mods feitos para o PS2 e criar mods novos. Os formatos
por dentro estão em [formatos.md](formatos.md).

## Sumário

1. [Visão geral](#1-visão-geral)
2. [Mod Studio (a interface gráfica)](#2-mod-studio-a-interface-gráfica)
3. [Usar mods](#3-usar-mods)
4. [Converter um mod do PS2](#4-converter-um-mod-do-ps2)
5. [Criar um mod de texturas](#5-criar-um-mod-de-texturas)
6. [Criar uma tradução](#6-criar-uma-tradução)
7. [Estrutura de um mod](#7-estrutura-de-um-mod)
8. [Linha de comando](#8-linha-de-comando)
9. [Como o carregador funciona](#9-como-o-carregador-funciona)
10. [Problemas comuns](#10-problemas-comuns)

## 1. Visão geral

O jogo do Vita guarda tudo em dois arquivos `.psarc` (`exec/gow1_psp2_SCEA.psarc` e `exec/gow2_psp2_SCEA.psarc`).
Dentro deles, cada fase e cada personagem é um **WAD** (`wad/r_hero01.wad_psp2`, por exemplo), uma lista de
registros com texturas, modelos, materiais, animações, scripts e a interface (FLP).

O vitart nunca altera o psarc. Ele monta as trocas na hora de ler:

- **Patch do vitart** (`vitart-data/mods/<psarc>.vtpatch`): o menu de pausa com as opções do PC, os cheats, os ícones de
  teclado e controle e a página Mods. É gerado pelo próprio kit (Reconstruir).
- **Mods** (`vitart-data/mods/<id>/`): cada mod troca arquivos inteiros do psarc (WADs, normalmente) ou registros dentro
  de um WAD (uma tradução troca só os textos). Vários mods podem ficar ligados ao mesmo tempo; se dois trocarem o mesmo
  arquivo, vale o que estiver mais abaixo na lista.

Mods de arquivo inteiro ligam e desligam **com o jogo rodando** (menu de pausa > Options > Mods > Apply). O que o jogo só
lê ao abrir (o `r_perma` com o HUD, o menu de pausa e os textos; o `r_shella` com o menu principal) e os mods de
registros (traduções) valem na próxima vez que o jogo abrir.

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
| **Textos / Tradução** | Exportar os textos do jogo para traduzir e instalar o `.txt` traduzido como mod |
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
**Apply**: a troca vale na hora, sem fechar o jogo.

- Os arquivos de fase, inimigos e efeitos mudam na próxima vez que o jogo os carregar.
- Se o mod troca o **Kratos** (`r_hero*`), o HUD ou os menus (`r_perma`, `r_shella`), o jogo **reinicia sozinho** com
  tudo aplicado de uma vez. Salve antes: o progresso desde o último save se perde.

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
  o que fica atrás (veja [Problemas comuns](#10-problemas-comuns)).
- O **pack de texturas HD** do vitart troca as texturas originais pelo conteúdo. Uma textura trocada por um mod tem
  conteúdo novo, então o pack não a substitui e vale a do mod.

## 6. Criar uma tradução

Os textos do jogo (menus, dicas, tutoriais, legendas da interface) ficam no registro `MSGS_TXT` do
`wad/_VitaTV/r_perma.wad_psp2`. Uma tradução é um mod que troca só esse registro.

1. Aba **Textos / Tradução** > **Exportar os textos originais...**: salva o `MSGS_TXT` em `.txt` (UTF-8).
2. Traduza o arquivo:
   - Cada mensagem começa numa linha `*N*` (o número não muda) e pode ter várias linhas.
   - Mantenha as marcas entre colchetes (`[XButton]`, `[JoystickL]`, `[PS2_CyclopsEye_Count]`...): viram ícones ou
     números no jogo.
   - Linhas que começam com `//` são comentários.
3. Escolha o `.txt` traduzido, dê um nome e **Instalar tradução**. Ligue o mod e abra o jogo de novo.

A **fonte do jogo** tem as letras acentuadas minúsculas (á à â ä ç é ê í ó ô ö ú) e o Í. Ela usa os códigos das outras
maiúsculas acentuadas (Á Â Ä Ç É Ê Ó Ô Ú) para os **ícones dos botões**, e não tem ã, õ, Ã nem Õ. O kit troca
sozinho as maiúsculas acentuadas pela letra sem acento e ã/õ por ä/ö. O registro mostra quantas letras trocou.

O jogo dimensiona as tabelas de mensagens por dois parâmetros do WAD, `MSGS_COUNT` e `MSGS_LINES`. O kit conta as
mensagens e as linhas do texto novo do mesmo jeito que o jogo (`0x81019f7e` no GoW2): pula linhas vazias e `//`,
e `*N*` abre uma mensagem. Os dois parâmetros só crescem.

Pela linha de comando:

```bash
python tools/vitart_modkit.py text-mod gow2 traducao_ptbr "Tradução PT-BR" portugues_MSGS_TXT.txt
```

Um mod de tradução é aplicado por cima da versão do `r_perma` que vale ao abrir o jogo. Ele funciona junto com um mod
que também troca o `r_perma`, como o GoW3 Kratos.

## 7. Estrutura de um mod

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
    records.tsv                          (opcional) troca de registros dentro de um WAD
    records/MSGS_TXT.txt                 (o corpo de um registro trocado)
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

**records.tsv**: troca registros dentro de um WAD em vez do arquivo inteiro. Uma linha por registro:
`<psarc>\t<entrada>\t<tag>\t<registro>\t<ocorrência>\t<arquivo>` ou `...\t=<valor>` para parâmetros (tag 0, em que o
"tamanho" do registro é o valor). Exemplo da tradução:

```
gow2_psp2_SCEA.psarc	12	8	MSGS_TXT	0	records/MSGS_TXT.txt
gow2_psp2_SCEA.psarc	12	0	MSGS_COUNT	0	=802
gow2_psp2_SCEA.psarc	12	0	MSGS_LINES	0	=2009
```

**Distribuir um mod:** compacte a pasta do mod (sem precisar do entries.tsv). Quem recebe copia para
`vitart-data/mods/` e clica em Reconstruir. Distribua só o que você fez: o mod contém os WADs inteiros, então inclui
dados do jogo. Para mods públicos, prefira distribuir as ferramentas e as imagens, ou a fonte do mod (como os mods do
PS2 fazem com XDelta).

## 8. Linha de comando

```bash
python tools/vitart_modkit.py list
python tools/vitart_modkit.py enable meu_mod
python tools/vitart_modkit.py disable meu_mod
python tools/vitart_modkit.py rebuild
python tools/vitart_modkit.py convert-ps2 gow2 "mod.zip" "God of War II (USA).iso" meu_mod "Meu Mod" --author Fulano
python tools/vitart_modkit.py textures gow2 wad/_VitaTV/r_hero01.wad_psp2
python tools/vitart_modkit.py export-tex gow2 wad/_VitaTV/r_hero01.wad_psp2 "~000kratos01_1~pal_krat" kratos.png
python tools/vitart_modkit.py texture-mod gow2 kratos_azul "Kratos Azul" "wad/r_hero01.wad_psp2=~000kratos01_1~pal_krat=kratos.png"
python tools/vitart_modkit.py text-mod gow2 traducao_ptbr "Tradução PT-BR" portugues_MSGS_TXT.txt
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

## 9. Como o carregador funciona

No runtime (`runtime/vitart/hle_io.cpp`, `psarc_patch`), quando o jogo abre o psarc:

1. Carrega o patch do vitart (`<psarc>.vtpatch`): a TOC remendada e os dados novos que ficam "depois do fim" do psarc.
2. Reserva os arquivos de **todos os mods instalados**, ligados ou não, na ordem do `modlist.tsv` (`entries.tsv`):
   blocos novos no fim da tabela de blocos (sem compressão) e os dados no fim dos extras. A TOC aponta só os dos mods
   ligados (`mod_<id>=1` no `vitart_options.ini`).
3. Aplica os `records.tsv` dos mods ligados por cima da versão de cada WAD que vale agora (a base ou a de um mod
   ligado). O WAD montado fica sempre apontado.
4. Acrescenta entradas sem nome (MD5 que nenhum caminho dá) apontando para cada arquivo reservado e para o original que
   ele troca. Ao montar o psarc, o FIOS do jogo só prepara os blocos que alguma entrada usa; com essas entradas, os dois
   lados ficam prontos para a troca a quente.
5. Se a TOC cresceu por cima de dados do psarc (o manifesto vem logo depois dela), esses dados são copiados para os extras.

O jogo continua lendo o psarc normalmente; o vitart junta TOC remendada, psarc original e extras na leitura.

**Troca a quente** (`mods_hot_apply`). O FIOS lê a TOC uma vez, guarda na memória do jogo e **ordena as entradas pelo
MD5 do nome** (a do manifesto fica na frente; os campos continuam big-endian). O Apply recalcula a TOC com os mods
ligados e, para cada entrada que mudou, acha a cópia do jogo pelo MD5 e troca os 14 bytes de bloco, tamanho e
offset. Se o Kratos, o HUD ou os menus mudaram, o vitart relança o próprio processo com a mesma linha de comando
(`games/gow/game_options.cpp: restart_process`).

Já foram testados dois caminhos sem reiniciar, e os dois foram abandonados:
- **Só os comandos de herói (12/13/25).** Recriam o Kratos sem posição nem câmera.
- **Comando 19 ("Restart from last checkpoint", `0x810219c2`)**, com o herói atual (`[0x812eb030]+4`) e o último
  pedido (`0x812cb9a4`) marcados como -1. Recarrega o `R_Hero`, mas volta ao checkpoint, às vezes o começo da fase.

A página **Mods** do menu de pausa é gerada pelo `gow_menu_mod.py --modlist` com uma linha por mod. Para mostrar o estado de
cada mod, o script do menu faz uma consulta síncrona ao vitart (`fscommand VT_modq_1`): o vitart põe no campo do estado um bit
por mod ligado, o script lê na hora e devolve (`VT_modq_2`). Ligar um mod manda `VT_moda_2` (a..p = mods 0 a 15), e
**Apply** manda `VT_restart_2`, que salva as opções e faz a troca a quente.

## 10. Problemas comuns

| Sintoma | Causa e solução |
|---|---|
| O mod não aparece no menu de pausa | Faltou **Reconstruir** depois de instalar ou copiar o mod |
| Ligado mas nada muda | Use **Apply** no menu. HUD, menus e traduções só valem ao abrir o jogo de novo |
| Letra acentuada vira ícone de botão | A fonte do jogo usa as maiúsculas acentuadas para os ícones. Instale a tradução pelo kit, que troca essas letras |
| "não existe no psarc" no Reconstruir | O caminho em `files/` está errado (é o caminho dentro do psarc, ex.: `wad/_VitaTV/r_hero01.wad_psp2`) |
| Peças transparentes escondem o que está atrás (ex.: uma saia que some com a perna) | Ordem de desenho: o conversor desenha por último os objetos com textura transparente (`gow2_mod_wad.py: gmdl_record`). Em mods de textura, evite alfa em materiais opacos |
| A conversão falha com "xdelta3 falhou" | ISO diferente da que o mod pede (região/versão) ou xdelta3 errado |
| Pouco espaço em disco | A conversão precisa de espaço para os WADs mudados (e para a ISO, se vier em `.7z`) |

Teste sem arriscar a instalação: `VITART_MODS_DIR=<outra pasta>` faz o vitart usar outra pasta de mods.
