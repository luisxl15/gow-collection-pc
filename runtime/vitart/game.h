// o que o runtime pergunta pro jogo. cada jogo responde na sua pasta, games/<jogo>/
#pragma once

#include <cstdint>
#include <string>

namespace vt {

// 0 enquanto nada carregou (menu da coletânea, por exemplo)
int game_id();
// nome da subpasta dos dados do jogo atual: texpack/<tag>, pbr/<tag>; "" se não tem
const char *game_tag();
// arquivos do áudio de PS2 extraído: <dados>/<prefixo>.bin e .idx
const char *game_audio_prefix();
const wchar_t *game_window_title();

// teclado: o jogo diz que botão do Vita cada tecla aperta. WASD e Shift ficam com o runtime
using KeyDown = bool (*)(int vk);
uint32_t game_keyboard_buttons(KeyDown down);

// formato dos WADs pro compositor de mods: esse registro tem corpo depois do cabeçalho?
bool game_wad_has_body(uint16_t tag, const std::string &psarc_name);

void game_options_tick();             // uma vez por quadro, antes de apresentar
void enemy_hp_draw(uint32_t fb);      // desenho do jogo por cima do quadro
bool game_in_gameplay();
uint32_t game_qte_auto(uint8_t &lx, uint8_t &ly);
std::string renderer_choice();        // lido do ini antes de abrir o renderizador

} // namespace vt
