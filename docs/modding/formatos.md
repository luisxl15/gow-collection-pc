# Formatos (para quem faz ferramentas)

Referência técnica do que o kit lê e escreve. Tudo foi medido no God of War Collection do Vita (PCSA00126) e na ISO
americana do God of War II de PS2 (SCUS-974.81). Valores em little-endian, salvo indicação.

## psarc do Vita

`exec/gow1_psp2_SCEA.psarc`, `exec/gow2_psp2_SCEA.psarc`. Cabeçalho de 32 bytes em **big-endian**: `PSAR`, versão,
compressão (`zlib`), tamanho da TOC, tamanho da entrada (30), número de entradas, tamanho do bloco, flags.

- Entrada (30 bytes): MD5 do nome (16), índice do primeiro bloco (u32), tamanho descomprimido (u40), deslocamento (u40).
- Depois das entradas, a tabela de blocos: u16 se o bloco for de até 64 KB. Valor 0 = bloco cheio sem compressão;
  outro valor = tamanho comprimido (zlib) do bloco.
- A entrada 0 é o manifesto: os nomes, um por linha, na ordem das entradas 1..n.
- Leitura: `tools/psarc_gxp.py` (`entries`, `read_file`).

## vtpatch (patch do vitart)

`vitart-data/mods/<psarc>.vtpatch`: `VTPP`, u64 tamanho do psarc original, u32 tamanho da TOC, u32 tamanho dos extras,
a TOC inteira remendada e os extras. Os extras ficam "depois do fim" do psarc (endereço = tamanho original +
deslocamento). Gerado por `tools/gow_menu_mod.py: build_patch`. O runtime acrescenta os mods por cima
(`runtime/vitart/hle_io.cpp: overlay_file`).

## WAD

Lista de registros: cabeçalho de 32 bytes (u16 tag, u16 flags, u32 tamanho, nome[24]) seguido do corpo, alinhado a 16.

| Jogo | Dados | Sem corpo (o "tamanho" é um valor) |
|---|---|---|
| GoW1 | tag 30 | 24 |
| GoW2 | tag 1 | 0 (parâmetro), 2 (GroupStart), 3 (GroupEnd), 24 |

GoW2: tag 21 = cabeçalho `WAD_...`; tags 11 a 16 = "tweak templates" (parâmetros do jogo; no Vita são de outra
versão do jogo e nunca devem vir do PS2). Um registro de dados com corpo vazio é uma **referência** a um registro com
o mesmo nome (o modelo lista os materiais assim). O primeiro u32 do corpo diz o servidor (tipo):

| Tipo | Servidor | Tipo | Servidor |
|---|---|---|---|
| 0x0c | GFX/PAL (imagem/paleta) | 0x07 | TXR (textura = GFX + PAL) |
| 0x08 | MAT (material) | 0x0002000f | MDL (modelo) |
| 0x0001000f | malha de PS2 (`MDL_<nome>_0`) | 0x0003000f | GMDL (`~MD_<nome>`, só no Vita) |
| 0x00070007 | textura do Vita (`~NNN...`) | 0x00010001 | objeto/esqueleto |
| 0x03 | ANM | 0x04 | SCR / SCP |

### O WAD do Vita é o do PS2 com três diferenças

Medido no GoW2: dos 40 WADs que o mod GoW3 muda, 36 são reproduzidos byte a byte a partir do PS2 original por
`tools/gow2_mod_wad.py` (os outros dois têm registros a mais só do Vita):

1. `GFX_*` e `PAL_*` viram tocos: só os 32 primeiros bytes do corpo (o cabeçalho e 8 bytes de pixels/cores), tamanho 32.
2. Depois de cada `PAL_*` entra a textura convertida `~NNN<gfx>~<pal>` (nome em minúsculas, até 23 caracteres). O
   `TXR_*` aponta para ela no **3º campo de nome** (+0x34); no PS2 esse campo é zero.
3. Depois de cada `MDL_<nome>` entra `~MD_<nome>` (GMDL), a malha que o Vita desenha. A malha de PS2 continua no WAD,
   byte a byte igual.

Todo o resto (materiais, animações, scripts, efeitos, esqueleto, colisão) é idêntico ao PS2.

## Texturas

**PS2** (`GFX_*` + `PAL_*`): cabeçalho u32 0xC, largura, altura, codificação, bits por índice, número de imagens; os
índices (8 bits swizzled do GS ou 4 bits) e a paleta (256 cores em CSM1, com blocos de 8 trocados 0,2,1,3). Alfa do PS2:
0x80 = opaco. Leitura: `tools/texpack_ps2.py` (`gfx_parse`, `palette`, `indexes`), `gow2_mod_wad.ps2_rgba`.

**Vita** (`~NNN...`, tipo 0x00070007): `TXTR`, 6 bytes, e um GXT:

- cabeçalho: `GXT\0`, versão 0x10000003, número de texturas, deslocamento e tamanho dos dados, paletas P4 e P8;
- textura (32 bytes): deslocamento, tamanho, paleta (0xFFFFFFFF = nenhuma), flags, tipo (0 = swizzled), formato
  (0x85000000 BC1, 0x86 BC2, 0x87 BC3, 0x95 P8, 0x0C RGBA8), largura, altura (u16), níveis de mipmap.

Swizzle do Vita: ordem Z (Morton) nos pixels, ou nos blocos 4x4 para BCn; lado maior em faixas de quadrados.
Decodificação: `tools/gxm_tex_dump.py`. Codificação: `gow2_mod_wad.bc3_swizzled` (BC3 pelo Pillow, mips até 1x1).

## Malha de PS2 (GoW2)

`MDL_<nome>_0`, magia 0x0001000f (porte do god_of_war_browser de mogaika, `pack/wad/mesh`): partes -> grupos de LOD
-> objetos. Objeto: tipo, material, mapa de ossos, número de instâncias e de camadas de textura, e cadeias DMA
(REF/RET) com UNPACKs do VIF. Por pacote:

| Dado | UNPACK | Valor |
|---|---|---|
| posição + flags | 16 bits x 4 | xyz / 16; w: bit 15 = pular o triângulo, resto / 4096 = peso |
| UV | 16 ou 32 bits x 2 | / 4096 |
| normal | 8 bits x 3 | / 127 |
| cor | 8 bits x 4 | RGBA (0x80 = 1,0) |
| metadados | 32 bits x 4, alvo 0/0x155/0x2ab | blocos de 16 bytes: vértices, ossos (block[12] >> 2, block[13] >> 4) |

Triângulos: tiras; cada vértice sem a flag de pular fecha (v-2, v-1, v). Leitura: `tools/ps2_mesh.py`.

## GMDL (Vita)

`~MD_<nome>`, tipo 0x0003000f, depois o GMDL (cabeçalho em big-endian): `GMDL`, 1, 1, 1, número de modelos (= partes
da malha de PS2), 1, deslocamentos. Modelo: `MODL`, id, flags (9), deslocamento dos índices; streams (little-endian):

| Stream | Formato | Do PS2 |
|---|---|---|
| POS0 | 4 floats | (x, y, z, 1 - peso) |
| NRM0 | 3 int8 | a normal do PS2 |
| COL0 | 4 half | RGB / 255, alfa x 2 / 255 |
| TEX0 | 2 half | UV |
| BONI | 4 u8 | (osso1 se 1 - peso > 0, osso2 se peso > 0, 0, 0) |

Índices u16 (big-endian), lista de triângulos. Objetos (um por objeto do PS2, do LOD 0): caixa, material, faixa de
vértices e de índices, mapa de ossos em **ordem crescente** (o conversor da Sanzaru ordena e renumera os ossos).
Vértices desduplicados por objeto. Leitura: `tools/gmdl.py`; escrita: `gow2_mod_wad.gmdl_record`.

**Ordem de desenho:** os objetos são desenhados na ordem da lista. Objetos com textura transparente nas UVs que usam
(franjas de roupa) devem vir por último, senão gravam profundidade nos pixels transparentes e escondem o que é
desenhado depois. O conversor mede isso por objeto e reordena.

## ISO do PS2 (GoW2)

`GODOFWAR.TOC`: u32 n, n entradas {nome[24], tamanho, cópias, índice} e uma tabela de setores; setor < 10.000.000 =
`PART1.PAK`, senão `PART2.PAK` (na segunda camada do DVD). Uma ISO de camada única (como a do mod GoW3) tem o PART2.PAK
no mesmo volume. Leitura: `tools/texpack_ps2.py: toc_any`, `tools/ps2_xdelta_wads.py: toc_new`.

## Menu de pausa (FLP) e opções

O patch do vitart reescreve o FLP do HUD (`FLP_HUD`/`FLP_HUDA` no `r_perma`). Estado das opções numa palavra só
(`PS2_Cheat` no GoW1, `PS2_OverUnder` no GoW2): textura | áudio << 2 | resolução << 4 | fps << 7 | vsync << 9 | QTE << 10 |
contador << 11 | modo << 12 | cheats << 13..15 | FPS medidos << 17. O menu manda as escolhas por fscommand
(`FSCommand:VT_<chave>_<valor>`), lidas em `runtime/vitart/game_options.cpp: on_import`. Detalhes em
`tools/gow_menu_mod.py`.
