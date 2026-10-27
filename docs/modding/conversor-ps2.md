# Conversor de mods do PS2 para o Vita

Mods do God of War II de PS2 rodam no God of War Collection do Vita (e no vitart) depois de convertidos. O
conversor aproveita que o porte do Vita foi feito a partir do jogo de PS2: os WADs têm a mesma árvore de
registros, só que as texturas e as malhas estão em formatos do Vita. Quem usa: o Mod Studio, na aba "Converter mod do
PS2", ou `tools/vitart_modkit.py convert-ps2`. Este documento explica o que acontece por dentro.

```
patch .xdelta (ou .zip do mod) + ISO original do PS2
   │  1. ps2_xdelta_wads.py   aplica o patch em fluxo, guarda só os arquivos que mudaram
   ▼
WADs do mod (PS2) + originais desses WADs (PS2)
   │  2. gow2_mod_wad.py      WAD do PS2 -> WAD do Vita (texturas, malhas, registros)
   ▼
.wad_psp2 convertidos (versão normal e de PS TV)
   │  3. vitart_modkit.py     instala como mod: vitart-data/mods/<id>/
   ▼
   4. gow_menu_mod.py + hle_io.cpp   patch do psarc e carregamento pelo jogo
```

## 1. Aplicar o patch sem gravar a ISO (`tools/ps2_xdelta_wads.py`)

Os mods de PS2 são distribuídos como patch XDelta da ISO americana (SCUS-974.81), para não distribuir o jogo. Uma
ISO com o patch teria uns 8 GB. O conversor nunca grava essa ISO:

1. **Primeira passada:** o `xdelta3 -d` escreve a ISO nova no stdout, e o script guarda só os primeiros 64 MB de
   cada camada do DVD. Ali ficam os descritores ISO9660, os diretórios e o `GODOFWAR.TOC`, o índice dos arquivos
   dentro dos `PART1.PAK`/`PART2.PAK`.
2. **Segunda passada:** com o TOC novo, o script recolhe cada arquivo do fluxo e o compara pelo nome com o da ISO
   original. Grava só os que mudaram e a lista deles, `mudados.tsv`.

O patch pode estar dentro do `.zip` do mod, lido em fluxo, sem extrair. Uma ISO num `.7z` é extraída numa pasta
temporária e apagada no fim. Em seguida, o kit copia da ISO original as versões antigas dos WADs que mudaram
(`orig/`). Arquivos que não são WAD, como os vídeos `.PSS`, são descartados, porque o Vita tem os próprios.

## 2. WAD do PS2 -> WAD do Vita (`tools/gow2_mod_wad.py`)

Um WAD é uma lista de registros `{tag, flags, tamanho, nome[24], corpo}` alinhados a 16 bytes. Comparando o
`r_hero01` do PS2 com o do Vita registro por registro, o Vita mostrou ser a mesma árvore com três diferenças:

| Registro | PS2 | Vita |
|---|---|---|
| Textura (`GFX_*` + `PAL_*`) | pixels de 4/8 bits + paleta | os dois viram "tocos" de 32 bytes (só o cabeçalho), e depois do `PAL_` entra `~NNN<gfx>~<pal>` (tipo 0x00070007) com um **GXT**: BC3 256×256, swizzle do Vita e mipmaps. O `TXR_*` aponta para esse nome em +0x34 |
| Malha (`MDL_<nome>_0`) | pacotes VIF do PS2 | depois de cada `MDL_<nome>` entra `~MD_<nome>` (tipo 0x0003000f): um **GMDL** com buffers de vértice |
| Materiais, animações, esqueleto, scripts, efeitos | — | **iguais, byte a byte** |

A conversão de cada parte:

- **Textura:**
  1. a paleta é aplicada aos índices, com o alfa do PS2 (0–128) levado a 0–255;
  2. a imagem é redimensionada para 256×256 e comprimida em BC3;
  3. os blocos são reordenados no swizzle do Vita (ordem Z) e entram os mipmaps;
  4. o GXT do registro original do Vita serve de molde para o cabeçalho.
- **Malha** (`tools/ps2_mesh.py` lê, `tools/gmdl.py` escreve):
  - os pacotes VIF trazem posição, peso, normal int8, cor e UV;
  - no GMDL:
    - `POS0 = (x, y, z, 1 − peso)`;
    - `BONI` = os dois ossos, com o mapa de ossos do objeto renumerado em ordem crescente;
    - `NRM0` = a normal;
    - `COL0` = cor e alfa em half;
    - `TEX0` = UV em half;
  - os triângulos saem da tira do PS2, pulando os vértices marcados para pular, e os vértices iguais são
    unificados;
  - conferido no Kratos original: os 7.683 vértices do PS2 batem com o GMDL do Vita.
- **Tudo o mais é copiado.** Por isso animações, golpes e efeitos do mod funcionam sem tradução.

Dois casos especiais:

- **`r_perma` e `r_shella`** (HUD, menus, textos) ganharam no Vita registros que o PS2 não tem. Para esses WADs,
  o `convert_merge` parte do WAD do Vita e troca só o que o mod mudou em relação ao **PS2 original**:
  - as texturas, o par GFX+PAL e o `~`;
  - as malhas e o GMDL do modelo;
  - os registros que o Vita deixou iguais aos do PS2.

  Os textos (`MSGS_*`) ficam os do Vita, que têm os do PS TV.
- **Versão de PS TV** (`wad/_VitaTV/`): é a que o vitart carrega, com L2/R2 no controle. Quando difere da normal
  (ícones e textos de botão), é convertida contra ela mesma. Quando é igual, vale o mesmo arquivo.

Medido no GoW2: dos 40 WADs que um mod de personagem testado muda, 36 são reproduzidos byte a byte a partir do PS2
original. Isso confere o conversor inteiro: converter o WAD original do PS2 tem de dar o WAD do Vita.

## 3. Instalar como mod (`tools/vitart_modkit.py`)

O mod vira uma pasta `vitart-data/mods/<id>/` com:
- `mod.ini`: nome, autor, versão, descrição e jogo;
- `src/`: os `.wad_psp2` convertidos, com o caminho que têm dentro do psarc;
- `entries.tsv`: a lista de arquivos que o mod troca.

O mod entra em `mods/modlist.tsv`, que dá a ordem e a linha no menu, e o kit roda o `rebuild`:
1. regera o patch base de cada jogo (o menu do PC, com a página Mods);
2. aplica o menu do vitart por cima dos `r_perma`/`r_shella` dos mods que também trocam esses arquivos, para o menu
   continuar existindo com o mod ligado.

## 4. Como o jogo carrega (`tools/gow_menu_mod.py` + `runtime/vitart/hle_io.cpp`)

O jogo lê tudo de `exec/gow2_psp2_SCEA.psarc` pela biblioteca FIOS, que consulta o índice (TOC) do psarc. O vitart
nunca altera o psarc:

- **Abertura do psarc.** O vitart monta uma TOC remendada, de mesmo tamanho, em que as entradas dos arquivos
  dos mods ligados apontam para dados "extras" depois do fim do psarc original. Na leitura, junta TOC remendada, psarc
  original e extras. Para o jogo, é o psarc de sempre.
- **Ligar/desligar com o jogo aberto** (Options > Mods > Apply):
  1. o FIOS guardou uma cópia da TOC na memória do jogo, ordenada pelo MD5 do nome;
  2. o vitart acha cada entrada pelo MD5 e troca bloco, tamanho e offset;
  3. entradas sem nome garantem que os blocos dos arquivos reservados já estejam preparados.

  Fases, inimigos e efeitos mudam na próxima vez que o jogo os carregar. Se o mod troca o Kratos, o HUD ou os menus,
  o jogo reinicia sozinho para aplicar tudo de uma vez.
- **Mods de registro** (`records.tsv`, como a tradução): trocam registros dentro de um WAD em vez do arquivo
  inteiro e são compostos sobre o resultado dos mods de arquivo inteiro.

## Limitações

- **Só GoW2.** O GoW1 do PS2 usa outro formato de malha, e o conversor dele não existe. As texturas do GoW1 dá para
  trocar pelo editor de texturas do Mod Studio.
- **Textos do mod não entram:** o Vita mantém os dele. Para tradução, use a aba Textos.
- **A ISO tem de ser a que o mod pede**, com a mesma região e versão. Senão o `xdelta3` falha logo no começo.

Comandos avulsos (o Mod Studio roda os mesmos):

```
python tools/ps2_xdelta_wads.py <xdelta3.exe> <mod.zip | patch.xdelta> <iso> <trabalho>/ps2
python tools/gow2_mod_wad.py --all <trabalho>/ps2 <app>/exec/gow2_psp2_SCEA.psarc <trabalho>/vita
python tools/vitart_modkit.py convert-ps2 ...        # tudo acima + instalação + rebuild
```
