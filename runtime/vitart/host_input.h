// Controles do host para o SceCtrl do Vita: XInput (Xbox e compatíveis) e, por HID nativo do Windows,
// DualShock 4 e DualSense (USB e Bluetooth). Sem dependências além das DLLs do sistema.
#pragma once
#include <cstdint>

namespace hostin {

struct State {
    uint32_t buttons = 0;              // bits do SceCtrlData (cruz 0x4000, start 0x8, L 0x100, R 0x200...)
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128; // analógicos do Vita: 0..255, centro 128, cima = 0
};

// Estado combinado de todos os controles conectados (botões somados; analógico do mais inclinado).
State poll();

} // namespace hostin
