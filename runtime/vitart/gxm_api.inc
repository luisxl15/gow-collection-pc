// Funções de um renderizador do GXM (ver gxm_vk.h). Incluído dentro de um namespace: gvk (a fachada usada pelo
// resto do vitart, gxm_backend.cpp), gvk::vkb (Vulkan, gxm_vk.cpp) e gvk::dx11 (Direct3D 11, gxm_d3d11.cpp).

// O formato existe como entrada de vértice nesta GPU?
bool vertex_format_ok(uint32_t fmt, uint32_t comps);
// window: cria também a janela e o swapchain (apresentação ao vivo).
bool init(bool validation, bool window = false);
// Mostra o alvo de cor no endereço dado na janela (escala para o tamanho da janela).
bool present(uint32_t addr);
bool window_closed();
// Retângulo externo da janela (para a troca de módulo sem janela nova); false se ainda não há janela.
bool window_rect(int &x, int &y, int &w, int &h);
// Mouse sobre a janela como toque: posição normalizada (0..1) na área cliente e botão esquerdo apertado.
bool pointer(float &nx, float &ny);
// A janela do jogo está em primeiro plano (teclado e mouse só valem com ela em foco).
bool has_focus();
// Aviso curto no título da janela (UTF-8), por alguns segundos, com o som de notificação do Windows.
void notify(const std::string &text);
bool is_target(uint32_t addr);
bool is_depth(uint32_t addr);
// Início de cena do GXM: a profundidade do alvo volta ao valor de fundo (1,0), a não ser que a
// superfície peça "force load". Sem isso a profundidade de uma cena vaza para a seguinte.
void begin_scene(uint32_t color_addr, uint8_t stencil = 0);
void shutdown();
// Resolução interna (múltiplo de 960x544): automática pelo monitor ou escolhida no menu do jogo.
uint32_t render_scale();
uint32_t auto_render_scale();
bool set_render_scale(uint32_t s);
// Contador de FPS no canto da imagem (opção do menu de pausa).
void set_fps_counter(bool on);
void draw_enemy_bars(uint32_t color_addr, const EnemyBar *bars, uint32_t n);
// Quadros apresentados por segundo (medidos a cada meio segundo).
uint32_t measured_fps();
// Janela sem bordas ocupando o monitor (o mesmo modo do F11 / Alt+Enter).
void set_borderless(bool on);
// VSync do menu de pausa: desligado, a apresentação não espera o vblank (modo imediato).
void set_vsync(bool on, bool fifo = false); // fifo: esperar o vblank do monitor (VSync com FPS máximo)
// Desenhos que gravaram profundidade no último quadro apresentado (0 em telas 2D, como o menu principal).
uint32_t last_frame_depth_writes();
// Teste: desliga o pós-processamento (iluminação de tela) enquanto ligado.
void set_post_bypass(bool on);
// Contadores do pós-processamento desde a última chamada: aplicados, sem matriz, sem ponto do HUD,
// alvo da tela, último desenho 3D, índice do HUD.
void post_stats(uint32_t out[6]);
void post_focus_values(float out[4]); // foco, alvo, distância e profundidade no centro (diagnóstico)
// Próximo present mostra esta imagem RGBA (cena remasterizada) no lugar do alvo do jogo. O ponteiro tem
// de valer até o present.
void set_video_frame(const uint8_t *rgba, uint32_t w, uint32_t h);
// Aviso de troféu igual ao do PS Vita (ícone, nível, nome), desenhado sobre o quadro.
void notify_trophy(const TrophyBanner &t);
bool is_borderless();
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
Stats take_stats();
Stats peek_stats(); // sem zerar (diagnóstico de travadas)
