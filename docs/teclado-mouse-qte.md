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

O último controle usado (teclas/mouse ou controle) decide os ícones. Os ícones dos botões do HUD não são
texturas: cada um é um clipe do FLP com um disco (malha com textura) e o símbolo em vetor, desenhado
sempre no quadro 0 e sem scripts (um script que trocava de quadro não teve efeito no texto do tutorial).

- **Desenhos** (`tools/kbm_icons.py`): teclas com o rótulo e mouse com o botão aceso.
- **Atlas** (`tools/kbm_atlas.py`): os desenhos vão para as células das letras acentuadas que o jogo em
  inglês não usa (glifos 87..106), no atlas da fonte do HUD (o mesmo nos dois jogos, chave
  `dd387c3eceaf6c63`), 4x maior: `vitart-data/texpack_kbm/<jogo>/`. As células foram medidas no jogo com um
  atlas "codificado" (vermelho = x, verde = y). Esse atlas só entra com teclado/mouse em uso
  (`hle_gxm.cpp`, `TexEntry::kbm_alt`).
- **HUD** (`tools/kbm_flp.py`, chamado pelo `gow_menu_mod.py`): para cada ícone (a biblioteca da raiz,
  GoW1 49..93 / GoW2 42..58, e as cópias soltas como as dicas da pausa do GoW1) o elemento do quadro 0
  ganha uma chave de teclado que aponta para a letra-ícone; rótulos sem desenho ("1-4") ficam com as
  letras da fonte sobre um disco escuro. Elementos que sobram ficam com escala zero (o motor ignora o alfa
  da cor do material). As chaves vão para `mods/<psarc>.kbm.tsv`.
- **Runtime** (`kbm_icons.cpp`): acha o FLP carregado procurando as chaves (o jogo ajusta ponteiros do FLP
  ao carregar, mas as chaves ficam como no arquivo) e escreve as do controle em uso.

O WAD portátil do GoW2 não tem espaço para os ícones (o patch não pode crescer em blocos do psarc); o
modo PS TV, o padrão do vitart, tem.

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
  Estado do jogo em `0x812ec498`: 0xc correndo, 0xd pausa (medido do mesmo jeito); fora do 0xc nada é
  apertado.

Toques a cerca de 14 por segundo (serve para os QTEs de repetir), "Hold" segurado, voltas a 2,5 por segundo.
Diagnóstico: `VITART_QTELOG=1` registra cada ícone; `VITART_AUTO_QTE=1` liga sem o menu.

A opção fica no bit 10 da palavra de estado do menu (`auto_qte=` no `vitart_options.ini`).
