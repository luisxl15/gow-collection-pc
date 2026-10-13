// Aviso de troféu no estilo do PS5: faixa escura de cantos arredondados no canto superior direito, com o
// ícone do troféu (TROPnnn.PNG do TROPHY.TRP do jogo), "Você ganhou um troféu" com a cor do nível e o nome
// do troféu. Desenhado com o GDI num bitmap BGRA (sem Vulkan: dá para testar sem janela).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gvk {

struct TrophyBanner {
    std::vector<uint8_t> icon_png; // ícone do troféu (PNG), pode faltar
    char tier = 'B';               // B, S, G, P
    std::string name;              // nome do troféu (UTF-8)
};

struct BannerImage {
    std::vector<uint32_t> px; // BGRA, alfa 255 dentro da faixa
    uint32_t w = 0, h = 0;
    uint32_t radius = 0;      // raio dos cantos: as linhas de cima/baixo copiam só o trecho de dentro
    // trecho de cada linha que pertence à faixa (para não copiar os cantos de fora)
    std::vector<uint32_t> row_from, row_to;
};

// Monta a imagem do aviso para uma tela de altura screen_h.
bool render_trophy_banner(const TrophyBanner &t, uint32_t screen_h, BannerImage &out);

} // namespace gvk
