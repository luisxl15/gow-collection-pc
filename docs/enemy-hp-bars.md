# Barras de vida dos inimigos (como no GoW3)

Estado em 25/10: **GoW2 levantado, falta desenhar**. `runtime/vitart/enemy_hp.cpp` só tem o diagnóstico
(`VITART_ENEMYLOG=1` grava `enemy_log.txt` no diretório do executável; `VITART_CAMREF=<hex>` procura ponteiros).

## GoW2: o que foi medido

- **Vida**: criatura `+0x170` (vida, float) e `+0x174` (vida máxima). Fonte: o medidor de chefe do HUD
  (0x81029cdc): `EnemyMeter_ValueN = vida / máxima * 100`, só com o estado do jogo `[0x812ec498] == 0xc`.
- **Lista de objetos**: tabela de handles em `[0x812cba94]`; entrada k em `+8 + 12k` = {handle, ?, objeto},
  válida se `handle >> 23 == k` (mesmo teste do jogo em 0x81029b62).
- **Kratos**: `[[0x812cbae4] + 4]`. Kratos e os inimigos têm a mesma vtable (`0x8122e9c4`); objetos com
  10000/10000 e outra vtable (`0x8122e734`) não são inimigos.
- **Posição de mundo**: criatura `+0x80` (x, y, z; y para cima; +0x110 fica 6,4 acima). Medido no começo do
  GoW2: legionários 20/20, Kratos 179/200.
- **Câmera**: `[0x812cc1ec]` = objeto "The_Viewport" (0x3c0 bytes, criado em 0x8106c200). Matrizes 4x4 por colunas
  (`clip_i = soma_j M[4j+i] * X_j + M[12+i]`):
  - `+0x000`: mundo -> câmera (olha para -z);
  - `+0x040`: mundo -> clip (câmera x projeção), **a usar**;
  - `+0x100`: projeção (1,4846; -2,6393; -1,002/-1; -20,03), o y negativo já põe +y para baixo;
  - `+0x180`/`+0x1c0`: viewport antigo do PS2 (512x448 centrado em 2048), não usar.
  - Tela: `x = 0.5 + 0.5 * clip.x / clip.w`, `y = 0.5 + 0.5 * clip.y / clip.w`, com `clip.w > 0`. Falta conferir
    numa captura.

## Próximos passos

1. Em `hle_gxm.cpp`, antes do bloco do `VITART_VK_DUMP` (para a captura de teste mostrar), chamar uma função nova
   do `gxm_vk.cpp` que copia retângulos para o alvo de cor `g_targets[fb].color` (como `overlay_fps`: barreira para
   TRANSFER_DST, `vkCmdCopyBufferToImage`, barreira de volta para COLOR_ATTACHMENT, no `g_cmd` do quadro).
2. `enemy_hp.cpp` monta a lista: criaturas com a vtable do Kratos, sem o Kratos, `0 < vida < máxima`, em gameplay
   (`[0x812ec498] == 0xc`); ponto da cabeça = `+0x80` + altura (medir; começar com ~30 unidades em y).
3. Estilo: moldura escura, fundo vermelho-escuro, preenchimento vermelho em degradê, largura ~8,5% da altura do
   alvo e altura ~1,1%.
4. Opção no menu (Graphics ou Audio & Gameplay) e em `vitart_options.ini` (`enemy_health_bars=1`). Na palavra de
   estado não há bit livre abaixo de 2^24 (o contador de FPS usa os bits 17..23): precisa de outra variável.
5. GoW1: repetir a busca (o medidor de chefe do GoW1 tem as mesmas strings `EnemyMeter_Value*`).

## Ferramentas

Em `tools/re/`: `xref.py` (índice de movw/movt e bl do ELF), `d.py` (desmontagem com strings).
ELFs em `gow-dump/elf/`.
