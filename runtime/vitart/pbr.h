// PBR no vitart: configuração (pbr.ini), materiais (normal map + ORM por textura) e parâmetros por
// desenho da variante PBR dos fragment shaders (vitarecomp/usse/pbr.py). Visão geral em docs/pbr.md.
//
// Materiais: <dados>/pbr/<pack>/index.tsv, uma linha por textura: chave<TAB>normal.png<TAB>orm.png (um
// dos dois pode faltar: "-"). A chave é a mesma do pack de texturas (hle_gxm.cpp, texpack_key), então
// tools/pbr_generate.py gera os mapas a partir do index.tsv do pack HD.
#pragma once
#include "gxm_vk.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace vt::pbr {

struct Config {
    bool enabled = false;         // liga a variante PBR (desligado: o jogo fica com a imagem original)
    bool all_surfaces = false;    // também nas texturas sem material (material neutro)
    bool normal_opengl = false;   // normal maps com G = +V para cima (padrão OpenGL): inverte o verde
    float normal_strength = 1.0f; // força do normal map
    float specular = 1.0f;        // especular das luzes
    float env_specular = 0.5f;    // especular de ambiente (céu/chão)
    float baked_max = 4.0f;       // teto da razão cor original / textura (luz assada)
    float roughness_scale = 0.75f, metallic_scale = 1.0f, ao_strength = 1.0f;
    // Luz-chave de relevo (vitarecomp/usse/pbr.py): direção em espaço de câmera, força do relevo e brilho.
    float key_dir[3] = {-0.35f, 0.75f, 0.55f};
    float relief = 0.8f, key_spec = 0.5f;
    // Iluminação de tela (gxm_vk.cpp, runtime/vitart/post): oclusão de ambiente, bloom e curva fílmica.
    bool post = false; // desligado por padrão (o usuário não quer o filtro); post=1 no pbr.ini liga
    float ssao = 1.6f, ssao_radius = 40.f, bloom = 0.35f, bloom_threshold = 0.7f, exposure = 1.08f, tonemap = 0.6f,
          saturation = 1.08f, vignette = 0.15f;
    // profundidade de campo (foco automático no centro), raios de luz, acabamento e color grading
    float dof = 0.65f, dof_range = 0.6f, focus_speed = 0.08f;
    bool dof_near = false;
    float shafts = 0.6f, grain = 0.03f, chroma = 0.004f, sharpen = 0.4f;
    bool post_split = false; // metade esquerda com o pós, direita sem (comparação)
    int post_debug = 0;      // 1 oclusão, 2 desfoque do DOF, 3 raios de luz
    float grade = 1.0f, contrast = 1.08f, lift[3] = {0.004f, 0.008f, 0.016f}, gain[3] = {1.04f, 1.0f, 0.95f};
    float default_roughness = 0.8f; // material neutro (all_surfaces)
    int debug = 0;                  // 1 normal, 2 luz assada, 3 rugosidade, 4 metal, 5 oclusão, 6 posição, 7 detalhe
    float sky[3] = {0.55f, 0.60f, 0.70f}, ground[3] = {0.22f, 0.20f, 0.18f}, up[3] = {0.f, 1.f, 0.f};
    // Luz para desenhos sem g_LightDir quando nenhum desenho iluminado passou há pouco (espaço de câmera).
    float fallback_dir[3] = {-0.35f, 0.75f, 0.55f}, fallback_color[3] = {0.65f, 0.62f, 0.58f};
};
const Config &config();
bool enabled(); // config().enabled e VITART_PBR != 0
// Uma vez por quadro apresentado, com o mutex do renderizador: relê o pbr.ini se mudou e tira da
// memória os mapas sem uso há mais de ~10 s (voltam do disco se forem pedidos de novo).
void tick(uint32_t present_frame);

struct Material {
    std::shared_ptr<std::vector<uint8_t>> normal, orm; // RGBA8
    uint32_t nw = 1, nh = 1, ow = 1, oh = 1;
    uint64_t key = 0;
};
// Chave do material da textura (0 = sem material). base: nível base da textura no guest.
uint64_t key_for(const uint8_t *base, size_t base_bytes, uint32_t w, uint32_t h, const uint8_t *palette);
// Material pronto (nullptr enquanto os PNGs carregam em segundo plano, ou se não abriram).
std::shared_ptr<const Material> material(uint64_t key, uint32_t present_frame);
// Material neutro (normal reta, rugosidade default_roughness) para all_surfaces.
std::shared_ptr<const Material> neutral();

// Uniforms do programa de vértice (palavra no default uniform buffer; -1 = o programa não tem).
struct VertexInfo {
    int proj = -1, light_dir = -1, light_color = -1, ambient = -1;
};
// Parâmetros por desenho (layout PARAMS de pbr.py). vu: default uniform buffer do vértice. false se o
// desenho não tem projeção em perspectiva (HUD, passadas de tela): fica sem PBR.
// Inversa de g_Projection (NDC do GXM -> câmera, coluna a coluna); false se não é perspectiva.
bool inv_projection(const VertexInfo &vi, const float *vu, uint32_t vu_words, float out[16]);
bool params(const VertexInfo &vi, const float *vu, uint32_t vu_words, uint32_t present_frame,
            float out[gvk::kPbrParamFloats]);

// Pixel do alvo -> câmera, coluna a coluna: inv_proj (NDC do GXM -> câmera) vezes a inversa do viewport
// realmente usado na GPU (VkViewport: escala interna e meio pixel inclusos; altura pode ser negativa).
void screen_to_view(const float inv_proj[16], float vx, float vy, float vw, float vh, float min_depth, float max_depth,
                    float out[16]);

} // namespace vt::pbr
