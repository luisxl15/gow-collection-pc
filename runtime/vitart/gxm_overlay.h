// Imagens desenhadas por cima do jogo, comuns aos renderizadores: o aviso de troféu (fila e animação), o contador
// de FPS e as barras de vida dos inimigos. Aqui só se calcula o quê e onde (pixels BGRA/RGBA na CPU); cada
// renderizador copia para a imagem dele.
#pragma once
#include "gxm_vk.h"
#include "trophy_banner.h"

#include <cstdint>
#include <vector>

namespace gvk::ovl {

// Aviso de troféu igual ao do PS Vita: entra deslizando pela direita, fica ~4 s (2 s com fila) e sai.
void trophy_push(const TrophyBanner &t);
// Na apresentação, para a área da imagem do jogo na tela: a imagem do aviso e o canto superior esquerdo (pode
// sair da tela à direita durante a animação). false se não há aviso agora.
bool trophy_frame(int32_t area_x, int32_t area_y, int32_t area_w, int32_t area_h, const BannerImage *&img, int32_t &x0,
                  int32_t &y0);

// Contador de FPS: a cada apresentação mede (quadros por segundo a cada meio segundo) e, ligado, monta o texto
// "NN FPS" em BGRA para uma tela de altura screen_h.
void set_fps_counter(bool on);
uint32_t measured_fps();
bool fps_frame(uint32_t screen_h, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h);

// Barra de vida de um inimigo num alvo de im_w x im_h (RGBA na memória); false se cai fora da imagem.
bool enemy_bar(uint32_t im_w, uint32_t im_h, const EnemyBar &bar, std::vector<uint32_t> &px, uint32_t &bw, uint32_t &bh,
               int32_t &x0, int32_t &y0);

} // namespace gvk::ovl
