// Renderizadores dos desenhos do GXM (Vulkan em gxm_vk.cpp, Direct3D 11 em gxm_d3d11.cpp).
//
// Recebe cada desenho já resolvido (SPIR-V traduzido pelo vitarecomp.usse, atributos em vec4 float,
// índices de triângulos, uniforms, texturas RGBA8 ou referência a outro alvo) e desenha no alvo
// identificado pelo endereço da superfície de cor do guest, como no hardware: um alvo desenhado no
// quadro pode ser lido depois como textura pelo mesmo endereço.
//
// Contrato do SPIR-V (ver vitarecomp/usse/translate.py): vértice com atributos em location k e uniforms
// em set 0 (binding 0 = default uniform buffer, 1 = memória do vldst); fragmento com texturas em set 1
// (binding = recurso do sampler) e uniforms em set 2. A variante PBR do fragmento (vitarecomp/usse/pbr.py,
// docs/pbr.md) usa também o set 3 (normal map e ORM) e o set 4 (parâmetros por desenho).
#pragma once
#include <string>
#include <cstdint>
#include <vector>

namespace gvk {

struct Texture {
    uint32_t binding = 0;
    uint32_t target_addr = 0; // != 0: usa o alvo desenhado neste endereço
    bool depth = false;       // target_addr é uma superfície de profundidade (lida como float em .r)
    uint32_t width = 1, height = 1;
    const uint8_t *rgba = nullptr; // width*height*4
    uint64_t key = 0;              // != 0: identifica o conteúdo (evita o hash dos pixels a cada desenho)
    // Amostragem pedida pelo jogo (palavra 0 do SceGxmTexture): SceGxmTextureFilter (0 ponto, 1 linear)
    // e SceGxmTextureAddrMode de u e v.
    uint8_t mag = 1, min = 1, addr_u = 0, addr_v = 0;
    // Mipmaps: o jogo declara níveis (mip_count > 0) e o filtro entre eles; os níveis são gerados
    // aqui a partir do nível 0. lod_bias é o campo cru de 6 bits ((v - 31) / 8).
    uint8_t mip_count = 0, mip_filter = 0, lod_bias = 31;
};

// Estado de stencil de uma face (valores do GXM já decodificados; mesma ordem do Vulkan).
struct Stencil {
    uint8_t func = 7;                   // SceGxmStencilFunc >> 25 (7 = sempre)
    uint8_t fail = 0, zfail = 0, zpass = 0; // SceGxmStencilOp (0 = manter)
    uint8_t cmask = 0xFF, wmask = 0xFF, ref = 0;
};

// Floats do buffer de parâmetros do PBR (PARAM_FLOATS em vitarecomp/usse/pbr.py).
constexpr uint32_t kPbrParamFloats = 56;

// Vértices crus (sem conversão na CPU): streams do jogo e atributos com o formato do GXM
// (SceGxmAttributeFormat: 0 U8, 1 S8, 2 U16, 3 S16, 4 U8N, 5 S8N, 6 U16N, 7 S16N, 8 F16, 9 F32).
struct VertexStream {
    const uint8_t *data = nullptr;
    uint32_t stride = 0, size = 0;
};
struct VertexAttr {
    uint32_t location = 0, stream = 0, offset = 0, fmt = 9, comps = 4;
};
struct Draw {
    uint32_t prim = 0, cull = 0;
    bool has_blend = false;
    uint32_t blend = 0;     // SceGxmBlendInfo
    uint32_t depth_func = 7; // SceGxmDepthFunc (mesma ordem do VkCompareOp)
    bool depth_write = false;
    bool stencil = false;   // a superfície de profundidade tem stencil
    Stencil front, back;
    float vps[6] = {};      // x_off, x_escala, y_off, y_escala, z_off, z_escala
    uint32_t target_addr = 0, target_w = 0, target_h = 0, depth_addr = 0;
    uint32_t vertex_count = 0, attr_count = 0;
    const float *attrs = nullptr; // [attr_count][vertex_count][4]
    // Caminho cru: se raw_attrs não está vazio, attrs é ignorado (locations sem atributo ficam sem binding).
    std::vector<VertexStream> raw_streams;
    std::vector<VertexAttr> raw_attrs;
    const uint32_t *indices = nullptr; // lista de triângulos u32 (replay)
    // Índices do jogo sem conversão: se index_data != nullptr, indices é ignorado.
    const void *index_data = nullptr;
    uint32_t index_size = 4;  // 2 ou 4 bytes
    int32_t vertex_offset = 0; // somado a cada índice (rebase da faixa de vértices)
    uint32_t topology = 0;     // 0 lista, 1 faixa, 2 leque
    uint32_t index_count = 0;
    const uint8_t *vert_uniforms = nullptr; uint32_t vert_uniforms_size = 0;
    const uint8_t *frag_uniforms = nullptr; uint32_t frag_uniforms_size = 0;
    const uint32_t *vs = nullptr; uint32_t vs_size = 0; // bytes
    const uint32_t *fs = nullptr; uint32_t fs_size = 0;
    std::vector<Texture> textures;
    bool debug_vs = false; // o vertex shader (de depuração) grava OUTPUT 0..3 após os uniforms
    // PBR: fs é a variante PBR; pbr_maps = normal map e ORM (binding ignorado; amostragem como a da
    // difusa). pbr_params[0..15] chega com a inversa de g_Projection (NDC do GXM -> câmera, coluna a
    // coluna) e sai combinada com o viewport do desenho (pixel -> câmera).
    bool pbr = false;
    Texture pbr_maps[2];
    float pbr_params[kPbrParamFloats] = {};
    // Inversa de g_Projection do desenho (como pbr_params[0..15]), para o pós-processamento reconstruir a
    // posição pela profundidade; vale em qualquer desenho 3D, com ou sem PBR.
    bool view_inv_ok = false;
    float view_inv[16] = {};

};

// Barras de vida dos inimigos: centro (x) e base (y) em 0..1 da imagem do jogo, fração da vida.
struct EnemyBar {
    float x, y, frac;
    int num = -1; // número ao lado da barra (-1 = sem)
};
// trecho opaco de uma linha do desenho (o resto é transparente e não é copiado)
struct Span {
    uint32_t x, y, w;
};
struct TrophyBanner; // trophy_banner.h

// Onde o tempo vai, acumulado desde a última chamada (que zera): CPU em ns, GPU por timestamps.
struct Stats {
    uint64_t wait_ns = 0;   // esperando a fence do quadro anterior
    uint64_t gpu_ns = 0;    // duração na GPU dos command buffers de desenho concluídos
    uint64_t alloc_ns = 0;  // criação de imagens (texturas, cópias de alvo)
    uint64_t pipe_ns = 0;   // criação de módulos e pipelines
    uint64_t ring_ns = 0;   // cópias para o anel de upload
    uint64_t desc_ns = 0;   // alocação e escrita de descritores
    uint32_t submits = 0, snapshots = 0, uploads = 0, pipes = 0, draws = 0;
};

// Renderizadores: Vulkan (gxm_vk.cpp) e Direct3D 11 (gxm_d3d11.cpp); o vitart chama a fachada gvk::, que repassa
// ao escolhido (gxm_backend.cpp).
enum class Backend { vulkan, d3d11 };
// "vulkan", "d3d11" ou "auto" (Vulkan se houver driver e GPU; senão Direct3D 11, com o WARP do Windows como
// último recurso). Antes do init.
void select_backend(const char *name);
Backend backend();
const char *backend_name();

#include "gxm_api.inc"

} // namespace gvk
