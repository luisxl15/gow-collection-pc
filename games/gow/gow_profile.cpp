// as respostas do God of War Collection pro runtime (game.h)
#include "game.h"

#include <windows.h>

namespace vt {

const char *game_tag() {
    switch (game_id()) {
    case 1: return "gow1";
    case 2: return "gow2";
    default: return "";
    }
}

// o GoW2 tem o próprio arquivo; o do GoW1 ficou com o nome antigo
const char *game_audio_prefix() { return game_id() == 2 ? "ps2audio_gow2" : "ps2audio"; }

const wchar_t *game_window_title() { return L"God of War"; }

// esquema do God of War (2018) de PC, levado pros botões que fazem o mesmo nos GoW do Vita.
// a tabela completa está em docs/teclado-mouse-qte.md
uint32_t game_keyboard_buttons(KeyDown down) {
    const bool g2 = game_id() == 2;
    const struct { int vk; uint32_t bit; } map[] = {
        {VK_SPACE, 0x4000}, {VK_LBUTTON, 0x8000}, {VK_RBUTTON, 0x1000}, {'F', 0x2000}, {'Q', 0x400},
        {VK_LCONTROL, 0x100}, {'E', g2 ? 0x800u : 0x200u}, {'R', g2 ? 0x200u : 0x800u}, {'V', 0x6},
        {'1', 0x10}, {'2', 0x20}, {'3', 0x40}, {'4', 0x80}, {VK_UP, 0x10}, {VK_RIGHT, 0x20},
        {VK_DOWN, 0x40}, {VK_LEFT, 0x80}, {VK_ESCAPE, 0x1}, {VK_BACK, 0x1}, {'I', 0x8}, {'K', 0x8},
        {VK_TAB, 0x8}, {VK_RETURN, 0x8}, {'C', g2 ? 0xC00u : 0u},
    };
    uint32_t b = 0;
    for (auto &m : map)
        if (down(m.vk)) b |= m.bit;
    if (down(VK_MBUTTON)) b |= down(VK_SHIFT) ? 0x6u : 0x2000u; // com Shift vira fúria
    return b;
}

// nos WADs, a tag 24 nunca tem corpo; no GoW2 também não as 0, 2 e 3 (o "tamanho" ali é um valor)
bool game_wad_has_body(uint16_t tag, const std::string &psarc_name) {
    const bool gow2 = psarc_name.find("gow2") != std::string::npos;
    return !(tag == 24 || (gow2 && (tag == 0 || tag == 2 || tag == 3)));
}

} // namespace vt
