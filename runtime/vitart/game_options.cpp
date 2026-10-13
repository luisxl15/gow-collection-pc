// Opções do PC dentro do menu de pausa do God of War 1 (Options > HD Textures / PS2 Audio / Resolution /
// Frame Rate).
//
// O menu é o FLP do próprio jogo, modificado por tools/gow_menu_mod.py (servido por cima do psarc, ver
// hle_io.cpp). Ida e volta:
//   motor -> menu: uma palavra de estado na variável PS2_Cheat, que o código do jogo exporta (só leitura
//     para os scripts) a partir do campo +0x2bc do objeto de opções (ponteiro global em 0x812a681c,
//     registro em sub_81014640); nada mais no jogo usa esse campo e o save não o grava. Os scripts do
//     menu decodificam a palavra para mostrar os valores ao abrir a página.
//   menu -> motor: os scripts mandam fscommand 'FSCommand:VT_<opção>_<valor>'. O tratador de FSCommand do
//     jogo (sub_810e4072) começa com strncmp(comando, "FSCommand:", 10), uma chamada de import; o vitart
//     observa essa chamada e lê o comando. (As variáveis do PS2 que sobraram no Vita não servem: as
//     graváveis têm tratadores no jogo -- SoundMode reconfigura o som e esgotou a memória de vídeo,
//     WideScreen mexe na proporção -- e a PS2_Cheat é só de leitura.)
//
// Palavra de estado: 0x10000 | textura | áudio << 2 | resolução << 4 | fps << 7 | proporção << 9 |
// contador de FPS << 12 (1 Off, 2 On) | modo de exibição << 14 (1 janela, 2 sem bordas) | FPS medidos << 17 (0 com o contador desligado; o clipe do
// contador no HUD, no estilo do contador de HITS, mostra esse número).
// Texturas e áudio: 1 Off, 2 On, 3 indisponível. Resolução: 1..4 (escala interna). FPS: 1 = 30, 2 = 60.
// Proporção: reservada (1 = 16:9). As escolhas ficam em <dados>/vitart_options.ini.
#include "vitart.h"
#include "gxm_vk.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace vt {

void texpack_set_enabled(bool on);
bool texpack_available();
void ps2audio_set_enabled(bool on);
bool ps2audio_available();
void display_set_fps60(bool on);

namespace {

constexpr uint32_t kOptionsPtr = 0x812a681c;  // ponteiro para o objeto de opções (gow1)
constexpr uint32_t kOptionsVtbl = 0x8120c534; // vtable do objeto (0x8120c52c + 8)
constexpr uint32_t kNameWide = 0x811f036c;    // "WideScreen" (confere que o módulo é o gow1)
constexpr uint32_t kState = 0x2bc;            // PS2_Cheat
constexpr uint32_t kStrncmpStub = 0x811d1a70; // import strncmp chamado pelo tratador de FSCommand
constexpr uint32_t kFsPrefix = 0x811fbdf4;    // "FSCommand:"
struct Options {
    bool tex = true, audio = true, fps60 = false, counter = false, borderless = false;
    uint32_t scale = 0; // 0 = automática pelo monitor
};
Options g_opt;
// Pedidos vindos do menu (thread do jogo), aplicados no próximo quadro: 0 = nenhum.
std::atomic<uint32_t> g_req_tex{0}, g_req_aud{0}, g_req_res{0}, g_req_fps{0}, g_req_cnt{0}, g_req_dsp{0};

std::string ini_path() { return data_path() + "/vitart_options.ini"; }
std::string g_iso; // ISO do PS2 lembrada (o iniciador a passa em VITART_PS2_AUDIO)

void save() {
    std::string iso = g_iso;
    if (const char *e = getenv("VITART_PS2_AUDIO"); e && *e) iso = e;
    if (FILE *f = fopen(ini_path().c_str(), "w")) {
        fprintf(f, "# Opções do PC (mudam pelo menu de pausa do jogo: Options)\n");
        fprintf(f, "hd_textures=%u\nps2_audio=%u\nresolution=%u\nfps=%u\nfps_counter=%u\nborderless=%u\n",
                g_opt.tex ? 1u : 0u, g_opt.audio ? 1u : 0u, g_opt.scale, g_opt.fps60 ? 60u : 30u, g_opt.counter ? 1u : 0u,
                g_opt.borderless ? 1u : 0u);
        if (!iso.empty()) fprintf(f, "ps2_iso=%s\n", iso.c_str());
        fclose(f);
    }
}

void load() {
    if (FILE *f = fopen(ini_path().c_str(), "r")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            unsigned v = 0;
            if (sscanf(line, "hd_textures=%u", &v) == 1) g_opt.tex = v != 0;
            if (sscanf(line, "ps2_audio=%u", &v) == 1) g_opt.audio = v != 0;
            if (sscanf(line, "resolution=%u", &v) == 1) g_opt.scale = v <= 4 ? v : 0;
            if (sscanf(line, "fps=%u", &v) == 1) g_opt.fps60 = v == 60;
            if (sscanf(line, "fps_counter=%u", &v) == 1) g_opt.counter = v != 0;
            if (sscanf(line, "borderless=%u", &v) == 1) g_opt.borderless = v != 0;
            if (!strncmp(line, "ps2_iso=", 8)) {
                g_iso = line + 8;
                while (!g_iso.empty() && (unsigned char)g_iso.back() < 32) g_iso.pop_back();
            }
        }
        fclose(f);
    }
    save(); // grava o caminho da ISO quando ele veio do iniciador
}

uint32_t cur_scale() {
    const uint32_t s = g_opt.scale ? g_opt.scale : gvk::auto_render_scale();
    return s < 1 ? 1 : s > 4 ? 4 : s;
}

uint32_t encode() {
    const uint32_t tex = texpack_available() ? (g_opt.tex ? 2u : 1u) : 3u;
    const uint32_t aud = ps2audio_available() ? (g_opt.audio ? 2u : 1u) : 3u;
    uint32_t fps = g_opt.counter ? gvk::measured_fps() : 0;
    fps = fps > 127 ? 127 : fps; // a palavra passa por float no menu: até 2^24
    return 0x10000u | tex | aud << 2 | cur_scale() << 4 | (g_opt.fps60 ? 2u : 1u) << 7 | 1u << 9 |
           (g_opt.counter ? 2u : 1u) << 12 | (g_opt.borderless ? 2u : 1u) << 14 | fps << 17;
}

bool is_gow1() {
    static int ok = -1;
    if (ok < 0) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(mem_base() + kNameWide, &mi, sizeof mi) || mi.State != MEM_COMMIT) return (ok = 0) == 1;
        char name[11] = {};
        std::memcpy(name, ptr<char>(kNameWide), 10);
        ok = !std::strcmp(name, "WideScreen");
    }
    return ok == 1;
}

void on_import(arc_ctx *ctx, uint32_t stub) {
    if (stub != kStrncmpStub || ctx->r[1] != kFsPrefix || !ctx->r[0]) return;
    const char *c = gstr(ctx->r[0]);
    if (std::strncmp(c, "FSCommand:VT_", 13) != 0) return;
    char key[8] = {};
    unsigned v = 0;
    if (std::sscanf(c + 13, "%3[a-z]_%u", key, &v) != 2 || !v) return;
    if (!std::strcmp(key, "tex")) g_req_tex = v;
    if (!std::strcmp(key, "aud")) g_req_aud = v;
    if (!std::strcmp(key, "res")) g_req_res = v;
    if (!std::strcmp(key, "fps")) g_req_fps = v;
    if (!std::strcmp(key, "cnt")) g_req_cnt = v;
    if (!std::strcmp(key, "dsp")) g_req_dsp = v;
}
const bool g_watch = (watch_import(on_import), true);

} // namespace

// Antes do primeiro quadro: aplica as escolhas salvas (texturas, áudio e resolução valem também no GoW2).
void game_options_init() {
    load();
    texpack_set_enabled(g_opt.tex);
    ps2audio_set_enabled(g_opt.audio);
    if (g_opt.scale) gvk::set_render_scale(g_opt.scale);
    gvk::set_fps_counter(g_opt.counter && !is_gow1());
    gvk::set_borderless(g_opt.borderless);
}

void game_options_tick() {
    static bool inited = false;
    if (!inited) {
        inited = true;
        game_options_init();
    }
    // F11 / Alt+Enter também mudam o modo de exibição: a escolha acompanha (vale em todos os módulos)
    static bool applied_borderless = g_opt.borderless;
    if (const uint32_t v = g_req_dsp.exchange(0); v == 1 || v == 2) {
        g_opt.borderless = applied_borderless = v == 2;
        gvk::set_borderless(g_opt.borderless);
        save();
    } else {
        // janela diferente da escolha por alguns quadros seguidos = mudou por tecla (o pedido já aplicado
        // leva um quadro para valer)
        static int settle = 0;
        if (gvk::is_borderless() == applied_borderless) settle = 0;
        else if (++settle > 3) g_opt.borderless = applied_borderless = gvk::is_borderless(), save(), settle = 0;
    }
    if (!is_gow1()) return;
    // 60 FPS: o jogo espera 2 vblanks por quadro e calcula dt = max(tempo real do quadro, 1/60) (estrutura
    // em 0x812b75dc; dt usado em 0x812b75f0). Com 1 vblank o dt real é 1/60: o dobro de quadros na mesma
    // velocidade. Acima de 60 o dt trava em 1/60 e o jogo acelera, por isso só 30 ou 60.
    display_set_fps60(g_opt.fps60);
    // VITART_RES_AT="quadro:escala,..." (teste): troca a resolução ao vivo em quadros definidos, como o menu.
    {
        static uint32_t frame = 0;
        static const char *at = getenv("VITART_RES_AT");
        frame++;
        for (const char *e = at; e && *e;) {
            unsigned fr = 0, sc = 0;
            if (std::sscanf(e, "%u:%u", &fr, &sc) == 2 && fr == frame) g_req_res = sc;
            e = std::strchr(e, ',');
            if (e) e++;
        }
    }
    // pedidos do menu
    bool changed = false;
    if (const uint32_t v = g_req_tex.exchange(0); v && texpack_available() && (v == 2) != g_opt.tex)
        g_opt.tex = v == 2, texpack_set_enabled(g_opt.tex), changed = true;
    if (const uint32_t v = g_req_aud.exchange(0); v && ps2audio_available() && (v == 2) != g_opt.audio)
        g_opt.audio = v == 2, ps2audio_set_enabled(g_opt.audio), changed = true;
    if (const uint32_t v = g_req_res.exchange(0); v >= 1 && v <= 4 && v != cur_scale()) {
        g_opt.scale = v, changed = true;
        gvk::set_render_scale(v);
        logf("vitart: resolução interna %ux (%ux%u)", v, 960 * v, 544 * v);
    }
    if (const uint32_t v = g_req_fps.exchange(0); (v == 1 || v == 2) && (v == 2) != g_opt.fps60) {
        g_opt.fps60 = v == 2, changed = true;
        logf("vitart: %u FPS", g_opt.fps60 ? 60u : 30u);
    }
    if (const uint32_t v = g_req_cnt.exchange(0); (v == 1 || v == 2) && (v == 2) != g_opt.counter) {
        g_opt.counter = v == 2, changed = true;
    }
    if (changed) save();
    // estado para o menu mostrar
    const uint32_t obj = rd32(kOptionsPtr);
    if (!obj || rd32(obj) != kOptionsVtbl) return;
    const uint32_t want = encode();
    if (rd32(obj + kState) != want) wr32(obj + kState, want);
}

} // namespace vt
