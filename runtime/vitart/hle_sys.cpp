// HLE dos serviços de sistema do Vita para o vitart: áudio (NGS e SceAudio), entrada (SceCtrl,
// SceTouch, SceMotion), exibição, relógio (SceRtc, libc), tubos de mensagem, Fios overlays, NP,
// troféus, AppUtil e diálogos comuns. Assinaturas e layouts das estruturas: vita-headers.
//
// O NGS aqui cumpre o contrato da API (handles, buffers de parâmetros, estado das vozes), mas ainda
// não sintetiza som: o objetivo agora é o jogo seguir para o laço principal.
#include "vitart.h"

#include <chrono>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#pragma comment(lib, "user32.lib")
#endif

namespace vt {

constexpr uint32_t SCE_OK = 0;

static uint32_t zalloc(uint32_t size, const char *what) {
    const uint32_t a = galloc(Pool::user, size, 0x10, what);
    std::memset(ptr<void>(a), 0, size);
    return a;
}

// =================================================================== NGS
constexpr uint32_t NGS_ERROR_INVALID_ARG = 0x804A0002u;
constexpr uint32_t NGS_MODULE_PARAMS = 0x400; // bytes de parâmetros por módulo de voz
constexpr uint32_t NGS_MAX_MODULES = 8;

struct NgsVoice {
    uint32_t rack = 0;
    uint32_t state = 0; // SceNgsVoiceState: 1 ativa, 0x10 pausada, 0x20 key-off
    uint32_t params[NGS_MAX_MODULES] = {};
    uint32_t finished_cb = 0, finished_ud = 0;
    uint32_t updates = 0;
};
struct NgsRack {
    uint32_t def = 0;
    std::vector<uint32_t> voices;
    int channels = 2;
};
static std::mutex g_ngs_mutex;
static std::unordered_map<uint32_t, NgsVoice> g_voices;
static std::unordered_map<uint32_t, NgsRack> g_racks;
static std::unordered_map<uint32_t, uint32_t> g_patches; // patch -> SceNgsPatchSetupInfo copiado

// Cada sceNgsVoiceDefGet* devolve um ponteiro imutável e distinto; o jogo só o repassa ao rack.
static uint32_t voice_def(int kind) {
    static uint32_t defs[8] = {};
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    if (!defs[kind]) {
        defs[kind] = zalloc(0x40, "ngs:voicedef");
        wr32(defs[kind], 0x4E475344u + kind); // marca legível em dumps
    }
    return defs[kind];
}
VT_HLE(sceNgsVoiceDefGetScreamVoice) { ret(ctx, voice_def(0)); }
VT_HLE(sceNgsVoiceDefGetScreamAtrac9Voice) { ret(ctx, voice_def(1)); }
VT_HLE(sceNgsVoiceDefGetMasterBuss) { ret(ctx, voice_def(2)); }
VT_HLE(sceNgsVoiceDefGetReverbBuss) { ret(ctx, voice_def(3)); }
VT_HLE(sceNgsVoiceDefGetCompressorBuss) { ret(ctx, voice_def(4)); }
VT_HLE(sceNgsVoiceDefGetCompressorSideChainBuss) { ret(ctx, voice_def(5)); }

VT_HLE(sceNgsSystemGetRequiredMemorySize) {
    if (arg(ctx, 1)) wr32(arg(ctx, 1), 0x10000);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsSystemInit) {
    // (mem, size, params, out handle): o handle é o próprio bloco que o jogo nos deu.
    const uint32_t mem = arg(ctx, 0);
    if (!mem || !arg(ctx, 3)) return ret(ctx, NGS_ERROR_INVALID_ARG);
    wr32(arg(ctx, 3), mem);
    const uint32_t p = arg(ctx, 2);
    if (p)
        logf("vitart: NGS iniciado: %u racks, %u vozes, granularidade %u, %u Hz", rd32(p), rd32(p + 4), rd32(p + 8), rd32(p + 12));
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsSystemRelease) { ret(ctx, SCE_OK); }
VT_HLE(sceNgsSystemSetFlags) { ret(ctx, SCE_OK); }

VT_HLE(sceNgsRackGetRequiredMemorySize) {
    const uint32_t desc = arg(ctx, 1);
    const uint32_t voices = desc ? rd32(desc + 4) : 1;
    if (arg(ctx, 2)) wr32(arg(ctx, 2), 0x100 + voices * 0x40);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsRackInit) {
    // (system, SceNgsBufferInfo *buffer, const SceNgsRackDescription *desc, SceNgsHRack *out)
    const uint32_t buf = arg(ctx, 1), desc = arg(ctx, 2), out = arg(ctx, 3);
    if (!buf || !desc || !out) return ret(ctx, NGS_ERROR_INVALID_ARG);
    const uint32_t rack = rd32(buf);
    NgsRack r;
    r.def = rd32(desc);
    r.channels = (int)rd32(desc + 8);
    const uint32_t n = rd32(desc + 4);
    std::lock_guard<std::mutex> g(g_ngs_mutex);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t v = zalloc(0x20, "ngs:voice");
        g_voices[v].rack = rack;
        r.voices.push_back(v);
    }
    g_racks[rack] = std::move(r);
    wr32(out, rack);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsRackRelease) {
    // O callback opcional é chamado quando a liberação termina; aqui ela é imediata.
    const uint32_t rack = arg(ctx, 0), cb = arg(ctx, 1);
    {
        std::lock_guard<std::mutex> g(g_ngs_mutex);
        auto it = g_racks.find(rack);
        if (it != g_racks.end()) {
            for (uint32_t v : it->second.voices)
                g_voices.erase(v);
            g_racks.erase(it);
        }
    }
    (void)cb;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsRackGetVoiceHandle) {
    std::lock_guard<std::mutex> g(g_ngs_mutex);
    auto it = g_racks.find(arg(ctx, 0));
    if (it == g_racks.end() || arg(ctx, 1) >= it->second.voices.size() || !arg(ctx, 2))
        return ret(ctx, NGS_ERROR_INVALID_ARG);
    wr32(arg(ctx, 2), it->second.voices[arg(ctx, 1)]);
    ret(ctx, SCE_OK);
}

static NgsVoice *voice_of(uint32_t h) {
    auto it = g_voices.find(h);
    return it == g_voices.end() ? nullptr : &it->second;
}
VT_HLE(sceNgsVoiceInit) {
    std::lock_guard<std::mutex> g(g_ngs_mutex);
    NgsVoice *v = voice_of(arg(ctx, 0));
    if (!v) return ret(ctx, NGS_ERROR_INVALID_ARG);
    v->state = 0;
    v->updates = 0;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceLockParams) {
    // (voice, module, params_interface_id, SceNgsBufferInfo *out): o jogo escreve os parâmetros
    // do módulo no buffer devolvido; o primeiro campo do descritor é o id da interface.
    std::lock_guard<std::mutex> g(g_ngs_mutex);
    NgsVoice *v = voice_of(arg(ctx, 0));
    const uint32_t module = arg(ctx, 1), out = arg(ctx, 3);
    if (!v || module >= NGS_MAX_MODULES || !out) return ret(ctx, NGS_ERROR_INVALID_ARG);
    if (!v->params[module])
        v->params[module] = zalloc(NGS_MODULE_PARAMS, "ngs:params");
    wr32(v->params[module], arg(ctx, 2));
    wr32(v->params[module] + 4, NGS_MODULE_PARAMS);
    wr32(out, v->params[module]);
    wr32(out + 4, NGS_MODULE_PARAMS);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceUnlockParams) { ret(ctx, SCE_OK); }
VT_HLE(sceNgsVoiceSetParamsBlock) {
    if (arg(ctx, 3)) wr32(arg(ctx, 3), 0);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceGetParamsOutOfRange) {
    if (arg(ctx, 2)) ptr<char>(arg(ctx, 2))[0] = 0;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceBypassModule) { ret(ctx, SCE_OK); }
VT_HLE(sceNgsVoiceSetModuleCallback) { ret(ctx, SCE_OK); }
VT_HLE(sceNgsVoiceSetFinishedCallback) {
    std::lock_guard<std::mutex> g(g_ngs_mutex);
    if (NgsVoice *v = voice_of(arg(ctx, 0))) {
        v->finished_cb = arg(ctx, 1);
        v->finished_ud = arg(ctx, 2);
    }
    ret(ctx, SCE_OK);
}
static void voice_state(arc_ctx *ctx, uint32_t set, uint32_t clear) {
    std::lock_guard<std::mutex> g(g_ngs_mutex);
    NgsVoice *v = voice_of(arg(ctx, 0));
    if (!v) return ret(ctx, NGS_ERROR_INVALID_ARG);
    v->state = (v->state & ~clear) | set;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoicePlay) { voice_state(ctx, 1, 0x30); }
VT_HLE(sceNgsVoicePause) { voice_state(ctx, 0x10, 0); }
VT_HLE(sceNgsVoiceResume) { voice_state(ctx, 0, 0x10); }
VT_HLE(sceNgsVoiceKeyOff) { voice_state(ctx, 0, 0xFFFFFFFFu); } // sem síntese: o fim é imediato
VT_HLE(sceNgsVoiceKill) { voice_state(ctx, 0, 0xFFFFFFFFu); }
VT_HLE(sceNgsVoiceGetInfo) {
    std::lock_guard<std::mutex> g(g_ngs_mutex);
    NgsVoice *v = voice_of(arg(ctx, 0));
    const uint32_t info = arg(ctx, 1);
    if (!v || !info) return ret(ctx, NGS_ERROR_INVALID_ARG);
    std::memset(ptr<void>(info), 0, 0x18);
    wr32(info, v->state);
    wr32(info + 4, 4);
    wr32(info + 8, 1);
    wr32(info + 12, 1);
    wr32(info + 16, 1);
    wr32(info + 20, v->updates);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceGetStateData) {
    if (arg(ctx, 2)) std::memset(ptr<void>(arg(ctx, 2)), 0, arg(ctx, 3));
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsSystemUpdate) {
    std::lock_guard<std::mutex> g(g_ngs_mutex);
    for (auto &[h, v] : g_voices)
        if (v.state & 1) ++v.updates;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsPatchCreateRouting) {
    const uint32_t info = arg(ctx, 0), out = arg(ctx, 1);
    if (!info || !out) return ret(ctx, NGS_ERROR_INVALID_ARG);
    const uint32_t p = zalloc(0x14, "ngs:patch");
    std::memcpy(ptr<void>(p), ptr<void>(info), 0x14);
    {
        std::lock_guard<std::mutex> g(g_ngs_mutex);
        g_patches[p] = p;
    }
    wr32(out, p);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsPatchRemoveRouting) {
    std::lock_guard<std::mutex> g(g_ngs_mutex);
    g_patches.erase(arg(ctx, 0));
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsPatchGetInfo) {
    // (patch, SceNgsPatchRouteInfo *route, SceNgsPatchSetupInfo *setup)
    const uint32_t p = arg(ctx, 0);
    if (arg(ctx, 1)) {
        std::memset(ptr<void>(arg(ctx, 1)), 0, 0x18);
        wr32(arg(ctx, 1), 2);
        wr32(arg(ctx, 1) + 4, 2);
    }
    if (arg(ctx, 2) && p) std::memcpy(ptr<void>(arg(ctx, 2)), ptr<void>(p), 0x14);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoicePatchSetVolumesMatrix) { ret(ctx, SCE_OK); }
VT_HLE(sceNgsAT9GetSectionDetails) {
    if (arg(ctx, 3)) std::memset(ptr<void>(arg(ctx, 3)), 0, 0x10);
    ret(ctx, SCE_OK);
}

// =================================================================== SceAudio
// Saída cadenciada: sceAudioOutOutput bloqueia até o tempo do bloco anterior passar, como o
// hardware; o jogo usa isso como relógio da thread de áudio.
struct AudioPort {
    uint32_t len = 256, freq = 48000, mode = 1;
    std::chrono::steady_clock::time_point next{};
};
static std::mutex g_audio_mutex;
static std::map<int, AudioPort> g_ports;
static int g_next_port = 0;

VT_HLE(sceAudioOutOpenPort) {
    // (type, len, freq, mode)
    std::lock_guard<std::mutex> g(g_audio_mutex);
    const int id = g_next_port++;
    AudioPort &p = g_ports[id];
    p.len = arg(ctx, 1);
    p.freq = arg(ctx, 2) ? arg(ctx, 2) : 48000;
    p.mode = arg(ctx, 3);
    p.next = std::chrono::steady_clock::now();
    logf("vitart: porta de áudio %d: tipo %u, %u amostras, %u Hz, %s", id, arg(ctx, 0), p.len, p.freq, p.mode ? "estéreo" : "mono");
    ret(ctx, (uint32_t)id);
}
VT_HLE(sceAudioOutReleasePort) {
    std::lock_guard<std::mutex> g(g_audio_mutex);
    g_ports.erase((int)arg(ctx, 0));
    ret(ctx, SCE_OK);
}
VT_HLE(sceAudioOutSetVolume) { ret(ctx, SCE_OK); }
VT_HLE(sceAudioOutGetRestSample) { ret(ctx, 0); }
VT_HLE(sceAudioOutOutput) {
    std::chrono::steady_clock::time_point wake;
    {
        std::lock_guard<std::mutex> g(g_audio_mutex);
        auto it = g_ports.find((int)arg(ctx, 0));
        if (it == g_ports.end()) return ret(ctx, 0x80260105u); // SCE_AUDIO_OUT_ERROR_INVALID_PORT
        AudioPort &p = it->second;
        const auto now = std::chrono::steady_clock::now();
        if (p.next < now - std::chrono::milliseconds(100)) p.next = now; // após uma pausa longa
        wake = p.next;
        p.next += std::chrono::microseconds((uint64_t)p.len * 1000000ull / p.freq);
    }
    std::this_thread::sleep_until(wake);
    ret(ctx, SCE_OK);
}

// =================================================================== entrada
// Controle pelo teclado do host: setas = direcional, Z/X/A/S = cruz/círculo/quadrado/triângulo,
// Q/W = L/R, Enter = start, Backspace = select. Analógicos centrados.
static uint32_t keyboard_buttons() {
    uint32_t b = 0;
#ifdef _WIN32
    struct { int vk; uint32_t bit; } map[] = {
        { VK_BACK, 0x1 }, { VK_RETURN, 0x8 }, { VK_UP, 0x10 }, { VK_RIGHT, 0x20 }, { VK_DOWN, 0x40 },
        { VK_LEFT, 0x80 }, { 'Q', 0x100 }, { 'W', 0x200 }, { 'S', 0x1000 }, { 'X', 0x2000 },
        { 'Z', 0x4000 }, { 'A', 0x8000 },
    };
    for (auto &m : map)
        if (GetAsyncKeyState(m.vk) & 0x8000) b |= m.bit;
#endif
    return b;
}
static uint64_t now_us_sys() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
VT_HLE(sceCtrlSetSamplingMode) { ret(ctx, 0); } // devolve o modo anterior (desligado)
VT_HLE(sceCtrlSetSamplingModeExt) { ret(ctx, 0); }
static void ctrl_read(arc_ctx *ctx) {
    // (port, SceCtrlData *data, count): 0x20 bytes por amostra.
    const uint32_t data = arg(ctx, 1);
    const int count = (int)arg(ctx, 2) > 0 ? (int)arg(ctx, 2) : 1;
    const uint32_t buttons = keyboard_buttons();
    for (int i = 0; i < count; ++i) {
        const uint32_t d = data + 0x20u * i;
        std::memset(ptr<void>(d), 0, 0x20);
        wr64(d, now_us_sys());
        wr32(d + 8, buttons);
        for (int s = 0; s < 4; ++s) ptr<uint8_t>(d)[12 + s] = 128;
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
VT_HLE(sceTouchPeek) {
    const uint32_t data = arg(ctx, 1);
    const uint32_t n = arg(ctx, 2) ? arg(ctx, 2) : 1;
    for (uint32_t i = 0; i < n; ++i) {
        std::memset(ptr<void>(data + 0x90 * i), 0, 0x90);
        wr64(data + 0x90 * i, now_us_sys());
    }
    ret(ctx, n);
}
VT_HLE(sceTouchRead) {
    const uint32_t data = arg(ctx, 1);
    std::memset(ptr<void>(data), 0, 0x90);
    wr64(data, now_us_sys());
    ret(ctx, 1);
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
    const auto now = std::chrono::steady_clock::now();
    const auto n = (now - g_vblank0) / VBLANK;
    std::this_thread::sleep_until(g_vblank0 + VBLANK * (n + (count ? count : 1)));
}
VT_HLE(sceDisplayWaitVblankStart) { wait_vblank(1); ret(ctx, SCE_OK); }
VT_HLE(sceDisplayWaitVblankStartMulti) { wait_vblank(arg(ctx, 0)); ret(ctx, SCE_OK); }
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
constexpr uint32_t ERR_MPP_EMPTY = 0x800201B4u;
constexpr uint32_t ERR_MPP_FULL = 0x800201B3u;
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
VT_HLE(sceNpTrophyCreateContext) {
    if (arg(ctx, 0)) wr32(arg(ctx, 0), 1);
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
    logf("vitart: troféu %d desbloqueado", (int)arg(ctx, 2));
    if (arg(ctx, 3)) wr32(arg(ctx, 3), 0xFFFFFFFFu); // SCE_NP_TROPHY_INVALID_TROPHY_ID: sem platina
    ret(ctx, SCE_OK);
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
VT_HLE(sceAppUtilSaveDataSlotGetParam) { ret(ctx, APPUTIL_SLOT_NOT_FOUND); }
VT_HLE(sceAppUtilSaveDataDataSave) {
    // TODO: gravar os arquivos do slot no diretório de dados; por ora só registra o pedido.
    logf("vitart: sceAppUtilSaveDataDataSave(slot %u, %u arquivos) ignorado", arg(ctx, 0), arg(ctx, 2));
    if (arg(ctx, 4)) wr32(arg(ctx, 4), 0);
    ret(ctx, SCE_OK);
}

} // namespace vt
