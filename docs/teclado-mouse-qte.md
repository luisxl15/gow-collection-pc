# Teclado e mouse, ícones e QTE automático (GoW1 e GoW2)

## Teclas (esquema do God of War 2018 para PC)

Padrões do GoW 2018 no PC (Shacknews e Magic Game World): WASD anda, botão esquerdo é o ataque leve, o
direito é o pesado, o do meio é o "Stun Grab", Q defende, Ctrl mira, E interage, Espaço esquiva, Shift
corre, R chama o machado, Esc abre as opções. Cada tecla vai para o botão que faz a mesma coisa nos GoW do
Vita (modo PS TV):

| Tecla | Botão | Ação |
|---|---|---|
| W A S D | analógico esquerdo | andar |
| Shift + W A S D | analógico direito | esquiva / rolamento |
| botão esquerdo | quadrado | ataque leve |
| botão direito | triângulo | ataque pesado |
| botão do meio, F | círculo | agarrar (e os QTEs de círculo) |
| Shift + botão do meio, V | L3 + R3 | Rage of the Gods / Rage of the Titans |
| Espaço | X | pular |
| Q | L1 | defender / aparar |
| Ctrl esquerdo | L2 | magia (GoW1) / item (GoW2) |
| E | R2 no GoW1, R1 no GoW2 | interagir (baús, portas, alavancas) |
| R | R1 no GoW1, R2 no GoW2 | o outro botão R |
| 1 2 3 4, setas | direcional | escolher magia, menus |
| Esc, Backspace | SELECT | pausa (nos dois jogos) |
| I, K, Tab, Enter | START | Power Up |

O teclado só vale com a janela do jogo em foco. O mouse como toque na tela continua no menu da coletânea e
no menu principal; dentro do jogo o botão esquerdo é só o ataque.

Código: `runtime/vitart/hle_sys.cpp` (`keyboard_mouse`). Controles continuam somados.

## Ícones dos botões

O último controle usado (teclas/mouse ou controle) decide os ícones. As texturas com os botões do
PlayStation têm versões com as teclas em `vitart-data/texpack_kbm/<jogo>/index.tsv` (mesma chave do pack
de texturas: FNV-1a de largura, altura e primeiros 4 KB, mais a paleta se P8). O runtime refaz só as
texturas que têm versão de teclado quando o modo muda (`hle_gxm.cpp`, `TexEntry::kbm_alt`).

Para achar as texturas: `VITART_TEXDUMP=<pasta>` grava cada textura (até 512x512) como
`<chave>_<l>x<a>.rgba`. Os PNGs são desenhados por `tools/kbm_glyphs.py` com `tools/kbm_icons.py`.

## QTE automático (opção "Auto QTE" na página Options)

O runtime lê o ícone que o HUD está mostrando e aperta o botão (ou gira o analógico) junto com o jogador.

- **GoW1:** `sub_8101406c(hud, índice)` mostra o ícone do minigame e guarda o índice em `hud+0x1570`
  (-1 = nenhum). `hud` é o objeto em `0x812a681c`, o mesmo das opções. Os nomes estão na tabela
  `0x812a6858`: 0 X, 1 triângulo, 2 círculo, 3 quadrado, 4..7 meias-voltas, 8/9 volta inteira
  horária/anti-horária, 10 X repetido, 11 R2 repetido, 12 sacudir, 13 vaivém, 14 R2 parado. O estado do
  jogo fica em `0x812c2e04`: 0xb é o jogo correndo e 0xc a pausa (medido comparando a memória com e sem
  pausa). Fora do 0xb nada é apertado.
- **GoW2:** `sub_81070b60(slot, valor)` grava em `0x812ec588 + 4*slot` o hash do nome do clipe
  (`h = h*127 + maiúscula`, `sub_811346de`): `PB_XBtn`, `PB_360CW`, `PB_CircleBtnSmash`, `PB_R1BtnHold`...
  Os ícones "Static" (baú, inimigo para agarrar) ficam de fora.

Toques a cerca de 14 por segundo (serve para os QTEs de repetir), "Hold" segurado, voltas a 2,5 por segundo.
Diagnóstico: `VITART_QTELOG=1` registra cada ícone; `VITART_AUTO_QTE=1` liga sem o menu.

A opção fica no bit 11 da palavra de estado do menu (`auto_qte=` no `vitart_options.ini`).
