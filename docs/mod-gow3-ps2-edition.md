# Adaptar o mod "God of War 3 PS2 EDITION" (GoW2) para o vitart

Mod: <https://www.nexusmods.com/godofwar2/mods/11> (v1.1, GustavoPredador, RePlay Games, Mikachuuu; texturas de
MMZXLLNT / Luciano Neto2651). Troca o Kratos do GoW2 de PS2 pelo do GoW3: modelo 3D, texturas, eventos e efeitos das
Lâminas, Cestus e correntes. Distribuído como patch **XDelta** para a ISO americana **SCUS-974.81** (a mesma da qual
tiramos o áudio e a tabela de nomes; `Desktop\God of War II (USA).7z`). O download exige login no Nexus: o usuário
baixa e põe o `.xdelta` na área de trabalho.

## Por que não é só aplicar o patch

O vitart roda o GoW2 **do Vita** (`gow2_psp2_SCEA.psarc`, WADs `.wad_psp2`); o patch muda os WADs da ISO **de PS2**.
Os dois jogos têm a mesma engine e a mesma organização de dados, mas os conteúdos foram convertidos para o Vita:

| | PS2 (SCUS-974.81) | Vita (PCSA00126) |
|---|---|---|
| Contêiner | `GODOFWAR.TOC` + `PART1/2.PAK`, WADs (`tools/texpack_ps2.py: toc_any, walk`) | psarc, `wad/*.wad_psp2` (`tools/texpack_match.py: wad_records`) |
| Registros | tag 21 `WAD_...`, dados em tag 1, grupos 2/3 | igual: tag 21, servidores em tag 1, `GroupStart`/`GroupEnd` (2/3) |
| Servidores do WAD | GFX, TXR, MAT, MDL, ANM, SCR, BHV, FLP, CXT, PRT, CAM, SND, COL, SHG, FX, WYP, LGT, EEPR | os mesmos nomes (`GFXX_`, `TXRX_`, `MATX_`, `MDLX_`, `ANMX_`... em `r_hero00.wad_psp2`) |
| Textura | GFX + PAL (índices 4/8 bits do GS, swizzle do PS2) | GXT dentro do registro (`TXTR`, corpo+14), BC1/BC3/P8 swizzle do Vita |
| Modelo | MDL/OBJ com listas de vértices para o VU1 (pacotes VIF/GIF) | MDL próprio do Vita (buffers de vértice/índice para o sceGxm) |

Medido em 24/10 (`r_hero00.wad_psp2`): 144 registros, servidores na mesma ordem que no PS2, tipos (u32 no corpo)
GFX 0x0c, TXR 0x07, MAT 0x08, LGT 0x06, FLP 0x1b, CXT 0x01, ANM 0x03, SCR 0x04, BHV 0x14, MDL 0x0f, PRT 0x11,
CAM 0x09, EEPR 0x17, SND 0x15, COL 0x10, SHG 0x20, FX 0x19, WYP 0x12. Os dados do Kratos ficam nos `r_hero01`..`r_hero16`.

**Texturas sozinhas não servem**: as do mod foram pintadas para o mapeamento (UV) do modelo novo; no modelo do Vita
apareceriam embaralhadas. Precisa trocar o modelo **e** as texturas juntos.

## Plano

1. **Diferença do patch** — aplicar o `.xdelta` numa cópia da ISO (`xdelta3 -d -s <iso> <patch> <iso mod>`) e listar
   os WADs que mudam (`toc_any` nas duas ISOs, comparar por arquivo). Esperado: `R_HERO*.WAD`, armas, FX das Lâminas.
   Dentro de cada WAD, os registros trocados (nome, tipo do servidor, tamanho).
2. **Formato do modelo do PS2** — o "God of War Browser" (Mongaika, citado nos créditos do mod; github.com/mogaika/god_of_war_browser)
   já lê MDL/OBJ/MAT/GFX/TXR do GoW1/GoW2 de PS2 e exporta para glTF. Usar como referência (ou como ferramenta) para
   tirar vértices, UV, pesos de osso, materiais e o esqueleto do Kratos do mod.
3. **Formato do modelo do Vita** — o mesmo para `MDLX` do `.wad_psp2`: decodificar comparando com o modelo do PS2
   original (mesmo Kratos, mesmos ossos): achar onde ficam posições, UV, pesos, índices e a ligação com o esqueleto
   (o vitart já mostra os buffers no sceGxm: `VITART_VK_DUMP`, `gxm_replay`, os atributos de cada desenho dão o layout).
4. **Conversor** `tools/mdl_ps2_to_vita.py`: modelo do mod (PS2) -> registro `MDLX` do Vita, mantendo o esqueleto e os
   nomes dos ossos do Vita (as animações `ANMX` continuam as do jogo; se o mod mexer no esqueleto, converter também).
   Texturas: GFX/PAL do mod -> GXT (BC3, ou RGBA/P8 sem compressão, que o decodificador do vitart já lê).
5. **Entrega** como os outros mods: `tools/gow_menu_mod.py: build_patch` já escreve um `.vtpatch` que o vitart aplica na
   leitura do psarc (`hle_io.cpp`) e agora aceita arquivos que crescem. Os WADs `r_hero*` (e `_VitaTV/`) trocados vão
   num `.vtpatch` próprio, ligável por opção no menu (Options > "GoW3 Kratos") sem tocar no psarc original.
6. **Eventos das Lâminas** (scripts/efeitos do mod: `SCR`, `FX`, `PRT`) — mesmo caminho do passo 3 para cada servidor;
   começar só pelo modelo e as texturas e deixar os efeitos por último.

## Riscos

- O modelo do Vita pode ter menos ossos por vértice ou outro limite de matrizes por desenho que o do PS2; o modelo do
  GoW3 pode passar do orçamento (o conversor divide em lotes).
- Se o mod mudar o esqueleto (ossos novos para as Cestus/correntes), as animações do Vita não os conhecem: ou se
  convertem as animações do mod, ou os ossos extras ficam presos ao pai.
- Créditos e permissão: o mod é de terceiros; a adaptação deve citar os autores e só usar o arquivo baixado pelo
  próprio usuário (o patch é XDelta justamente para não distribuir dados do jogo).

## Estado

24/10: só levantamento (este documento).

25/10: passo 1 feito com `tools/ps2_xdelta_wads.py` (xdelta3 3.1.0 oficial; o patch é lido de dentro do zip, a ISO
com o patch não é gravada). A ISO do mod tem 4,30 GB e camada única (a original tem 8,5 GB e duas camadas); o TOC tem
os mesmos 913 nomes e **122 arquivos mudam** (1,4 GB, em `gow-dump/gow3mod_ps2/`, lista em `mudados.tsv`): os
`R_HERO*`, `R_WEAPON*`, `R_S_*` (armas secundárias), `R_SHELLA` e também vídeos (`.PSS`/`.PSW`). Próximo: passo 2,
comparar os registros de `R_HERO01.WAD` original e do mod (quais servidores mudaram) e decodificar o MDL de PS2.

25/10, passo 2 (comparação por registro, `texpack_ps2.walk`; originais dos WADs em `gow-dump/gow3mod_ps2/orig/`):
- `R_HERO01.WAD` (o Kratos): `MDL_hero_0` trocado (193.440 -> 178.336 bytes); as três texturas `kratos01_1..3`
  (GFX+PAL+TXR) saem e entram `kratos02_1..3` (GFX 65.560 bytes cada, ou seja 256x256 em 8 bits); material novo
  `MAT_kratos2B`; um registro sem nome de 27.904 bytes muda com o mesmo tamanho (provável esqueleto/OBJ).
- `R_WEAPON*`: textura da lâmina (`GFX_blade0` 8.216 -> 16.408) e o modelo (`MDL_MAIBlade_0` 10.144 -> 27.856).
- `R_S_*` (armas secundárias): texturas e `MDL_*` trocados com tamanhos parecidos.
- `R_SHELLA`: só texturas do menu.
- Vídeos `.PSS`/`.PSW`: reencodados (menores) para a ISO caber numa camada; não servem ao Vita.
No Vita (`wad/r_hero01.wad_psp2`) os nomes são os mesmos (`GFX_kratos01_1..3`, `MAT_kratos1A..3B`, `MDL_hero_0`,
este com uns 236 KB): a conversão é registro a registro, com o MDL como trabalho principal.
