# Barras de vida dos inimigos

Estado em 27/10: **GoW2 funcionando**. `games/gow/enemy_hp.cpp: enemy_hp_draw` monta as barras no present (antes
da captura de teste); `ovl::enemy_bar` (gxm_overlay.cpp) desenha cada uma e os renderizadores copiam só os trechos
opacos para o alvo de cor. Liga/desliga em **Options > Audio & Gameplay > Enemy Health** no menu de pausa, ou
`enemy_health_bars=1` no `vitart_options.ini` (padrão ligado; bit 16 da palavra de estado do menu).

Visual no jeito dos GoW novos: número (a vida que falta, já que o GoW2 não tem nível), seta `<`, filete dourado
(claro em cima, escuro embaixo; a parte perdida fica escura), seta `>`, tudo com contorno escuro de 1 px. O
filete tem 0,62% da altura do alvo de espessura e 22 vezes isso de comprimento. Testes: `VITART_HPBAR_ALL=1`
mostra a barra em todas as criaturas, `VITART_HPBAR_H` muda a altura (padrão 42 unidades acima dos pés),
`VITART_ENEMYLOG=1` liga o diagnóstico (`enemy_log.txt`, `enemy_hp_changes.txt`).

Atenção ao testar: a entrada automática dos scripts aperta X (pulo); para ferir inimigos é preciso apertar quadrado
(0x8000). Os legionários do começo têm 20 de vida.

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
3. Estilo: feito (ver acima). O primeiro era uma barra vermelha com moldura, trocada pelo filete dourado.
4. Opção no menu: feita, linha Enemy Health no GoW2, bit 16 da palavra de estado (estava livre).
5. GoW1: repetir a busca (o medidor de chefe do GoW1 tem as mesmas strings `EnemyMeter_Value*`).

## Ferramentas

Em `tools/re/`: `xref.py` (índice de movw/movt e bl do ELF), `d.py` (desmontagem com strings).
ELFs em `gow-dump/elf/`.
