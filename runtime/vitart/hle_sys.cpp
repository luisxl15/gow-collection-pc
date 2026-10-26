// HLE dos serviços de sistema do Vita para o vitart: áudio (SceAudio; o NGS fica em hle_ngs.cpp),
// entrada (SceCtrl, SceTouch, SceMotion), exibição, relógio (SceRtc, libc), tubos de mensagem, Fios
// overlays, NP, troféus, AppUtil e diálogos comuns. Assinaturas e layouts das estruturas: vita-headers.
#include "vitart.h"
#include "host_input.h"
#include "host_audio.h"
#include "gxm_vk.h"
#include "trophy_banner.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <vector>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma comment(lib, "user32.lib")
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace vt {

constexpr uint32_t SCE_OK = 0;

static uint32_t zalloc(uint32_t size, const char *what) {
    const uint32_t a = galloc(Pool::user, size, 0x10, what);
    std::memset(ptr<void>(a), 0, size);
    return a;
}

// =================================================================== SceAudio
// Saída pelo motor WASAPI de host_audio.cpp (um fluxo, uma fila por porta). A cadência do jogo vem da
// placa: sceAudioOutOutput entrega o bloco e espera enquanto a fila da porta passar de kAhead quadros
// além do próprio bloco (~21 ms + o bloco; com o buffer de 20 ms do WASAPI, ~50 ms do jogo ao alto-falante).
// Sem placa (VITART_MUTE ou falha do WASAPI), cadência pelo relógio do PC, como o hardware.
constexpr uint32_t kAhead = 1024;
struct AudioPort {
    uint32_t len = 256, freq = 48000, mode = 1;
    int host = 0; // porta no motor de som (0 = sem placa)
    uint64_t outputs = 0;
    int peak = 0; // maior amplitude enviada (diagnóstico)
    int vol[2] = { 32768, 32768 };
    std::chrono::steady_clock::time_point next{};
};
static std::mutex g_audio_mutex;
static int g_next_port = 0;
static std::map<int, AudioPort> g_ports;
void audio_silence() { hostaudio::silence(); }

VT_HLE(sceAudioOutOpenPort) {
    // (type, len, freq, mode)
    const bool dev = hostaudio::start();
    std::lock_guard<std::mutex> g(g_audio_mutex);
    const int id = g_next_port++;
    AudioPort &p = g_ports[id];
    p.len = arg(ctx, 1);
    p.freq = arg(ctx, 2) ? arg(ctx, 2) : 48000;
    p.mode = arg(ctx, 3);
    p.next = std::chrono::steady_clock::now();
    if (dev) p.host = hostaudio::open_port(p.freq);
    logf("vitart: porta de áudio %d: tipo %u, %u amostras, %u Hz, %s (lr 0x%08x)", id, arg(ctx, 0), p.len, p.freq,
         p.mode ? "estéreo" : "mono", ctx->r[14]);
    ret(ctx, (uint32_t)id);
}
VT_HLE(sceAudioOutReleasePort) {
    std::lock_guard<std::mutex> g(g_audio_mutex);
    auto it = g_ports.find((int)arg(ctx, 0));
    if (it == g_ports.end()) return ret(ctx, 0x80260105u);
    if (getenv("VITART_AUDIO_LOG"))
        logf("vitart: porta de áudio %d liberada após %llu saídas, pico %d (lr 0x%08x)", (int)arg(ctx, 0),
             (unsigned long long)it->second.outputs, it->second.peak, ctx->r[14]);
    if (it->second.host) hostaudio::close_port(it->second.host);
    g_ports.erase(it);
    ret(ctx, SCE_OK);
}
VT_HLE(sceAudioOutSetVolume) {
    // (port, flag: 1 = esquerdo, 2 = direito, vol[2]: 0..32768)
    std::lock_guard<std::mutex> g(g_audio_mutex);
    auto it = g_ports.find((int)arg(ctx, 0));
    if (it == g_ports.end()) return ret(ctx, 0x80260105u);
    const uint32_t flag = arg(ctx, 1), v = arg(ctx, 2);
    for (int c = 0; c < 2; c++)
        if ((flag >> c) & 1) {
            const int x = v ? (int)rd32(v + 4 * c) : 32768;
            it->second.vol[c] = x < 0 ? 0 : x > 32768 ? 32768 : x;
        }
    ret(ctx, SCE_OK);
}
VT_HLE(sceAudioOutGetRestSample) {
    std::lock_guard<std::mutex> g(g_audio_mutex);
    auto it = g_ports.find((int)arg(ctx, 0));
    if (it == g_ports.end()) return ret(ctx, 0x80260105u);
    const AudioPort &p = it->second;
    if (p.host) return ret(ctx, std::min<uint32_t>(hostaudio::queued(p.host), p.len));
    const auto now = std::chrono::steady_clock::now();
    if (p.next <= now) return ret(ctx, 0);
    const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(p.next - now).count();
    const uint64_t rest = us * p.freq / 1000000ull;
    ret(ctx, (uint32_t)(rest < p.len ? rest : p.len));
}
VT_HLE(sceAudioOutOutput) {
    // A thread que alimenta o áudio (a mesma do update do NGS) roda com prioridade alta.
    static thread_local bool raised = false;
    if (!raised) raised = true, SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    int host = 0;
    uint32_t len = 0;
    std::chrono::steady_clock::time_point wake;
    {
        std::lock_guard<std::mutex> g(g_audio_mutex);
        auto it = g_ports.find((int)arg(ctx, 0));
        if (it == g_ports.end()) return ret(ctx, 0x80260105u); // SCE_AUDIO_OUT_ERROR_INVALID_PORT
        AudioPort &p = it->second;
        p.outputs++;
        host = p.host, len = p.len;
        const auto now = std::chrono::steady_clock::now();
        if (p.next < now - std::chrono::milliseconds(100)) p.next = now; // após uma pausa longa
        wake = p.next;
        p.next += std::chrono::microseconds((uint64_t)p.len * 1000000ull / p.freq);
        if (const uint32_t buf = arg(ctx, 1)) {
            const int16_t *src = ptr<int16_t>(buf);
            const bool stereo = p.mode != 0;
            for (uint32_t i = 0; i < p.len * (stereo ? 2u : 1u); i++) p.peak = (std::max)(p.peak, std::abs((int)src[i]));
            if (host) hostaudio::write(host, src, p.len, stereo, p.vol[0], p.vol[1]);
        }
        static const bool alog = getenv("VITART_AUDIO_LOG") != nullptr;
        if (alog && p.outputs % 500 == 0) {
            const hostaudio::Stats st = hostaudio::stats();
            logf("vitart: porta de áudio %d: %llu saídas, pico %d, fila %u quadros; motor: %llu quadros, %llu faltas de dados, buffer %u ms",
                 (int)arg(ctx, 0), (unsigned long long)p.outputs, p.peak, host ? hostaudio::queued(host) : 0,
                 (unsigned long long)st.mixed, (unsigned long long)st.underruns, st.latency_ms);
            p.peak = 0;
        }
    }
    if (!host) {
        std::this_thread::sleep_until(wake);
        return ret(ctx, SCE_OK);
    }
    // Cadência pela placa: espera a fila da porta baixar (até ~200 ms, para não travar sem placa).
    for (int spins = 0; spins < 200 && hostaudio::queued(host) > len + kAhead; spins++)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ret(ctx, SCE_OK);
}

// =================================================================== entrada
// Teclado e mouse no esquema do God of War (2018) para PC (padrões do jogo: Shacknews e Magic Game World),
// com a ação de cada tecla levada ao botão que faz o mesmo nos GoW do Vita:
//   WASD = analógico esquerdo (andar)          Shift + WASD = analógico direito (esquiva/rolamento)
//   botão esquerdo = quadrado (ataque leve)    botão direito = triângulo (ataque pesado)
//   botão do meio / F = círculo (agarrar)      Shift + botão do meio / V = L3+R3 (fúria: Rage of the Gods)
//   Espaço = X (pular; esquiva no GoW 2018)    Q = L1 (defender/aparar)
//   Ctrl esquerdo = L2 (mirar no 2018)         E = interagir (R2 no GoW1: baús, portas; R1 no GoW2)
//   R = o outro R (R1 no GoW1, R2 no GoW2)     1 2 3 4 / setas = direcional (magias)
//   Esc / Backspace = SELECT (pausa nos dois)  I / K / Tab / Enter = START (Power Up)
//   C = L1+R1 no GoW2 (Amuleto das Moiras: congela o tempo)
// Só com a janela do jogo em foco (o teclado é global no Windows). Controles (XInput, DualShock 4,
// DualSense) em host_input.cpp, somados. O último usado decide os ícones dos botões (input_kbm_active).
// Modo PS TV por padrão (o jogo tem caminho próprio, wad/_VitaTV, com L2/R2/L3/R3 no controle);
// VITART_HANDHELD=1 volta ao portátil, em que L2/R2 eram gestos no painel traseiro.
void video_skip_request(); // hle_io.cpp
void video_skip_tick();
int game_id();             // game_options.cpp
bool game_in_gameplay();
uint32_t game_qte_auto(uint8_t &lx, uint8_t &ly);
bool handheld_mode() {
    static const bool h = getenv("VITART_HANDHELD") && *getenv("VITART_HANDHELD") == '1';
    return h;
}
struct KbmInput {
    uint32_t buttons = 0;
    int mx = 0, my = 0;  // direção das teclas WASD (-1, 0, 1)
    bool roll = false;   // Shift: a direção vai para o analógico direito
};
static KbmInput keyboard_mouse() {
    KbmInput k;
#ifdef _WIN32
    if (!gvk::has_focus()) return k;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool g2 = game_id() == 2;
    const bool shift = down(VK_SHIFT);
    struct { int vk; uint32_t bit; } map[] = {
        { VK_SPACE, 0x4000 }, { VK_LBUTTON, 0x8000 }, { VK_RBUTTON, 0x1000 }, { 'F', 0x2000 }, { 'Q', 0x400 },
        { VK_LCONTROL, 0x100 }, { 'E', g2 ? 0x800u : 0x200u }, { 'R', g2 ? 0x200u : 0x800u }, { 'V', 0x6 },
        { '1', 0x10 }, { '2', 0x20 }, { '3', 0x40 }, { '4', 0x80 }, { VK_UP, 0x10 }, { VK_RIGHT, 0x20 },
        { VK_DOWN, 0x40 }, { VK_LEFT, 0x80 }, { VK_ESCAPE, 0x1 }, { VK_BACK, 0x1 }, { 'I', 0x8 }, { 'K', 0x8 },
        { VK_TAB, 0x8 }, { VK_RETURN, 0x8 }, { 'C', g2 ? 0xC00u : 0u },
    };
    for (auto &m : map)
        if (down(m.vk)) k.buttons |= m.bit;
    if (down(VK_MBUTTON)) k.buttons |= shift ? 0x6u : 0x2000u;
    k.mx = (down('D') ? 1 : 0) - (down('A') ? 1 : 0);
    k.my = (down('S') ? 1 : 0) - (down('W') ? 1 : 0);
    k.roll = shift;
#endif
    return k;
}
// Ícones dos botões pelo último controle usado: 1 = teclado/mouse (começo, sem controle), 0 = controle da Sony
// (PlayStation), 2 = controle XInput (Xbox). Um DualShock lido pelo HID e também emulado como XInput (DS4Windows)
// fica com os do PlayStation.
static std::atomic<int> g_icon_mode{1};
int input_icon_mode() { // VITART_ICON_MODE=0/1/2 (testes): fixa o modo
    static const int forced = getenv("VITART_ICON_MODE") ? atoi(getenv("VITART_ICON_MODE")) : -1;
    return forced >= 0 ? forced : g_icon_mode.load();
}
bool input_kbm_active() { return input_icon_mode() == 1; }
static uint64_t now_us_sys() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// VITART_INPUT="t_ms:máscara:dur_ms,...": botões apertados em instantes dados desde o arranque (máscara em
// hexadecimal, bits do SceCtrlData; cruz = 4000, start = 8). Para testes sem ninguém no teclado.
static uint32_t scripted_buttons() {
    struct Press { uint64_t t, dur; uint32_t mask; };
    static const std::vector<Press> presses = [] {
        std::vector<Press> v;
        const char *e = getenv("VITART_INPUT");
        while (e && *e) {
            unsigned long long t = 0, d = 0;
            unsigned m = 0;
            if (std::sscanf(e, "%llu:%x:%llu", &t, &m, &d) == 3) v.push_back({ t * 1000, d * 1000, m });
            e = std::strchr(e, ',');
            if (e) e++;
        }
        return v;
    }();
    static const uint64_t t0 = now_us_sys();
    const uint64_t t = now_us_sys() - t0;
    uint32_t b = 0;
    for (const Press &p : presses)
        if (t >= p.t && t < p.t + p.dur) b |= p.mask;
    return b;
}
VT_HLE(sceCtrlSetSamplingMode) { ret(ctx, 0); } // devolve o modo anterior (desligado)
VT_HLE(sceCtrlSetSamplingModeExt) { ret(ctx, 0); }
static void ctrl_read(arc_ctx *ctx) {
    // (port, SceCtrlData *data, count): 0x20 bytes por amostra.
    const uint32_t data = arg(ctx, 1);
    const int count = (int)arg(ctx, 2) > 0 ? (int)arg(ctx, 2) : 1;
    const hostin::State pad = hostin::poll(); // XInput, DualShock 4 e DualSense
    const KbmInput kbm = keyboard_mouse();
    uint32_t buttons = kbm.buttons | scripted_buttons() | pad.buttons;
    uint8_t lx = pad.lx, ly = pad.ly, rx = pad.rx, ry = pad.ry;
    if (kbm.mx || kbm.my) { // teclas: inclinação total (diagonal com o mesmo raio)
        const int r = kbm.mx && kbm.my ? 90 : 127;
        const uint8_t x = (uint8_t)(128 + kbm.mx * r), y = (uint8_t)(128 + kbm.my * r);
        if (kbm.roll) rx = x, ry = y;
        else lx = x, ly = y;
    }
    {   // ícones do teclado ou do controle: o último que mexeu
        if (kbm.buttons || kbm.mx || kbm.my) g_icon_mode = 1;
        else if (pad.active & 2) g_icon_mode = 0;
        else if (pad.active & 1) g_icon_mode = 2;
    }
    buttons |= game_qte_auto(lx, ly); // opção "Auto QTE" (game_options.cpp)
    {   // X apertado (borda): pede para pular a cena em vídeo, se houver uma tocando (o usuário prefere o X;
        // o START abre o menu de Power Up no GoW1).
        static bool x_was = false;
        const bool x_now = (buttons & 0x4000) != 0;
        if (x_now && !x_was) video_skip_request();
        x_was = x_now;
        video_skip_tick();
    }
    if (handheld_mode()) { // portátil: sem L2/R2/L3/R3; L1/R1 são os bits 0x100/0x200
        if (buttons & 0x400) buttons |= 0x100;
        if (buttons & 0x800) buttons |= 0x200;
        buttons &= ~0xC06u;
    }
    for (int i = 0; i < count; ++i) {
        const uint32_t d = data + 0x20u * i;
        std::memset(ptr<void>(d), 0, 0x20);
        wr64(d, now_us_sys());
        wr32(d + 8, buttons);
        ptr<uint8_t>(d)[12] = lx, ptr<uint8_t>(d)[13] = ly, ptr<uint8_t>(d)[14] = rx, ptr<uint8_t>(d)[15] = ry;
    }
    ret(ctx, (uint32_t)count);
}
VT_HLE(sceCtrlReadBufferPositive2) { ctrl_read(ctx); }
VT_HLE(sceCtrlReadBufferPositive) { ctrl_read(ctx); }
VT_HLE(sceCtrlPeekBufferPositive) { ctrl_read(ctx); }
VT_HLE(sceCtrlPeekBufferPositive2) { ctrl_read(ctx); }

VT_HLE(sceTouchGetPanelInfo) {
    // Painel frontal 1920x1088 e traseiro 1920x890 (unidades do Vita), tela 960x544.
    const uint32_t port = arg(ctx, 0), info = arg(ctx, 1);
    if (!info) return ret(ctx, 0x80220001u);
    std::memset(ptr<void>(info), 0, 0x30);
    int16_t *s = ptr<int16_t>(info);
    s[0] = 0; s[1] = port ? 108 : 0; s[2] = 1919; s[3] = port ? 889 + 108 : 1087;
    s[4] = 0; s[5] = 0; s[6] = 1919; s[7] = 1087;
    ptr<uint8_t>(info)[16] = 1;
    ptr<uint8_t>(info)[17] = 128;
    ret(ctx, SCE_OK);
}
VT_HLE(sceTouchSetSamplingState) { ret(ctx, SCE_OK); }
// SceTouchData {u64 timeStamp; u32 status; u32 reportNum; SceTouchReport report[8]} com
// SceTouchReport {u8 id; u8 force; u16 x; u16 y; u8 reserved[8]; u16 info} (16 bytes). O mouse na
// janela vira um dedo no painel frontal (1920x1088 unidades); o traseiro fica sem toque.
// VITART_TOUCH="t_ms:x:y:dur_ms,...": toques no painel frontal em coordenadas normalizadas (0..1) da
// tela, contados desde a primeira leitura do toque. Para testes sem ninguém no mouse.
static bool scripted_touch(float &nx, float &ny) {
    struct Tap { uint64_t t, dur; float x, y; };
    static const std::vector<Tap> taps = [] {
        std::vector<Tap> v;
        const char *e = getenv("VITART_TOUCH");
        while (e && *e) {
            unsigned long long t = 0, d = 0;
            float x = 0, y = 0;
            if (std::sscanf(e, "%llu:%f:%f:%llu", &t, &x, &y, &d) == 4) v.push_back({ t * 1000, d * 1000, x, y });
            e = std::strchr(e, ',');
            if (e) e++;
        }
        return v;
    }();
    static const uint64_t t0 = now_us_sys();
    const uint64_t t = now_us_sys() - t0;
    for (const Tap &p : taps)
        if (t >= p.t && t < p.t + p.dur) { nx = p.x, ny = p.y; return true; }
    return false;
}
static void touch_fill(uint32_t port, uint32_t data) {
    std::memset(ptr<void>(data), 0, 0x90);
    wr64(data, now_us_sys());
    float nx, ny;
    if (port == 0 && (scripted_touch(nx, ny) || (!game_in_gameplay() && gvk::pointer(nx, ny)))) {
        static uint8_t id = 0;
        static bool down = false;
        static uint64_t last = 0;
        const uint64_t t = now_us_sys();
        if (!down || t - last > 200000) id++; // dedo novo a cada toque
        down = true, last = t;
        wr32(data + 12, 1);
        uint8_t *r = ptr<uint8_t>(data + 16);
        r[0] = id, r[1] = 128;
        *ptr<uint16_t>(data + 18) = (uint16_t)(nx * 1920.f);
        *ptr<uint16_t>(data + 20) = (uint16_t)(ny * 1088.f);
    }
}
VT_HLE(sceTouchPeek) {
    const uint32_t data = arg(ctx, 1);
    const uint32_t n = arg(ctx, 2) ? arg(ctx, 2) : 1;
    for (uint32_t i = 0; i < n; ++i) touch_fill(arg(ctx, 0), data + 0x90 * i);
    ret(ctx, n);
}
VT_HLE(sceTouchRead) {
    touch_fill(arg(ctx, 0), arg(ctx, 1));
    ret(ctx, 1);
}
VT_HLE(sceMotionGetState) {
    // SceMotionState (0xF8 bytes): aparelho parado e na horizontal (gravidade em -z, orientação identidade).
    const uint32_t st = arg(ctx, 0);
    if (!st) return ret(ctx, 0x80360200u);
    std::memset(ptr<void>(st), 0, 0xF8);
    wr32(st, (uint32_t)now_us_sys());
    float *f = ptr<float>(st);
    f[3] = -1.f;                         // acceleration.z
    f[13] = 1.f;                         // deviceQuat.w (x, y, z, w a partir de +0x28)
    for (int i = 0; i < 4; i++) f[14 + 5 * i] = f[30 + 5 * i] = 1.f; // rotationMatrix e nedMatrix identidade
    f[49] = -1.f;                        // basicOrientation.z (aparelho deitado)
    wr64(st + 0xC8, now_us_sys());
    ret(ctx, SCE_OK);
}
VT_HLE(sceMotionReset) { ret(ctx, SCE_OK); }
VT_HLE(sceMotionStartSampling) { ret(ctx, SCE_OK); }
VT_HLE(sceMotionStopSampling) { ret(ctx, SCE_OK); }

// =================================================================== exibição
static std::atomic<uint32_t> g_fb_base{ 0 }, g_fb_pitch{ 0 }, g_fb_w{ 0 }, g_fb_h{ 0 };
static std::atomic<uint64_t> g_fb_sets{ 0 };
static const auto g_vblank0 = std::chrono::steady_clock::now();
constexpr auto VBLANK = std::chrono::microseconds(16667);

uint32_t display_framebuffer(uint32_t *pitch, uint32_t *w, uint32_t *h) {
    if (pitch) *pitch = g_fb_pitch;
    if (w) *w = g_fb_w;
    if (h) *h = g_fb_h;
    return g_fb_base;
}
void gxm_present(uint32_t fb); // hle_gxm.cpp
VT_HLE(sceDisplaySetFrameBuf) {
    // SceDisplayFrameBuf: size, base, pitch, pixelformat, width, height
    const uint32_t p = arg(ctx, 0);
    if (p) {
        g_fb_base = rd32(p + 4);
        g_fb_pitch = rd32(p + 8);
        g_fb_w = rd32(p + 16);
        g_fb_h = rd32(p + 20);
        if (g_fb_sets++ == 0)
            logf("vitart: framebuffer 0x%08x, %ux%u, pitch %u, formato 0x%x", g_fb_base.load(), g_fb_w.load(), g_fb_h.load(), g_fb_pitch.load(), rd32(p + 12));
        gxm_present(g_fb_base);
    }
    ret(ctx, SCE_OK);
}
static void wait_vblank(uint32_t count) {
    // VITART_NOVSYNC=1 (medição): sem espera de vblank, para ver o teto de quadros por segundo.
    static const bool novsync = getenv("VITART_NOVSYNC") != nullptr;
    if (novsync) return;
    const auto now = std::chrono::steady_clock::now();
    const auto n = (now - g_vblank0) / VBLANK;
    std::this_thread::sleep_until(g_vblank0 + VBLANK * (n + (count ? count : 1)));
}
VT_HLE(sceDisplayWaitVblankStart) { wait_vblank(1); ret(ctx, SCE_OK); }
VT_HLE(sceDisplayGetVcount) {
    ret(ctx, (uint32_t)((std::chrono::steady_clock::now() - g_vblank0) / VBLANK));
}
// Opção de 60 FPS (game_options.cpp): o GoW1 espera 2 vblanks por quadro (30 FPS); com a taxa da lógica
// do jogo também em 60, 1 vblank dá 60 FPS na velocidade certa.
static std::atomic<bool> g_fps60{false};
void display_set_fps60(bool on) { g_fps60 = on; }
// FPS acima de 60 (game_options.cpp): o quadro sai a cada 1/fps s em vez de por vblanks.
static std::atomic<uint32_t> g_fps_target{0};
static std::atomic<uint32_t> g_fixed_step_addr{0}; // palavra = 1 no menu principal (GoW1: 0x812bc0c0)
void display_set_fixed_step_addr(uint32_t a) { g_fixed_step_addr = a; }
// Menus (pausa, Power Up, telas de carregamento e cenas: tudo fora da gameplay, game_options.cpp) animam por quadro
// e aceleravam acima de 60 FPS: neles o ritmo fica em 60.
static std::atomic<bool> g_in_menu{false};
void display_set_in_menu(bool on) { g_in_menu = on; }
void display_set_fps_target(uint32_t fps) { g_fps_target = fps; }
// Ritmo por prazo no laço principal do jogo (que chama esta função uma vez por quadro): o próximo quadro
// sai 1/30 s (ou 1/60 s) depois do anterior. Esperar o próximo vblank da grade transformava um quadro de
// 17 ms em 33 ms (60 -> 30/40 FPS em degrau); com prazo, um quadro atrasado não espera nada e o seguinte
// volta ao ritmo. O jogo mede o dt real (max(dt, 1/60)), então quadros irregulares não mudam a velocidade.
static void wait_frame(uint32_t vblanks) {
    static const bool novsync = getenv("VITART_NOVSYNC") != nullptr;
    if (novsync) return;
    using clk = std::chrono::steady_clock;
    static clk::time_point last{};
    uint32_t fps = g_fps_target;
    // menu principal (anima por quadro, não pelo tempo): ritmo do jogo original
    if (fps > 60 && ((g_fixed_step_addr && rd32(g_fixed_step_addr) == 1) || g_in_menu)) fps = 0, vblanks = 1;
    if (fps == UINT32_MAX) return; // VSync desligado: sem limite de quadros
    const auto interval = fps > 60 ? std::chrono::duration_cast<clk::duration>(std::chrono::duration<double>(1.0 / fps))
                                   : std::chrono::duration_cast<clk::duration>(VBLANK * (vblanks ? vblanks : 1));
    const auto now = clk::now();
    const auto target = last + interval;
    if (last.time_since_epoch().count() && now < target) {
        // dorme até 1,5 ms antes e completa girando: o sleep do Windows passa do prazo em até 1 ms
        high_res_timer();
        if (clk::now() < target - std::chrono::microseconds(1500))
            std::this_thread::sleep_until(target - std::chrono::microseconds(1500));
        while (clk::now() < target) std::this_thread::yield();
        last = target;
    } else {
        last = now; // atrasado (ou primeiro quadro): sem espera, o prazo recomeça daqui
    }
}
VT_HLE(sceDisplayWaitVblankStartMulti) {
    uint32_t n = arg(ctx, 0);
    if (n == 2 && g_fps60) n = 1;
    wait_frame(n);
    ret(ctx, SCE_OK);
}
VT_HLE(sceDisplayWaitSetFrameBuf) { wait_vblank(1); ret(ctx, SCE_OK); }

// =================================================================== relógio
// SceDateTime: year, month, day, hour, minute, second (u16) e microsecond (u32).
static void write_datetime(uint32_t p, const std::tm &t, uint32_t us) {
    uint16_t *s = ptr<uint16_t>(p);
    s[0] = (uint16_t)(t.tm_year + 1900); s[1] = (uint16_t)(t.tm_mon + 1); s[2] = (uint16_t)t.tm_mday;
    s[3] = (uint16_t)t.tm_hour; s[4] = (uint16_t)t.tm_min; s[5] = (uint16_t)t.tm_sec;
    wr32(p + 12, us);
}
static std::tm read_datetime(uint32_t p, uint32_t *us) {
    const uint16_t *s = ptr<uint16_t>(p);
    std::tm t{};
    t.tm_year = s[0] - 1900; t.tm_mon = s[1] - 1; t.tm_mday = s[2];
    t.tm_hour = s[3]; t.tm_min = s[4]; t.tm_sec = s[5];
    if (us) *us = rd32(p + 12);
    return t;
}
static int64_t utc_seconds(std::tm t) {
#ifdef _WIN32
    return _mkgmtime64(&t);
#else
    return timegm(&t);
#endif
}
VT_HLE(sceRtcGetCurrentClock) {
    // (SceDateTime *time, int time_zone em minutos)
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    const int64_t secs = us / 1000000 + (int32_t)arg(ctx, 1) * 60;
    std::tm t{};
    const time_t tt = (time_t)secs;
    gmtime_s(&t, &tt);
    write_datetime(arg(ctx, 0), t, (uint32_t)(us % 1000000));
    ret(ctx, SCE_OK);
}
VT_HLE(sceRtcGetCurrentClockLocalTime) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    std::tm t{};
    const time_t tt = (time_t)(us / 1000000);
    localtime_s(&t, &tt);
    write_datetime(arg(ctx, 0), t, (uint32_t)(us % 1000000));
    ret(ctx, SCE_OK);
}
VT_HLE(sceRtcGetTime64_t) {
    const std::tm t = read_datetime(arg(ctx, 0), nullptr);
    wr64(arg(ctx, 1), (uint64_t)utc_seconds(t));
    ret(ctx, SCE_OK);
}
VT_HLE(sceRtcSetTime64_t) {
    // (SceDateTime *out, SceUInt64 t): o u64 vem em r2:r3 (alinhado a par).
    const uint64_t secs = (uint64_t)ctx->r[2] | ((uint64_t)ctx->r[3] << 32);
    std::tm t{};
    const time_t tt = (time_t)secs;
    gmtime_s(&t, &tt);
    write_datetime(arg(ctx, 0), t, 0);
    ret(ctx, SCE_OK);
}
// struct tm do Vita: 9 ints, como o do host.
VT_HLE(sceKernelLibcGmtime_r) {
    const time_t tt = (time_t)rd32(arg(ctx, 0));
    std::tm t{};
    gmtime_s(&t, &tt);
    std::memcpy(ptr<void>(arg(ctx, 1)), &t, 36);
    ret(ctx, arg(ctx, 1));
}
VT_HLE(sceKernelLibcLocaltime_r) {
    const time_t tt = (time_t)rd32(arg(ctx, 0));
    std::tm t{};
    localtime_s(&t, &tt);
    std::memcpy(ptr<void>(arg(ctx, 1)), &t, 36);
    ret(ctx, arg(ctx, 1));
}
VT_HLE(sceKernelLibcMktime) {
    // (struct tm *date, time_t *out, u64 *out64)
    std::tm t{};
    std::memcpy(&t, ptr<void>(arg(ctx, 0)), 36);
    const bool y1900 = t.tm_year >= 1900;
    if (y1900) t.tm_year -= 1900;
    const time_t r = std::mktime(&t);
    if (y1900) t.tm_year += 1900;
    std::memcpy(ptr<void>(arg(ctx, 0)), &t, 36);
    if (arg(ctx, 1)) wr32(arg(ctx, 1), (uint32_t)r);
    if (arg(ctx, 2)) wr64(arg(ctx, 2), (uint64_t)r);
    ret(ctx, SCE_OK);
}

// =================================================================== tubos de mensagem
constexpr uint32_t ERR_UNKNOWN_MPP = 0x8002819Eu;
constexpr uint32_t ERR_MPP_EMPTY = 0x80028223u; // SCE_KERNEL_ERROR_MPP_EMPTY (o GoW1 compara com este valor)
constexpr uint32_t ERR_MPP_FULL = 0x80028222u;  // SCE_KERNEL_ERROR_MPP_FULL
constexpr uint32_t ERR_WAIT_TIMEOUT = 0x80028005u;
constexpr uint32_t MPP_FULL = 0x1, MPP_DONT_WAIT = 0x10, MPP_DONT_REMOVE = 0x100;

struct MsgPipe {
    std::string name;
    uint32_t cap = 0;
    std::deque<uint8_t> data;
    std::mutex m;
    std::condition_variable cv;
};
static std::mutex g_mpp_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<MsgPipe>> g_pipes;
static std::atomic<uint32_t> g_mpp_uid{ 0x00400001 };

static std::shared_ptr<MsgPipe> pipe_of(uint32_t uid) {
    std::lock_guard<std::mutex> g(g_mpp_mutex);
    auto it = g_pipes.find(uid);
    return it == g_pipes.end() ? nullptr : it->second;
}
VT_HLE(sceKernelCreateMsgPipe) {
    auto p = std::make_shared<MsgPipe>();
    p->name = gstr(arg(ctx, 0));
    p->cap = arg(ctx, 3) ? arg(ctx, 3) : 0x1000;
    const uint32_t uid = g_mpp_uid.fetch_add(2);
    std::lock_guard<std::mutex> g(g_mpp_mutex);
    g_pipes[uid] = p;
    ret(ctx, uid);
}
VT_HLE(sceKernelDeleteMsgPipe) {
    std::lock_guard<std::mutex> g(g_mpp_mutex);
    g_pipes.erase(arg(ctx, 0));
    ret(ctx, SCE_OK);
}
// Transferência com os modos do Vita: FULL exige o tamanho inteiro, ASAP aceita qualquer parte > 0.
static void mpp_send(arc_ctx *ctx, bool try_only) {
    auto p = pipe_of(arg(ctx, 0));
    if (!p) return ret(ctx, ERR_UNKNOWN_MPP);
    const uint32_t msg = arg(ctx, 1), size = arg(ctx, 2), mode = arg(ctx, 3), result = arg(ctx, 4);
    const uint32_t ptimeout = try_only ? 0 : arg(ctx, 5);
    const bool full = mode & MPP_FULL, nowait = try_only || (mode & MPP_DONT_WAIT);
    std::unique_lock<std::mutex> lk(p->m);
    auto ready = [&] { const uint32_t room = p->cap - (uint32_t)p->data.size(); return full ? room >= size : room > 0; };
    if (!ready()) {
        if (nowait) return ret(ctx, ERR_MPP_FULL);
        if (ptimeout) {
            if (!p->cv.wait_for(lk, std::chrono::microseconds(rd32(ptimeout)), ready)) { wr32(ptimeout, 0); return ret(ctx, ERR_WAIT_TIMEOUT); }
        } else p->cv.wait(lk, ready);
    }
    const uint32_t n = std::min<uint32_t>(size, p->cap - (uint32_t)p->data.size());
    const uint8_t *src = ptr<uint8_t>(msg);
    p->data.insert(p->data.end(), src, src + n);
    if (result) wr32(result, n);
    p->cv.notify_all();
    ret(ctx, SCE_OK);
}
static void mpp_recv(arc_ctx *ctx, bool try_only) {
    auto p = pipe_of(arg(ctx, 0));
    if (!p) return ret(ctx, ERR_UNKNOWN_MPP);
    const uint32_t msg = arg(ctx, 1), size = arg(ctx, 2), mode = arg(ctx, 3), result = arg(ctx, 4);
    const uint32_t ptimeout = try_only ? 0 : arg(ctx, 5);
    const bool full = mode & MPP_FULL, nowait = try_only || (mode & MPP_DONT_WAIT);
    std::unique_lock<std::mutex> lk(p->m);
    auto ready = [&] { return full ? p->data.size() >= size : !p->data.empty(); };
    if (!ready()) {
        if (nowait) return ret(ctx, ERR_MPP_EMPTY);
        if (ptimeout) {
            if (!p->cv.wait_for(lk, std::chrono::microseconds(rd32(ptimeout)), ready)) { wr32(ptimeout, 0); return ret(ctx, ERR_WAIT_TIMEOUT); }
        } else p->cv.wait(lk, ready);
    }
    const uint32_t n = std::min<uint32_t>(size, (uint32_t)p->data.size());
    uint8_t *dst = ptr<uint8_t>(msg);
    std::copy(p->data.begin(), p->data.begin() + n, dst);
    if (!(mode & MPP_DONT_REMOVE)) p->data.erase(p->data.begin(), p->data.begin() + n);
    if (result) wr32(result, n);
    p->cv.notify_all();
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelSendMsgPipe) { mpp_send(ctx, false); }
VT_HLE(sceKernelSendMsgPipeCB) { mpp_send(ctx, false); }
VT_HLE(sceKernelTrySendMsgPipe) { mpp_send(ctx, true); }
VT_HLE(sceKernelReceiveMsgPipe) { mpp_recv(ctx, false); }
VT_HLE(sceKernelReceiveMsgPipeCB) { mpp_recv(ctx, false); }
VT_HLE(sceKernelTryReceiveMsgPipe) { mpp_recv(ctx, true); }

// =================================================================== threads e módulos (informação)
VT_HLE(sceKernelGetThreadInfo) {
    // SceKernelThreadInfo: size, processId, name[32], attr, status, entry, stack, stackSize,
    // initPriority, currentPriority, ...
    const uint32_t info = arg(ctx, 1);
    Thread *t = arg(ctx, 0) ? find_thread((int)arg(ctx, 0)) : current();
    if (!t || !info) return ret(ctx, 0x80028001u);
    const uint32_t size = rd32(info) ? rd32(info) : 0x80;
    std::memset(ptr<void>(info), 0, size);
    wr32(info, size);
    wr32(info + 4, 1);
    std::strncpy(ptr<char>(info + 8), t->name.c_str(), 31);
    wr32(info + 0x2C, t->state == 1 ? 1 : 0x10);
    wr32(info + 0x30, t->entry);
    wr32(info + 0x34, t->stack);
    wr32(info + 0x38, t->stack_size);
    wr32(info + 0x3C, (uint32_t)t->priority);
    wr32(info + 0x40, (uint32_t)t->priority);
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelGetModuleInfoByAddr) { ret(ctx, 0x8002D082u); } // SCE_KERNEL_ERROR_MODULEMGR_NOENT

// =================================================================== Fios2 overlays (sem sobreposições)
VT_HLE(sceFiosOverlayAddForProcess02) {
    static std::atomic<uint32_t> id{ 1 };
    if (arg(ctx, 2)) wr32(arg(ctx, 2), id++);
    ret(ctx, SCE_OK);
}
VT_HLE(sceFiosOverlayRemoveForProcess02) { ret(ctx, SCE_OK); }
VT_HLE(sceFiosOverlayModifyForProcess02) { ret(ctx, SCE_OK); }
VT_HLE(sceFiosOverlayGetInfoForProcess02) { ret(ctx, 0x80820005u); }
VT_HLE(sceFiosOverlayGetList02) {
    // (pid, minOrder, maxOrder, ids*, maxIds, actual*)
    if (arg(ctx, 5)) wr32(arg(ctx, 5), 0);
    ret(ctx, SCE_OK);
}
VT_HLE(sceFiosOverlayGetRecommendedScheduler02) {
    const char *path = gstr(arg(ctx, 1));
    if ((int)arg(ctx, 0) <= 1 || std::strlen(path) < 6) return ret(ctx, 0);
    ret(ctx, std::memcmp(path, "host", 4) == 0 && path[4] <= '9' && path[5] == ':');
}
VT_HLE(sceFiosOverlayResolveWithRangeSync02) {
    // (pid, flag, in, out, maxPath, min, max): sem sobreposições, o caminho é ele mesmo.
    const uint32_t out = arg(ctx, 3), max = arg(ctx, 4);
    if (out && max) {
        std::strncpy(ptr<char>(out), gstr(arg(ctx, 2)), max);
        ptr<char>(out)[max - 1] = 0;
    }
    ret(ctx, SCE_OK);
}
VT_HLE(sceFiosOverlayThreadSetDisabled02) { ret(ctx, SCE_OK); }
VT_HLE(sceFiosOverlayThreadIsDisabled02) { ret(ctx, 0); }

// =================================================================== NP e troféus
VT_HLE(sceNpInit) { ret(ctx, SCE_OK); }
VT_HLE(sceNpTerm) { ret(ctx, SCE_OK); }
VT_HLE(sceNpTrophyInit) { ret(ctx, SCE_OK); }
VT_HLE(sceNpTrophyTerm) { ret(ctx, SCE_OK); }
// Troféus: a lista vem do TROP.SFM (XML) dentro de app0:sce_sys/trophy/<NPWR>_<nn>/TROPHY.TRP; os
// desbloqueados ficam em <dados>/trophy/<NPWR>_<nn>/unlocked.txt (um id por linha) e o aviso aparece no
// título da janela. A platina (ttype P) sai sozinha quando todos os outros estão desbloqueados.
struct Trophy { int id; char type; std::string name, detail; };
struct TrophySet {
    std::string comm;              // "NPWR05244_00"
    std::vector<Trophy> list;
    std::set<int> unlocked;
};
static std::mutex g_trophy_mutex;
static std::map<uint32_t, TrophySet> g_trophy_sets; // contexto -> conjunto

static std::string xml_between(const std::string &s, size_t from, const char *open, const char *close) {
    const size_t a = s.find(open, from);
    if (a == std::string::npos) return "";
    const size_t b = s.find(close, a);
    return b == std::string::npos ? "" : s.substr(a + std::strlen(open), b - a - std::strlen(open));
}
static void trophy_load(TrophySet &ts) {
    const std::string trp = app_path() + "/sce_sys/trophy/" + ts.comm + "/TROPHY.TRP";
    FILE *f = fopen(trp.c_str(), "rb");
    if (!f) return logf("vitart: troféus: %s não encontrado", trp.c_str());
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    auto be32 = [&](size_t o) { return (uint32_t)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3]; };
    auto be64 = [&](size_t o) { return (uint64_t)be32(o) << 32 | be32(o + 4); };
    if (d.size() < 0x40 || be32(0) != 0xDCA24D00u) return;
    const uint32_t n = be32(16), esz = be32(20);
    for (uint32_t i = 0; i < n && 0x40 + (i + 1) * esz <= d.size(); i++) {
        const size_t e = 0x40 + i * esz;
        if (std::strncmp(reinterpret_cast<const char *>(&d[e]), "TROP.SFM", 32) != 0) continue;
        const uint64_t off = be64(e + 32), size = be64(e + 40);
        if (off + size > d.size()) break;
        const std::string x(reinterpret_cast<const char *>(&d[off]), (size_t)size);
        for (size_t p = x.find("<trophy id=\""); p != std::string::npos; p = x.find("<trophy id=\"", p + 1)) {
            Trophy t;
            t.id = std::atoi(x.c_str() + p + 12);
            const size_t ty = x.find("ttype=\"", p);
            t.type = ty != std::string::npos ? x[ty + 7] : 'B';
            t.name = xml_between(x, p, "<name>", "</name>");
            t.detail = xml_between(x, p, "<detail>", "</detail>");
            ts.list.push_back(t);
        }
    }
    const std::string saved = data_path() + "/trophy/" + ts.comm + "/unlocked.txt";
    if (FILE *u = fopen(saved.c_str(), "r")) {
        int id;
        while (fscanf(u, "%d", &id) == 1) ts.unlocked.insert(id);
        fclose(u);
    }
    logf("vitart: troféus %s: %zu no jogo, %zu já desbloqueados", ts.comm.c_str(), ts.list.size(), ts.unlocked.size());
}
static void trophy_save(const TrophySet &ts) {
    const std::string dir = data_path() + "/trophy/" + ts.comm;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (FILE *u = fopen((dir + "/unlocked.txt").c_str(), "w")) {
        for (int id : ts.unlocked) fprintf(u, "%d\n", id);
        fclose(u);
    }
}
// Arquivo de dentro do TROPHY.TRP (ícones TROPnnn.PNG).
static std::vector<uint8_t> trp_entry(const std::string &comm, const std::string &name) {
    std::vector<uint8_t> out;
    FILE *f = fopen((app_path() + "/sce_sys/trophy/" + comm + "/TROPHY.TRP").c_str(), "rb");
    if (!f) return out;
    uint8_t h[0x40];
    auto be32 = [](const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; };
    if (fread(h, 1, sizeof h, f) == sizeof h && be32(h) == 0xDCA24D00u) {
        const uint32_t n = be32(h + 16), esz = be32(h + 20);
        std::vector<uint8_t> e(esz);
        for (uint32_t i = 0; i < n; i++) {
            fseek(f, 0x40 + (long)(i * esz), SEEK_SET);
            if (fread(e.data(), 1, esz, f) != esz) break;
            if (std::strncmp(reinterpret_cast<const char *>(e.data()), name.c_str(), 32) != 0) continue;
            const uint64_t off = (uint64_t)be32(&e[32]) << 32 | be32(&e[36]), size = (uint64_t)be32(&e[40]) << 32 | be32(&e[44]);
            out.resize((size_t)size);
            _fseeki64(f, (long long)off, SEEK_SET);
            if (fread(out.data(), 1, out.size(), f) != out.size()) out.clear();
            break;
        }
    }
    fclose(f);
    return out;
}

static void trophy_announce(const TrophySet &ts, int id) {
    const char *kind = "Bronze";
    std::string name = "#" + std::to_string(id), detail;
    for (const Trophy &t : ts.list)
        if (t.id == id) {
            name = t.name, detail = t.detail;
            kind = t.type == 'P' ? "Platina" : t.type == 'G' ? "Ouro" : t.type == 'S' ? "Prata" : "Bronze";
        }
    logf("vitart: TROFÉU (%s) desbloqueado: %s — %s", kind, name.c_str(), detail.c_str());
    gvk::notify(std::string("Troféu desbloqueado (") + kind + "): " + name + " — " + detail); // título da janela
    gvk::TrophyBanner b;
    char icon[16];
    std::snprintf(icon, sizeof icon, "TROP%03d.PNG", id);
    b.icon_png = trp_entry(ts.comm, icon);
    for (const Trophy &t : ts.list)
        if (t.id == id) b.tier = t.type;
    b.name = name;
    // som: <dados>/trophy.wav (platina: platinum.wav), tocado quando o aviso aparece
    const std::string wav = data_path() + (b.tier == 'P' && std::filesystem::exists(data_path() + "/platinum.wav")
                                               ? "/platinum.wav" : "/trophy.wav");
    if (std::filesystem::exists(wav)) b.sound = wav;
    gvk::notify_trophy(b);
}

VT_HLE(sceNpTrophyCreateContext) {
    // (SceNpTrophyContext *ctx, const SceNpCommunicationId *commId {char data[9]; char term; u8 num}, sign, opts)
    static std::atomic<uint32_t> next{1};
    const uint32_t c = next++, cid = arg(ctx, 1);
    if (arg(ctx, 0)) wr32(arg(ctx, 0), c);
    if (cid) {
        char comm[16];
        std::snprintf(comm, sizeof comm, "%.9s_%02u", gstr(cid), *ptr<uint8_t>(cid + 10));
        const std::lock_guard<std::mutex> lock(g_trophy_mutex);
        TrophySet &ts = g_trophy_sets[c];
        ts.comm = comm;
        trophy_load(ts);
    }
    ret(ctx, SCE_OK);
}
VT_HLE(sceNpTrophyCreateHandle) {
    static std::atomic<uint32_t> h{ 1 };
    if (arg(ctx, 0)) wr32(arg(ctx, 0), h++);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNpTrophyDestroyHandle) { ret(ctx, SCE_OK); }
VT_HLE(sceNpTrophyUnlockTrophy) {
    // (context, handle, trophyId, int *platinumId)
    const int id = (int)arg(ctx, 2);
    if (arg(ctx, 3)) wr32(arg(ctx, 3), 0xFFFFFFFFu); // SCE_NP_TROPHY_INVALID_TROPHY_ID: sem platina
    const std::lock_guard<std::mutex> lock(g_trophy_mutex);
    auto it = g_trophy_sets.find(arg(ctx, 0));
    if (it == g_trophy_sets.end()) {
        logf("vitart: troféu %d desbloqueado (contexto desconhecido)", id);
        return ret(ctx, SCE_OK);
    }
    TrophySet &ts = it->second;
    if (!ts.unlocked.insert(id).second) return ret(ctx, 0x80551628u); // SCE_NP_TROPHY_ERROR_TROPHY_ALREADY_UNLOCKED
    trophy_announce(ts, id);
    // Platina: todos os outros desbloqueados.
    int plat = -1;
    bool all = true;
    for (const Trophy &t : ts.list) {
        if (t.type == 'P') plat = t.id;
        else if (!ts.unlocked.count(t.id)) all = false;
    }
    if (plat >= 0 && all && ts.unlocked.insert(plat).second) {
        trophy_announce(ts, plat);
        if (arg(ctx, 3)) wr32(arg(ctx, 3), (uint32_t)plat);
    }
    trophy_save(ts);
    ret(ctx, SCE_OK);
}

// Cheat "Unlock All Trophies" (menu de pausa, game_options.cpp): desbloqueia todos os troféus dos conjuntos abertos.
// Cada troféu desbloqueado agora ganha o aviso, um depois do outro (a fila do aviso, gxm_vk.cpp), e a platina por
// último. Devolve quantos foram desbloqueados agora.
int trophy_unlock_all() {
    const std::lock_guard<std::mutex> lock(g_trophy_mutex);
    int n = 0;
    for (auto &[ctx, ts] : g_trophy_sets) {
        int plat = -1;
        for (const Trophy &t : ts.list) {
            if (t.type == 'P') plat = t.id;
            else if (ts.unlocked.insert(t.id).second) n++, trophy_announce(ts, t.id);
        }
        if (plat >= 0 && ts.unlocked.insert(plat).second) n++, trophy_announce(ts, plat);
        trophy_save(ts);
    }
    logf("vitart: cheat: %d troféus desbloqueados", n);
    return n;
}

// =================================================================== diálogos comuns
// Sem interface: cada diálogo termina na hora com o resultado "OK".
constexpr uint32_t DIALOG_NONE = 0, DIALOG_FINISHED = 2;
static std::atomic<uint32_t> g_msg_dialog{ DIALOG_NONE }, g_trophy_dialog{ DIALOG_NONE };
VT_HLE(sceCommonDialogUpdate) { ret(ctx, SCE_OK); }
VT_HLE(sceMsgDialogInit) {
    const uint32_t p = arg(ctx, 0);
    // SceMsgDialogParam: sdkVersion, commonParam(0x2C), mode @0x30, ..., userMsgParam* @0x38?
    logf("vitart: diálogo de mensagem (modo %u) respondido automaticamente", p ? rd32(p + 0x30) : 0);
    g_msg_dialog = DIALOG_FINISHED;
    ret(ctx, SCE_OK);
}
VT_HLE(sceMsgDialogGetStatus) { ret(ctx, g_msg_dialog); }
VT_HLE(sceMsgDialogGetResult) {
    const uint32_t r = arg(ctx, 0);
    if (r) {
        std::memset(ptr<void>(r), 0, 0x2C);
        wr32(r + 8, 1); // SCE_MSG_DIALOG_BUTTON_ID_OK / YES
    }
    ret(ctx, SCE_OK);
}
VT_HLE(sceMsgDialogTerm) { g_msg_dialog = DIALOG_NONE; ret(ctx, SCE_OK); }
VT_HLE(sceMsgDialogClose) { g_msg_dialog = DIALOG_FINISHED; ret(ctx, SCE_OK); }
VT_HLE(sceNpTrophySetupDialogInit) { g_trophy_dialog = DIALOG_FINISHED; ret(ctx, SCE_OK); }
VT_HLE(sceNpTrophySetupDialogGetStatus) { ret(ctx, g_trophy_dialog); }
VT_HLE(sceNpTrophySetupDialogGetResult) {
    if (arg(ctx, 0)) std::memset(ptr<void>(arg(ctx, 0)), 0, 0x24);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNpTrophySetupDialogTerm) { g_trophy_dialog = DIALOG_NONE; ret(ctx, SCE_OK); }

// =================================================================== AppUtil
constexpr uint32_t APPUTIL_SLOT_NOT_FOUND = 0x80100641u;
VT_HLE(sceAppUtilInit) {
    // (SceAppUtilInitParam*, SceAppUtilBootParam*): boot normal, sem atributos.
    if (arg(ctx, 1)) std::memset(ptr<void>(arg(ctx, 1)), 0, 0x30);
    ret(ctx, SCE_OK);
}
VT_HLE(sceAppUtilShutdown) { ret(ctx, SCE_OK); }
VT_HLE(sceAppUtilSystemParamGetInt) {
    // 1 idioma (1 = inglês EUA), 2 botão de confirmação (1 = cruz), 3 formato de data, 4 formato de hora,
    // 5 fuso, 6 horário de verão.
    static const uint32_t values[] = { 0, 1, 1, 0, 0, 0, 0 };
    const uint32_t id = arg(ctx, 0);
    if (!arg(ctx, 1)) return ret(ctx, 0x80100602u);
    wr32(arg(ctx, 1), id < 7 ? values[id] : 0);
    ret(ctx, SCE_OK);
}
// Saves: os arquivos vão para savedata0: (pasta savedata0 do diretório de dados); os parâmetros de cada
// slot (SceAppUtilSaveDataSlotParam, 0x34C bytes) ficam em savedata0:sce_sys/vitart_slot_<id>.bin, de
// onde sceAppUtilSaveDataSlotGetParam os lê (o jogo só carrega um slot que existe).
constexpr uint32_t kSlotParamSize = 0x34C;
static std::string slot_file(uint32_t id) { return vita_host_path("savedata0:sce_sys/vitart_slot_" + std::to_string(id) + ".bin"); }
VT_HLE(sceAppUtilSaveDataSlotGetParam) {
    // (slotId, SceAppUtilSaveDataSlotParam *param, mountPoint)
    FILE *f = fopen(slot_file(arg(ctx, 0)).c_str(), "rb");
    if (!f) return ret(ctx, APPUTIL_SLOT_NOT_FOUND);
    std::vector<uint8_t> b(kSlotParamSize);
    const size_t n = fread(b.data(), 1, b.size(), f);
    fclose(f);
    if (arg(ctx, 1)) std::memcpy(ptr<void>(arg(ctx, 1)), b.data(), n);
    ret(ctx, SCE_OK);
}
VT_HLE(sceAppUtilSaveDataDataRemove) {
    // (slot, SceAppUtilSaveDataRemoveItem *files (0x2C cada: caminho, modo), fileNum, mountPoint)
    const uint32_t files = arg(ctx, 1), n = arg(ctx, 2);
    std::error_code ec;
    for (uint32_t i = 0; files && i < n; i++) {
        const uint32_t it = files + 0x2C * i;
        const std::string p = vita_host_path(std::string("savedata0:") + gstr(rd32(it)));
        if (rd32(it + 4) == 0) std::filesystem::remove(p, ec); else std::filesystem::remove_all(p, ec);
        logf("vitart: save removido: %s", p.c_str());
    }
    ret(ctx, SCE_OK);
}
VT_HLE(sceAppUtilSaveDataDataSave) {
    // (SceAppUtilSaveDataFileSlot *slot {id, slotParam*}, SceAppUtilSaveDataFile *files, fileNum, mountPoint,
    //  SceSize *requiredSizeKB). Arquivo: {filePath, buf, bufSize, _, offset (64 bits em +16), mode em +24}.
    // Modo 0 escreve em offset sem truncar, 1 trunca antes, 2 cria diretório.
    const uint32_t slot = arg(ctx, 0), files = arg(ctx, 1), n = arg(ctx, 2);
    std::error_code ec;
    uint64_t total = 0;
    for (uint32_t i = 0; files && i < n; i++) {
        const uint32_t it = files + 0x40 * i;
        const std::string p = vita_host_path(std::string("savedata0:") + gstr(rd32(it)));
        const uint32_t buf = rd32(it + 4), size = rd32(it + 8), mode = rd32(it + 24);
        const uint64_t off = (uint64_t)rd32(it + 16) | (uint64_t)rd32(it + 20) << 32;
        if (mode == 2) {
            std::filesystem::create_directories(p, ec);
            continue;
        }
        std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
        FILE *f = mode == 1 ? fopen(p.c_str(), "wb") : fopen(p.c_str(), "r+b");
        if (!f) f = fopen(p.c_str(), "w+b");
        if (!f) {
            logf("vitart: save: não abriu %s", p.c_str());
            return ret(ctx, 0x80100602u);
        }
        if (buf && size) {
            _fseeki64(f, (long long)off, SEEK_SET);
            fwrite(ptr<void>(buf), 1, size, f);
        }
        fclose(f);
        total += size;
        logf("vitart: save: %s (%u bytes em +%llu)", p.c_str(), size, (unsigned long long)off);
    }
    if (slot && rd32(slot + 4)) {
        std::vector<uint8_t> b(kSlotParamSize);
        std::memcpy(b.data(), ptr<void>(rd32(slot + 4)), b.size());
        std::memset(b.data(), 0, 4); // status: disponível
        const uint32_t kb = (uint32_t)((total + 1023) / 1024);
        std::memcpy(b.data() + 0x308, &kb, 4); // sizeKB
        const std::time_t now = std::time(nullptr);
        std::tm t{};
        localtime_s(&t, &now);
        const uint16_t dt[6] = {(uint16_t)(t.tm_year + 1900), (uint16_t)(t.tm_mon + 1), (uint16_t)t.tm_mday,
                                (uint16_t)t.tm_hour, (uint16_t)t.tm_min, (uint16_t)t.tm_sec};
        std::memcpy(b.data() + 0x30C, dt, sizeof dt); // modifiedTime
        const std::string sf = slot_file(rd32(slot));
        std::filesystem::create_directories(std::filesystem::path(sf).parent_path(), ec);
        if (FILE *f = fopen(sf.c_str(), "wb")) {
            fwrite(b.data(), 1, b.size(), f);
            fclose(f);
        }
    }
    if (arg(ctx, 4)) wr32(arg(ctx, 4), 0);
    ret(ctx, SCE_OK);
}

} // namespace vt
