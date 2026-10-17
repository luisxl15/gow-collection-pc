// Cenas em vídeo remasterizadas (video_hd.cpp): troca a imagem do .bk2 que o jogo toca pela do arquivo
// em alta resolução de <dados>/videos/<jogo>/index.tsv, no mesmo instante.
#pragma once
#include <cstdint>
#include <string>

namespace vt {
// O jogo abriu um vídeo (.bk2): começa a decodificar o remaster correspondente, se houver.
void video_hd_open(const std::string &path, uint32_t frames, uint32_t fps_num, uint32_t fps_den, uint32_t out_w, uint32_t out_h);
void video_hd_close();
// Quadro para mostrar agora; bink_frame = quadro atual do Bink do jogo (-1 se ainda desconhecido).
bool video_hd_frame(int64_t bink_frame, const uint8_t *&rgba, uint32_t &w, uint32_t &h);
bool video_hd_active();
// Quadro atual do Bink do jogo (hle_io.cpp); -1 enquanto desconhecido.
int64_t video_bink_frame();
} // namespace vt
