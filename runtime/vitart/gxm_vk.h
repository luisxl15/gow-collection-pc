// Renderizador Vulkan dos desenhos do GXM.
//
// Recebe cada desenho já resolvido (SPIR-V traduzido pelo vitarecomp.usse, atributos em vec4 float,
// índices de triângulos, uniforms, texturas RGBA8 ou referência a outro alvo) e desenha no alvo
// identificado pelo endereço da superfície de cor do guest, como no hardware: um alvo desenhado no
// quadro pode ser lido depois como textura pelo mesmo endereço.
//
// Contrato do SPIR-V (ver vitarecomp/usse/translate.py): vértice com atributos em location k e uniforms
// em set 0 (binding 0 = default uniform buffer, 1 = memória do vldst); fragmento com texturas em set 1
// (binding = recurso do sampler) e uniforms em set 2.
#pragma once
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
};

struct Draw {
    uint32_t prim = 0, cull = 0;
    bool has_blend = false;
    uint32_t blend = 0;     // SceGxmBlendInfo
    uint32_t depth_func = 7; // SceGxmDepthFunc (mesma ordem do VkCompareOp)
    bool depth_write = false;
    float vps[6] = {};      // x_off, x_escala, y_off, y_escala, z_off, z_escala
    uint32_t target_addr = 0, target_w = 0, target_h = 0, depth_addr = 0;
    uint32_t vertex_count = 0, attr_count = 0;
    const float *attrs = nullptr; // [attr_count][vertex_count][4]
    const uint32_t *indices = nullptr;
    uint32_t index_count = 0;
    const uint8_t *vert_uniforms = nullptr; uint32_t vert_uniforms_size = 0;
    const uint8_t *frag_uniforms = nullptr; uint32_t frag_uniforms_size = 0;
    const uint32_t *vs = nullptr; uint32_t vs_size = 0; // bytes
    const uint32_t *fs = nullptr; uint32_t fs_size = 0;
    std::vector<Texture> textures;
    bool debug_vs = false; // o vertex shader (de depuração) grava OUTPUT 0..3 após os uniforms

};

// window: cria também a janela e o swapchain (apresentação ao vivo).
bool init(bool validation, bool window = false);
// Mostra o alvo de cor no endereço dado na janela (escala para o tamanho da janela).
bool present(uint32_t addr);
bool window_closed();
bool is_target(uint32_t addr);
bool is_depth(uint32_t addr);
// Início de cena do GXM: a profundidade do alvo volta ao valor de fundo (1,0), a não ser que a
// superfície peça "force load". Sem isso a profundidade de uma cena vaza para a seguinte.
void begin_scene(uint32_t color_addr);
void shutdown();
// Grava o desenho no quadro atual (os desenhos são executados em ordem em end_frame).
bool draw(const Draw &d);
// Executa o quadro e espera a GPU.
void end_frame();
// Lê um alvo em RGBA8 (para PNG ou para apresentar).
bool read_target(uint32_t addr, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h);
std::vector<uint32_t> targets();
const char *last_error();
// Saída OUTPUT 0..3 por vértice do último desenho com debug_vs (vec4 por vértice).
const std::vector<float> &debug_vs_output();

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
Stats take_stats();

} // namespace gvk
