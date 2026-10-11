// Decodificação de SceGxmTexture para RGBA8 (porte de tools/gxm_tex_dump.py, que é a referência).
#pragma once
#include <cstdint>
#include <vector>

namespace gxmtex {

struct Info {
    uint32_t type = 0, base = 0, swizzle = 0, width = 0, height = 0, stride = 0, data = 0, palette = 0;
};

Info describe(const uint32_t w[4]);
// guest: base da janela de 4 GiB do guest (endereço do guest = deslocamento). Devolve false para
// formatos ainda não suportados.
bool decode(const uint8_t *guest, const Info &t, std::vector<uint8_t> &rgba);
// Bytes que a textura ocupa na memória do guest a partir de t.data (0 se o formato não é suportado).
size_t data_size(const Info &t);

} // namespace gxmtex
