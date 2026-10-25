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
// Palavra de estado: 0x10000 | textura | áudio << 2 | resolução << 4 | fps << 7 | vsync << 9 | QTE automático << 10 |
// contador de FPS << 11 | modo de exibição << 12 (sem bordas) | cheats: vida infinita << 13, magia infinita << 14, fúria
// infinita << 15 | FPS medidos << 17 (0 com o contador desligado; o clipe do contador no HUD mostra esse número). As alternâncias têm um bit (0 Off, 1 On). Texturas e áudio: 1 Off, 2 On, 3 indisponível. Resolução: 1..4
// (escala interna). FPS: 1 = 30, 2 = 60, 3 = máximo. As escolhas ficam em <dados>/vitart_options.ini.
#include "vitart.h"
#include <cmath>
#include <vector>
#include <chrono>
#include <filesystem>
#include <tuple>
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
int game_id(); // abaixo: 1 = GoW1, 2 = GoW2
int trophy_unlock_all(); // hle_sys.cpp: cheat "Unlock All Trophies"
void enemy_hp_tick(int game); // enemy_hp.cpp: barras de vida dos inimigos

void texpack_set_enabled(bool on);
bool texpack_available();
void ps2audio_set_enabled(bool on);
bool ps2audio_available();
void display_set_fps60(bool on);
void display_set_fps_target(uint32_t fps);
void display_set_fixed_step_addr(uint32_t a);
void display_set_in_menu(bool on); // hle_sys.cpp: fora da gameplay o ritmo fica em 60 FPS
bool input_kbm_active(); // hle_sys.cpp: o último controle usado foi o teclado/mouse
void kbm_icons_start();

namespace {

constexpr uint32_t kNameWide = 0x811f036c;    // "WideScreen" (confere que o módulo é o gow1)
// Canal com o menu, por jogo: objeto de opções (ponteiro global e vtable + 8), campo da palavra de estado e
// a chamada de strncmp do tratador de FSCommand com o endereço de "FSCommand:".
//   GoW1: PS2_Cheat (+0x2bc, só leitura para os scripts, registrada em sub_81014640).
//   GoW2: não tem Cheat; PS2_OverUnder (+0x3e0, o modo 3D acima/abaixo do PS3, sem uso no Vita). Objeto
//     criado em 0x8106c49e (construtor sub_81016fe0, vtable 0x8122e048), ponteiro em 0x812cb1b8; tratador de
//     FSCommand em 0x8111669e chamando strncmp (stub 0x811f5510) com "FSCommand:" em 0x8121e8d0.
struct OptProfile {
    uint32_t optr, vtbl, field, strncmp_stub, fs_prefix;
};
constexpr OptProfile kOptGow1{0x812a681c, 0x8120c534, 0x2bc, 0x811d1a70, 0x811fbdf4};
constexpr OptProfile kOptGow2{0x812cb1b8, 0x8122e050, 0x3e0, 0x811f5510, 0x8121e8d0};
const OptProfile *opt_profile();
struct Options {
    bool tex = true, audio = true, counter = false, borderless = false;
    uint32_t fps = 1; // 1 = 30, 2 = 60, 3 = máximo (taxa do monitor, sem acelerar o jogo)
    bool vsync = true; // desligado: sem limite de quadros e sem esperar o vblank
    uint32_t scale = 0; // 0 = automática pelo monitor
    bool auto_qte = false; // QTE automático (game_qte_auto)
    bool inf_health = false, inf_magic = false, inf_rage = false; // cheats (cheats_tick)
    bool hp_bars = true; // barras de vida dos inimigos (enemy_hp.cpp)
    bool mod_kratos = false; // mod "GoW3 Kratos" (vitart-data/mods/gow3_kratos, hle_io.cpp); vale ao reiniciar
};
Options g_opt;
// Pedidos vindos do menu (thread do jogo), aplicados no próximo quadro: 0 = nenhum.
std::atomic<uint32_t> g_req_tex{0}, g_req_aud{0}, g_req_res{0}, g_req_fps{0}, g_req_cnt{0}, g_req_dsp{0}, g_req_vsync{0}, g_req_qte{0},
    g_req_hp{0}, g_req_mp{0}, g_req_rage{0}, g_req_trophy{0}, g_req_kratos{0}, g_req_restart{0};

std::string ini_path() { return data_path() + "/vitart_options.ini"; }
std::string g_iso; // ISO do PS2 lembrada (o iniciador a passa em VITART_PS2_AUDIO)

void save() {
    std::string iso = g_iso;
    if (const char *e = getenv("VITART_PS2_AUDIO"); e && *e) iso = e;
    if (FILE *f = fopen(ini_path().c_str(), "w")) {
        fprintf(f, "# Opções do PC (mudam pelo menu de pausa do jogo: Options)\n");
        fprintf(f, "hd_textures=%u\nps2_audio=%u\nresolution=%u\nfps=%s\nvsync=%u\nfps_counter=%u\nborderless=%u\nauto_qte=%u\n",
                g_opt.tex ? 1u : 0u, g_opt.audio ? 1u : 0u, g_opt.scale, g_opt.fps == 3 ? "max" : g_opt.fps == 2 ? "60" : "30",
                g_opt.vsync ? 1u : 0u, g_opt.counter ? 1u : 0u, g_opt.borderless ? 1u : 0u,
                g_opt.auto_qte ? 1u : 0u);
        fprintf(f, "cheat_infinite_health=%u\ncheat_infinite_magic=%u\ncheat_infinite_rage=%u\n", g_opt.inf_health ? 1u : 0u,
                g_opt.inf_magic ? 1u : 0u, g_opt.inf_rage ? 1u : 0u);
        fprintf(f, "enemy_health_bars=%u\n", g_opt.hp_bars ? 1u : 0u);
        fprintf(f, "mod_gow3_kratos=%u\n", g_opt.mod_kratos ? 1u : 0u); // carregador de mods (hle_io.cpp)
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
            if (sscanf(line, "fps=%u", &v) == 1) g_opt.fps = v == 60 ? 2 : v > 60 ? 3 : 1;
            if (!strncmp(line, "fps=max", 7)) g_opt.fps = 3;
            if (sscanf(line, "fps_counter=%u", &v) == 1) g_opt.counter = v != 0;
            if (sscanf(line, "vsync=%u", &v) == 1) g_opt.vsync = v != 0;
            if (sscanf(line, "borderless=%u", &v) == 1) g_opt.borderless = v != 0;
            if (sscanf(line, "auto_qte=%u", &v) == 1) g_opt.auto_qte = v != 0;
            if (sscanf(line, "cheat_infinite_health=%u", &v) == 1) g_opt.inf_health = v != 0;
            if (sscanf(line, "cheat_infinite_magic=%u", &v) == 1) g_opt.inf_magic = v != 0;
            if (sscanf(line, "cheat_infinite_rage=%u", &v) == 1) g_opt.inf_rage = v != 0;
            if (sscanf(line, "enemy_health_bars=%u", &v) == 1) g_opt.hp_bars = v != 0;
            if (sscanf(line, "mod_gow3_kratos=%u", &v) == 1) g_opt.mod_kratos = v != 0;
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
    // áudio do PS2 só existe para o GoW1 (no GoW2 o menu mostra "Unavailable"); o pack de texturas, para os dois
    const bool g1 = opt_profile() == &kOptGow1;
    const uint32_t tex = texpack_available() ? (g_opt.tex ? 2u : 1u) : 3u; // pack do jogo em execução (texpack/<jogo>)
    const uint32_t aud = ps2audio_available() ? (g_opt.audio ? 2u : 1u) : 3u;
    uint32_t fps = g_opt.counter ? gvk::measured_fps() : 0;
    fps = fps > 127 ? 127 : fps; // a palavra passa por float no menu: até 2^24
    // bit 16: o mod GoW3 Kratos escolhido (o menu mostra a escolha; o WAD carregado muda ao reiniciar)
    return (g_opt.mod_kratos ? 0x10000u : 0u) | tex | aud << 2 | cur_scale() << 4 | g_opt.fps << 7 | (g_opt.vsync ? 1u : 0u) << 9 |
           (g_opt.auto_qte ? 1u : 0u) << 10 | (g_opt.counter ? 1u : 0u) << 11 | (g_opt.borderless ? 1u : 0u) << 12 |
           (g_opt.inf_health ? 1u : 0u) << 13 | (g_opt.inf_magic ? 1u : 0u) << 14 | (g_opt.inf_rage ? 1u : 0u) << 15 | fps << 17;
}

bool is_gow1();
bool is_gow2();
const OptProfile *opt_profile() { return is_gow1() ? &kOptGow1 : is_gow2() ? &kOptGow2 : nullptr; }
bool is_gow2() {
    static int ok = -1;
    if (ok < 0) {
        constexpr uint32_t kNameWide2 = 0x81214970; // "WideScreen" no gow2 (0x214a70 no ELF)
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(mem_base() + kNameWide2, &mi, sizeof mi) || mi.State != MEM_COMMIT) return (ok = 0) == 1;
        char name[11] = {};
        std::memcpy(name, ptr<char>(kNameWide2), 10);
        ok = !std::strcmp(name, "WideScreen") && !is_gow1();
    }
    return ok == 1;
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
    const OptProfile *op = opt_profile();
    if (!op || stub != op->strncmp_stub || ctx->r[1] != op->fs_prefix || !ctx->r[0]) return;
    const char *c = gstr(ctx->r[0]);
    if (std::strncmp(c, "FSCommand:VT_", 13) != 0) return;
    char key[8] = {};
    unsigned v = 0;
    if (std::sscanf(c + 13, "%7[a-z]_%u", key, &v) != 2 || !v) return; // até 7 letras ("vsync")
    if (!std::strcmp(key, "tex")) g_req_tex = v;
    if (!std::strcmp(key, "aud")) g_req_aud = v;
    if (!std::strcmp(key, "res")) g_req_res = v;
    if (!std::strcmp(key, "fps")) g_req_fps = v;
    if (!std::strcmp(key, "cnt")) g_req_cnt = v;
    if (!std::strcmp(key, "dsp")) g_req_dsp = v;
    if (!std::strcmp(key, "vsync")) g_req_vsync = v;
    if (!std::strcmp(key, "qte")) g_req_qte = v;
    if (!std::strcmp(key, "hp")) g_req_hp = v;
    if (!std::strcmp(key, "mp")) g_req_mp = v;
    if (!std::strcmp(key, "rage")) g_req_rage = v;
    if (!std::strcmp(key, "trophy")) g_req_trophy = v;
    if (!std::strcmp(key, "kratos")) g_req_kratos = v;
    if (!std::strcmp(key, "restart")) g_req_restart = v;
}

// Cheats (menu de pausa: Infinite Health / Magic / Rage), aplicados a cada quadro dentro do jogo.
//   GoW2: as opções do modo de desafio (FreeCombat_InfiniteHealth/Magic/GodMode, +0x2a4.. do objeto de opções) são
//     copiadas para as globais 0x812eb058/5c/60 quando um desafio começa (sub_8106d2c0) e zeradas na saída
//     (0x8106f744); a função de dano (sub_81037338) não tira vida do Kratos com a primeira ligada, e as de magia e
//     fúria (0x8104fa12.., 0x81051882..) não gastam o medidor. Fora dos desafios ninguém as liga: o cheat as mantém.
//   GoW1: não tem essas globais; a tabela de ajustes do jogador (0x812c27f4, carregada de "/Player" em sub_8103fb6e)
//     tem 6 fantasias de 0x20 bytes em +0x204 (+4 "AI Damage Mult" = dano dos inimigos, +8 "Infinite Magic") e 6
//     níveis de arma de 0x48 bytes em +0x54 (+0x3c "God Mode Meter Drain" = quanto a fúria esvazia por segundo).
//     Cheat ligado: dano dos inimigos 0, magia infinita 1, dreno 0; desligado: os valores originais de volta.
void cheats_tick(int game) {
    static bool was[3] = {};
    const bool on[3] = {g_opt.inf_health, g_opt.inf_magic, g_opt.inf_rage};
    if (game == 2) {
        for (int i = 0; i < 3; i++) {
            const uint32_t a = 0x812eb058u + 4 * i;
            if (on[i]) wr32(a, 1);
            else if (was[i]) wr32(a, 0); // só desliga o que o cheat ligou
            was[i] = on[i];
        }
        return;
    }
    if (game != 1) return;
    constexpr uint32_t kPlayer = 0x812c27f4u;
    static uint32_t orig_dmg[6], orig_mag[6], orig_drain[6];
    for (int k = 0; k < 6; k++) {
        const uint32_t cos = kPlayer + 0x204 + 0x20 * k, lvl = kPlayer + 0x54 + 0x48 * k;
        const uint32_t vals[3] = {cos + 4, cos + 8, lvl + 0x3c};
        uint32_t *orig[3] = {orig_dmg, orig_mag, orig_drain};
        const uint32_t cheat[3] = {0u, 1u, 0u};  // 0.0f, verdadeiro, 0.0f
        for (int i = 0; i < 3; i++) {
            if (on[i]) {
                if (!was[i]) orig[i][k] = rd32(vals[i]); // a tabela pode ser recarregada (fase nova): reescreve
                if (rd32(vals[i]) != cheat[i]) wr32(vals[i], cheat[i]);
            } else if (was[i]) {
                wr32(vals[i], orig[i][k]);
            }
        }
    }
    for (int i = 0; i < 3; i++) was[i] = on[i];
}
const bool g_watch = (watch_import(on_import), true);

// "Apply & Restart" (menu Mods): abre o mesmo executável com a mesma linha de comando e sai. Os mods trocam arquivos
// dentro do psarc (a TOC que o jogo lê no começo), então só valem num processo novo.
void restart_process() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = GetCommandLineW();
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    logf("vitart: reiniciando para aplicar os mods");
    if (CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
        ExitProcess(0);
    }
    logf("vitart: não deu para reiniciar (CreateProcess %lu)", GetLastError());
}

} // namespace

bool enemy_bars_enabled() { return g_opt.hp_bars; }

// Antes do primeiro quadro: aplica as escolhas salvas (texturas, áudio e resolução valem também no GoW2).
void game_options_init() {
    load();
    texpack_set_enabled(g_opt.tex);
    ps2audio_set_enabled(g_opt.audio);
    if (g_opt.scale) gvk::set_render_scale(g_opt.scale);
    gvk::set_fps_counter(g_opt.counter && !opt_profile()); // nos dois GoW o contador é o do HUD (FLP)
    gvk::set_borderless(g_opt.borderless);
}

void game_options_tick() {
    static bool inited = false;
    if (!inited) {
        inited = true;
        game_options_init();
        kbm_icons_start(); // ícones de teclado no HUD (kbm_icons.cpp)
    }
    // VITART_MEMSNAP=<pasta>:<s1>,<s2>,... (diagnóstico, para achar a vida e a magia): nos segundos pedidos despeja
    // a memória do guest em <pasta>/t<s> e anota os medidores do HUD (HealthMeter_Value/MagicMeter_Value, +0x14/+0x18
    // do objeto do HUD) em <pasta>/hud.txt.
    if (static const char *snap = getenv("VITART_MEMSNAP"); snap && game_id()) {
        static const auto t0 = std::chrono::steady_clock::now();
        static std::vector<int> when = [] {
            std::vector<int> v;
            const char *c = strrchr(getenv("VITART_MEMSNAP"), ':');
            for (c = c ? c + 1 : ""; *c; c = strchr(c, ',') ? strchr(c, ',') + 1 : "") v.push_back(atoi(c));
            return v;
        }();
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!when.empty() && s >= when.front()) {
            const std::string dir = std::string(snap, strrchr(snap, ':') ? strrchr(snap, ':') - snap : strlen(snap));
            const std::string sub = dir + "/t" + std::to_string(when.front());
            std::filesystem::create_directories(sub);
            const uint32_t hud = rd32(game_id() == 1 ? 0x812a681cu : 0x812cb1b8u);
            float hp = 0, mp = 0;
            if (hud) std::memcpy(&hp, ptr<uint8_t>(hud + 0x14), 4), std::memcpy(&mp, ptr<uint8_t>(hud + 0x18), 4);
            if (FILE *f = fopen((dir + "/hud.txt").c_str(), "a")) fprintf(f, "t%d hud=%08x hp=%g mp=%g\n", when.front(), hud, hp, mp), fclose(f);
            dump_regions(sub, 0x80000000u, 0xC0000000u);
            when.erase(when.begin());
        }
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
    // Tempo e ritmo, por jogo: endereços da estrutura de tempo e das palavras do remendo do dt
    // (tools/patch_gow1_dt.py, tools/patch_gow2_dt.py) e da flag do menu principal.
    struct TimeProfile {
        uint32_t floor, dt, menu; // piso do dt (float, centésimos de quadro), dt do quadro, = 1 no menu principal
    };
    static const TimeProfile kGow1{0x812b75d0, 0x812b75e0, 0x812bc0c0};
    static const TimeProfile kGow2{0x812df6d0, 0x812df6e0, 0x812e40e0}; // menu: 1 só no menu principal (memória comparada)
    const TimeProfile *tp = is_gow1() ? &kGow1 : is_gow2() ? &kGow2 : nullptr;
    if (tp) {
        // VITART_MEMSNAP="q1,q2,...": grava a memória global do jogo (0x81000000..0x81800000, só o mapeado)
        // nos quadros pedidos, em memsnap_<quadro>.bin (diagnóstico; assim foi achada a flag do menu)
        if (const char *ms = getenv("VITART_MEMSNAP")) {
            static uint32_t q = 0;
            q++;
            for (const char *e = ms; e && *e;) {
                if ((uint32_t)std::atoi(e) == q) {
                    char path[64];
                    std::snprintf(path, sizeof path, "memsnap_%05u.bin", q);
                    if (FILE *f = fopen(path, "wb")) {
                        std::vector<uint8_t> buf(0x800000, 0);
                        for (uint32_t a = 0x81000000; a < 0x81800000;) {
                            MEMORY_BASIC_INFORMATION mi{};
                            if (!VirtualQuery(mem_base() + a, &mi, sizeof mi)) break;
                            const uint32_t end = (uint32_t)std::min<uint64_t>(0x81800000ull, (uint64_t)((uint8_t *)mi.BaseAddress - mem_base()) + mi.RegionSize);
                            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                                std::memcpy(&buf[a - 0x81000000], mem_base() + a, end - a);
                            a = end;
                        }
                        fwrite(buf.data(), 1, buf.size(), f);
                        fclose(f);
                    }
                }
                e = std::strchr(e, ',');
                if (e) e++;
            }
        }
        // VITART_TIMEDUMP=1 (depuração): a soma do dt do jogo contra o relógio, a cada 10 s
        if (getenv("VITART_TIMEDUMP")) {
            static double game_t = 0;
            static uint32_t nframes = 0;
            static auto t0 = std::chrono::steady_clock::now();
            float dtv;
            const uint32_t raw = rd32(tp->dt);
            std::memcpy(&dtv, &raw, 4);
            if (std::isfinite(dtv) && dtv > 0 && dtv < 1) game_t += dtv, nframes++;
            const double real = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (real >= 10) {
                logf("vitart: tempo do jogo %.2f s em %.2f s reais (%u quadros, razão %.3f), menu %u", game_t, real, nframes,
                     game_t / real, tp->menu ? rd32(tp->menu) : 0u);
                game_t = 0, nframes = 0, t0 = std::chrono::steady_clock::now();
            }
        }
        // 30/60 FPS: o jogo espera 2 vblanks por quadro e calcula dt = max(60/taxa, clamp(tempo real em
        // quadros, 1, 4)) / 60. Com 1 vblank o dt real é 1/60: o dobro de quadros na mesma velocidade. Acima de
        // 60 o piso de 1 quadro fazia o jogo acelerar; a DLL remendada lê o piso em tp->floor.
        display_set_fps60(g_opt.fps >= 2);
        static uint32_t refresh = 0;
        if (!refresh) {
            DEVMODEW dm{};
            dm.dmSize = sizeof dm;
            refresh = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) ? dm.dmDisplayFrequency : 60;
            if (refresh <= 61) refresh = 60; // VSync ligado: no máximo a taxa do monitor
            if (refresh > 240) refresh = 240;
        }
        // VSync desligado: sem limite (o jogo roda o mais rápido que der) e piso do dt em 1/3000 s;
        // ligado: 30, 60 ou a taxa do monitor
        // VSync com FPS máximo: a apresentação em FIFO espera o vblank do monitor e dá o ritmo sozinha (um limite por
        // software na mesma taxa brigava com o do monitor); 30/60 continuam pelo relógio do vitart (60 não divide 74 Hz)
        const bool fifo = g_opt.vsync && g_opt.fps == 3 && !getenv("VITART_NOFIFO");
        uint32_t target = !g_opt.vsync || fifo ? UINT32_MAX : g_opt.fps == 3 ? refresh : 0;
        if (const char *e = getenv("VITART_FPS")) target = (uint32_t)std::atoi(e); // teste
        gvk::set_vsync(g_opt.vsync, fifo);
        // o menu principal anima por quadro, não pelo tempo: nele o ritmo é o do jogo mesmo sem VSync
        // (GoW1: 0x812bc0c0 = 1 só no menu principal, achado comparando a memória global nas fases)
        display_set_fixed_step_addr(tp->menu);
        // estado do jogo (o mesmo do QTE automático): GoW1 0x812c2e04 = 0xb jogando; GoW2 0x812ec498 = 0xc jogando.
        // Qualquer outro (pausa, Power Up, carregamento, cenas) anima por quadro: 60 FPS no máximo.
        display_set_in_menu(rd32(game_id() == 1 ? 0x812c2e04u : 0x812ec498u) != (game_id() == 1 ? 0xbu : 0xcu));
        display_set_fps_target(target);
        const float floor = fifo ? 25.f : target == UINT32_MAX ? 2.f : target > 60 ? 25.f : 0.f; // centésimos de quadro de 60 Hz
        uint32_t fb;
        std::memcpy(&fb, &floor, 4);
        if (rd32(tp->floor) != fb) wr32(tp->floor, fb);
    }
    const OptProfile *op = opt_profile();
    if (!op) return;
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
    // VITART_TEST_NOTIFY (teste): mostra o aviso de troféu no quadro 400
    if (static const char *tn = getenv("VITART_TEST_NOTIFY"); tn) {
        static uint32_t n = 0;
        if (++n == 400)
            gvk::notify(std::string("Troféu desbloqueado (Bronze)") + char(10) + "Zero Health = Bronze Trophy" + char(10) +
                        "Open a Health Chest when your health meter is already full");
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
    if (const uint32_t v = g_req_fps.exchange(0); v >= 1 && v <= 3 && v != g_opt.fps) {
        g_opt.fps = v, changed = true;
        logf("vitart: FPS %s", v == 3 ? "máximo" : v == 2 ? "60" : "30");
    }
    if (const uint32_t v = g_req_vsync.exchange(0); (v == 1 || v == 2) && (v == 2) != g_opt.vsync) {
        g_opt.vsync = v == 2, changed = true;
        logf("vitart: VSync %s", g_opt.vsync ? "ligado" : "desligado");
    }
    if (const uint32_t v = g_req_qte.exchange(0); (v == 1 || v == 2) && (v == 2) != g_opt.auto_qte) {
        g_opt.auto_qte = v == 2, changed = true;
        logf("vitart: QTE automático %s", g_opt.auto_qte ? "ligado" : "desligado");
    }
    if (const uint32_t v = g_req_cnt.exchange(0); (v == 1 || v == 2) && (v == 2) != g_opt.counter) {
        g_opt.counter = v == 2, changed = true;
    }
    for (auto [req, opt, what] : {std::tuple{&g_req_hp, &g_opt.inf_health, "vida infinita"},
                                  std::tuple{&g_req_mp, &g_opt.inf_magic, "magia infinita"},
                                  std::tuple{&g_req_rage, &g_opt.inf_rage, "fúria infinita"}})
        if (const uint32_t v = req->exchange(0); (v == 1 || v == 2) && (v == 2) != *opt) {
            *opt = v == 2, changed = true;
            logf("vitart: cheat %s %s", what, *opt ? "ligado" : "desligado");
        }
    if (g_req_trophy.exchange(0) == 2) trophy_unlock_all();
    if (const uint32_t v = g_req_kratos.exchange(0); v == 1 || v == 2) {
        g_opt.mod_kratos = v == 2, changed = true;
        logf("vitart: mod GoW3 Kratos %s (vale ao reiniciar)", g_opt.mod_kratos ? "ligado" : "desligado");
    }
    if (g_req_restart.exchange(0) == 2) {
        save();
        restart_process(); // aplica os mods: o psarc é aberto (e remendado) de novo do começo
    }
    cheats_tick(game_id());
    enemy_hp_tick(game_id());
    if (changed) save();
    // estado para o menu mostrar
    const uint32_t obj = rd32(op->optr);
    if (getenv("VITART_OPTLOG")) {
        static uint32_t n = 0, prev = 0, overwritten = 0;
        if (obj && rd32(obj) == op->vtbl && prev && rd32(obj + op->field) != prev) overwritten++;
        if (n++ % 600 == 0)
            logf("vitart: opções: objeto 0x%08x vtable 0x%08x (esperada 0x%08x), campo 0x%08x, sobrescrito pelo jogo %u vezes",
                 obj, obj ? rd32(obj) : 0, op->vtbl, obj ? rd32(obj + op->field) : 0, overwritten);
        prev = encode();
    }
    if (!obj || rd32(obj) != op->vtbl) return;
    const uint32_t want = encode();
    if (rd32(obj + op->field) != want) wr32(obj + op->field, want);
}

// Jogo carregado: 1 = GoW1, 2 = GoW2, 0 = outro (o menu da coletânea).
int game_id() { return is_gow1() ? 1 : is_gow2() ? 2 : 0; }

// Dentro de um dos jogos e fora do menu principal (flags achadas comparando a memória global).
bool game_in_gameplay() {
    const int g = game_id();
    return g && rd32(g == 1 ? 0x812bc0c0u : 0x812e40e0u) != 1;
}

// QTE automático: o botão que o HUD está pedindo vira entrada, somada à do jogador.
//   GoW1: sub_8101406c(hud, índice) mostra o ícone e guarda o índice em hud+0x1570 (-1 = nenhum); hud é o
//     objeto em 0x812a681c (o mesmo das opções). Índices = tabela de nomes em 0x812a6858: 0 X, 1 triângulo,
//     2 círculo, 3 quadrado, 4..7 meia-volta do analógico (Up180CW, Up180CCW, Down180CW, Down180CCW), 8 360CW,
//     9 360CCW, 10 X repetido, 11 R2 repetido (portas), 12 sacudir o analógico, 13 vaivém, 14 R2 parado.
//     Estado do jogo em 0x812c2e04 (máquina de estados de sub_8105fc..810614xx): 0xb = jogo correndo, 0xc = pausa
//     (achado comparando a memória pausada e não pausada); fora do 0xb nada é apertado.
//   GoW2: sub_81070b60(slot, valor) grava o ícone em 0x812ec588 + 4*slot; o valor é o hash do nome do
//     clipe (h = h*127 + maiúscula, sub_811346de): "PB_XBtn", "PB_360CW", "PB_CircleBtnSmash"...
// Botões no SceCtrlData do modo PS TV (L2/R2 = 0x100/0x200, L1/R1 = 0x400/0x800).
namespace {
enum QteKind { QNone, QPress, QHold, QCw, QCcw, QWiggleX, QWiggleY };
struct Qte {
    QteKind kind;
    uint32_t buttons;
};
uint32_t gow_hash(const char *s) {
    uint32_t h = 0;
    for (; *s; s++) h = h * 127 + (uint32_t)(int8_t)(*s >= 'a' && *s <= 'z' ? *s - 32 : *s);
    return h;
}
Qte qte_gow1(uint32_t idx) {
    static const Qte t[15] = {{QPress, 0x4000}, {QPress, 0x1000}, {QPress, 0x2000}, {QPress, 0x8000}, {QCw, 0},
                              {QCcw, 0},        {QCw, 0},         {QCcw, 0},        {QCw, 0},         {QCcw, 0},
                              {QPress, 0x4000}, {QPress, 0x200},  {QWiggleX, 0},    {QWiggleY, 0},    {QNone, 0}};
    return idx < 15 ? t[idx] : Qte{QNone, 0};
}
Qte qte_gow2(uint32_t hash, bool &known) {
    struct Name {
        const char *name;
        Qte q;
    };
    static const Name names[] = {
        {"PB_XBtn", {QPress, 0x4000}},        {"PB_TriangleBtn", {QPress, 0x1000}}, {"PB_CircleBtn", {QPress, 0x2000}},
        {"PB_SquareBtn", {QPress, 0x8000}},   {"PB_Up180CW", {QCw, 0}},              {"PB_Up180CCW", {QCcw, 0}},
        {"PB_Down180CW", {QCw, 0}},           {"PB_Down180CCW", {QCcw, 0}},          {"PB_360CW", {QCw, 0}},
        {"PB_360CCW", {QCcw, 0}},             {"PB_Stick360CW", {QCw, 0}},           {"PB_Stick360CCW", {QCcw, 0}},
        {"PB_Stick360CWF", {QCw, 0}},         {"PB_Stick360CCWF", {QCcw, 0}},        {"PB_StickShake", {QWiggleX, 0}},
        {"PB_StickBack", {QWiggleY, 0}},      {"PB_CircleBtnSmash", {QPress, 0x2000}}, {"PB_CircleBtnHold", {QHold, 0x2000}},
        {"PB_R1BtnHold", {QHold, 0x800}},     {"PB_R1BtnStatic", {QNone, 0}},        {"PB_CircleBtnStatic", {QNone, 0}},
        {"PB_CSBtnFail", {QNone, 0}},
    };
    static std::vector<std::pair<uint32_t, Qte>> table;
    if (table.empty())
        for (const Name &n : names) table.push_back({gow_hash(n.name), n.q});
    known = false;
    for (auto &[h, q] : table)
        if (h == hash) return known = true, q;
    return {QNone, 0};
}
} // namespace

uint32_t game_qte_auto(uint8_t &lx, uint8_t &ly) {
    const int g = game_id();
    if (!g || (!g_opt.auto_qte && !getenv("VITART_AUTO_QTE"))) return 0;
    Qte q{QNone, 0};
    static const bool log = getenv("VITART_QTELOG") != nullptr;
    static uint32_t last_seen = 0xffffffffu;
    uint32_t seen = 0xffffffffu;
    if (g == 1) {
        const uint32_t hud = rd32(0x812a681c);
        if (!hud || rd32(hud) != kOptGow1.vtbl) return 0;
        const uint32_t idx = rd32(hud + 0x1570);
        const bool menu = rd32(0x812c2e04) != 0xb; // estado do jogo: 0xb = correndo (0xc pausa, outros = menus e cenas)
        seen = menu ? 0xfffffffeu : idx;
        if (!menu) q = qte_gow1(idx);
    } else {
        if (rd32(0x812ec498) != 0xc) return 0; // estado do jogo no GoW2: 0xc = correndo, 0xd = pausa (memória comparada)
        for (uint32_t s = 0; s < 8 && q.kind == QNone; s++) {
            const uint32_t v = rd32(0x812ec588 + 4 * s);
            if (!v) continue;
            bool known;
            q = qte_gow2(v, known);
            if (q.kind != QNone || !known) seen = v;
            if (log && !known) {
                static uint32_t unknown_logged = 0;
                if (v != unknown_logged) unknown_logged = v, logf("vitart: QTE: ícone desconhecido no slot %u: hash 0x%08x", s, v);
            }
        }
    }
    if (log && seen != last_seen) logf("vitart: QTE: %s 0x%08x -> tipo %d botões 0x%x", g == 1 ? "índice" : "hash", seen, (int)q.kind, q.buttons);
    last_seen = seen;
    if (q.kind == QNone) return 0;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    switch (q.kind) {
    case QPress: return std::fmod(t, 0.07) < 0.035 ? q.buttons : 0; // ~14 toques por segundo (serve para os repetidos)
    case QHold: return q.buttons;
    case QCw:
    case QCcw: { // 2,5 voltas por segundo; horário na tela = cima, direita, baixo, esquerda
        const double a = t * 2.5 * 6.283185307 * (q.kind == QCw ? 1 : -1);
        lx = (uint8_t)std::lround(128 + 127 * std::sin(a));
        ly = (uint8_t)std::lround(128 - 127 * std::cos(a));
        return 0;
    }
    case QWiggleX: lx = std::fmod(t, 0.12) < 0.06 ? 0 : 255; return 0;
    case QWiggleY: ly = std::fmod(t, 0.12) < 0.06 ? 0 : 255; return 0;
    default: return 0;
    }
}

} // namespace vt
