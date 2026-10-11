// HLE do NGS (o mixer/sintetizador de áudio do Vita) para o vitart.
//
// O jogo monta racks de vozes a partir de definições (Scream = player PCM/HE-ADPCM, Scream ATRAC9,
// e os busses master, reverb e compressor), liga saídas a entradas com patches (cada um com uma matriz
// de volume 2x2) e chama sceNgsSystemUpdate uma vez por grão (granularidade, 512 amostras no GoW). O
// master buss entrega o grão em PCM estéreo de 16 bits por sceNgsVoiceGetStateData(master, 1), que o
// jogo manda para sceAudioOutOutput.
//
// Aqui: as vozes fonte decodificam os buffers que o jogo passa nos parâmetros do módulo 0 (ATRAC9 pelo
// LibAtrac9, PCM de 16 bits), com troca e laço de buffers e os callbacks de módulo que o jogo usa para
// reabastecer o streaming; a reamostragem é linear; os busses somam as entradas e repassam (reverb e
// compressor ainda passam sem efeito: o reverb não devolve nada, o compressor repassa). Layouts dos
// parâmetros e estados: vita-headers (ngs.h) e medidas no próprio jogo.
#include "vitart.h"

#include <libatrac9.h>

#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace vt {
namespace {

constexpr uint32_t SCE_OK = 0;
constexpr uint32_t NGS_ERROR_INVALID_ARG = 0x804A0002u;
constexpr uint32_t kParamBytes = 0x200; // buffer de parâmetros por módulo

// SceNgsVoiceState
constexpr uint32_t ST_ACTIVE = 1, ST_FINALIZE = 4, ST_PAUSED = 0x20, ST_KEY_OFF = 0x40;

enum Kind { K_SCREAM = 0, K_SCREAM_AT9, K_MASTER, K_REVERB, K_COMPRESSOR, K_SIDECHAIN, K_COUNT };
constexpr uint32_t kModules[K_COUNT] = {6, 6, 2, 2, 2, 3};
constexpr uint32_t kOutputs[K_COUNT] = {2, 2, 1, 1, 1, 1};
constexpr uint32_t kInputs[K_COUNT] = {0, 0, 1, 1, 1, 2};
constexpr uint32_t kAt9Id = 0x5CAA, kPlayerId = 0x5CE6;

uint32_t zalloc(uint32_t size, const char *what) {
    const uint32_t a = galloc(Pool::user, size, 0x10, what);
    std::memset(ptr<void>(a), 0, size);
    return a;
}
float rdf(uint32_t a) {
    float f;
    std::memcpy(&f, ptr<void>(a), 4);
    return f;
}

struct Voice {
    uint32_t handle = 0, rack = 0;
    Kind kind = K_SCREAM;
    int channels = 2; // canais por voz do rack
    uint32_t state = 0, updates = 0;
    bool key_off = false;
    uint32_t params[8] = {}; // buffer de parâmetros de cada módulo (guest)
    uint32_t mod_cb[8] = {}, mod_ud[8] = {};
    uint32_t fin_cb = 0, fin_ud = 0;
    // Fonte: posição nos buffers do jogo e PCM decodificado (estéreo float) à espera de sair.
    int32_t cur_buffer = 0, byte_pos = 0, loops = 0;
    int32_t samples_since_key_on = 0, bytes_since_key_on = 0, samples_total = 0, bytes_total = 0;
    std::vector<float> pcm;
    size_t pcm_read = 0;
    double frac = 0.0;     // posição fracionária da reamostragem
    bool src_done = false; // os buffers acabaram (falta só escoar o PCM)
    std::vector<uint8_t> staging; // superquadro ATRAC9 dividido entre dois buffers
    int32_t hist[2][4] = {}; // histórico do HE-ADPCM por canal
    void *at9 = nullptr;
    uint32_t at9_config = 0;
    Atrac9CodecInfo at9_info{};
    // Bus: soma das entradas deste grão; saída do grão (fonte ou bus).
    std::vector<float> in, out;
    bool has_out = false;
    std::vector<int16_t> master_pcm;
    ~Voice() {
        if (at9) Atrac9ReleaseHandle(at9);
    }
};
struct Rack {
    uint32_t def = 0;
    Kind kind = K_SCREAM;
    int channels = 2;
    uint32_t patches_per_output = 1;
    std::vector<uint32_t> voices;
};
struct Patch {
    uint32_t src = 0, dst = 0;
    int32_t out_index = 0, out_sub = 0, in_index = 0;
    float m[2][2] = {};
};

std::mutex g_mutex;
std::map<uint32_t, std::shared_ptr<Voice>> g_voices;
std::map<uint32_t, Rack> g_racks;
std::map<uint32_t, Patch> g_patches;
std::vector<std::pair<uint32_t, uint32_t>> g_release_cbs; // (callback, rack) para o próximo update
uint32_t g_defs[K_COUNT] = {};
int g_granularity = 512, g_rate = 48000;
bool g_log = false;

uint32_t voice_def(int kind) {
    static std::mutex m;
    std::lock_guard<std::mutex> g(m);
    if (!g_defs[kind]) {
        g_defs[kind] = zalloc(0x40, "ngs:voicedef");
        wr32(g_defs[kind], 0x4E475344u + kind);
    }
    return g_defs[kind];
}
Kind kind_of_def(uint32_t def) {
    for (int k = 0; k < K_COUNT; k++)
        if (g_defs[k] == def) return (Kind)k;
    return K_SCREAM;
}
std::shared_ptr<Voice> voice_of(uint32_t h) {
    auto it = g_voices.find(h);
    return it == g_voices.end() ? nullptr : it->second;
}
bool is_source(const Voice &v) { return v.kind == K_SCREAM || v.kind == K_SCREAM_AT9; }

// ------------------------------------------------------------------- parâmetros das fontes
// SceNgsAT9Params: buffers de 16 bytes em +8 {ptr, bytes, loop i16, next i16, descarte início/fim i16},
// frequência +72, escala +76, canais +88, config +92. SceNgsPlayerParams: buffers de 12 bytes em +8
// {ptr, bytes, loop i16, next i16}, frequência +56, escala +60, início em bytes +72, canais +76,
// tipo +79 (0 PCM, 1 HE-ADPCM), buffer inicial +81.
struct BufRef {
    uint32_t ptr = 0;
    int32_t bytes = 0;
    int16_t loop = 0, next = -1, skip_start = 0, skip_end = 0;
};
BufRef buf_param(const Voice &v, int i) {
    BufRef b;
    if (i < 0 || i > 3) return b;
    const uint32_t p = v.params[0];
    if (v.kind == K_SCREAM_AT9) {
        const uint32_t a = p + 8 + 16 * i;
        b.ptr = rd32(a), b.bytes = (int32_t)rd32(a + 4);
        b.loop = *ptr<int16_t>(a + 8), b.next = *ptr<int16_t>(a + 10);
        b.skip_start = *ptr<int16_t>(a + 12), b.skip_end = *ptr<int16_t>(a + 14);
    } else {
        const uint32_t a = p + 8 + 12 * i;
        b.ptr = rd32(a), b.bytes = (int32_t)rd32(a + 4);
        b.loop = *ptr<int16_t>(a + 8), b.next = *ptr<int16_t>(a + 10);
    }
    return b;
}
double play_rate(const Voice &v) {
    const uint32_t p = v.params[0];
    float f = rdf(p + (v.kind == K_SCREAM_AT9 ? 72 : 56)), s = rdf(p + (v.kind == K_SCREAM_AT9 ? 76 : 60));
    if (!(f > 0.f && f <= 192000.f)) f = (float)g_rate;
    if (!(s > 0.f && s <= 10.f)) s = 1.f;
    return (double)f * s / g_rate;
}

// Callback de módulo ou de fim: SceNgsCallbackInfo {voz, rack, id do módulo, motivo, motivo 2, ptr,
// dado do usuário} na pilha do guest. Chamado sem o mutex: o jogo costuma mexer no NGS dentro dele.
void invoke(arc_ctx *ctx, std::unique_lock<std::mutex> &lock, const Voice &v, uint32_t cb, uint32_t ud,
            uint32_t module_id, uint32_t reason, uint32_t reason2, uint32_t rptr) {
    if (!cb || !ctx) return;
    const uint32_t handle = v.handle, rack = v.rack;
    lock.unlock();
    const uint32_t sp = ctx->r[13];
    const uint32_t info = (sp - 32) & ~7u;
    wr32(info, handle);
    wr32(info + 4, rack);
    wr32(info + 8, module_id);
    wr32(info + 12, reason);
    wr32(info + 16, reason2);
    wr32(info + 20, rptr);
    wr32(info + 24, ud);
    ctx->r[13] = info;
    call_guest(ctx, cb, {info});
    ctx->r[13] = sp;
    lock.lock();
}

// Fim do buffer atual: laço, troca para o próximo ou fim dos dados (com o callback do módulo 0).
// Devolve false quando não há mais dados.
bool advance_buffer(arc_ctx *ctx, std::unique_lock<std::mutex> &lock, Voice &v) {
    const bool at9 = v.kind == K_SCREAM_AT9;
    const uint32_t mid = at9 ? kAt9Id : kPlayerId;
    const BufRef b = buf_param(v, v.cur_buffer);
    v.byte_pos = 0;
    v.loops++;
    if (b.loop != -1 && v.loops > b.loop) {
        const int32_t prev = v.cur_buffer;
        v.cur_buffer = b.next;
        v.loops = 0;
        const BufRef n = buf_param(v, v.cur_buffer);
        if (v.cur_buffer < 0 || !n.ptr || n.bytes <= 0) {
            invoke(ctx, lock, v, v.mod_cb[0], v.mod_ud[0], mid, 0 /* fim dos dados */, 0, 0);
            return false;
        }
        invoke(ctx, lock, v, v.mod_cb[0], v.mod_ud[0], mid, 1 /* buffer trocado */, (uint32_t)prev, n.ptr);
    } else {
        invoke(ctx, lock, v, v.mod_cb[0], v.mod_ud[0], mid, at9 ? 4u : 2u /* laço */, (uint32_t)v.loops, b.ptr);
    }
    return true;
}

// HE-ADPCM (HEVAG): quadros de 16 bytes, 28 amostras; byte 0 = deslocamento (nibble baixo) e índice
// do coeficiente (nibble alto), byte 1 = bits altos do índice (nibble alto) e flags (nibble baixo; 7 =
// silêncio). Predição de 4 amostras. Só as 14 primeiras linhas da tabela são conhecidas (as 5 primeiras
// são as do VAG do PS1/PS2 escaladas); índice fora delas é contado e predito com zero.
const int16_t kHevag[14][4] = {
    {0, 0, 0, 0},           {7680, 0, 0, 0},          {14720, -6656, 0, 0},       {12544, -7040, 0, 0},
    {15616, -7680, 0, 0},   {14731, -7059, 0, 0},     {14507, -7366, 0, 0},       {13920, -7522, 0, 0},
    {13133, -7680, 0, 0},   {12028, -7680, 0, 0},     {10764, -7680, 0, 0},       {9359, -7680, 0, 0},
    {13064, -4075, -2824, 1877}, {5333, 2999, 775, -1132}};
uint64_t g_hevag_unknown = 0, g_hevag_frames = 0;
void hevag_frame(const uint8_t *f, int32_t h[4], int16_t *out, int stride) {
    unsigned coef = (f[0] >> 4) | (f[1] & 0xF0);
    const int shift = 20 - (f[0] & 15), flag = f[1] & 15;
    g_hevag_frames++;
    if (coef >= 14) g_hevag_unknown++, coef = 0;
    const int16_t *c = kHevag[coef];
    for (int j = 0; j < 28; j++) {
        int32_t s = 0;
        if (flag < 7) {
            int n = (f[2 + j / 2] >> ((j & 1) * 4)) & 15;
            if (n >= 8) n -= 16;
            s = (n << shift) + ((h[0] * c[0] + h[1] * c[1] + h[2] * c[2] + h[3] * c[3]) >> 5);
            s >>= 8;
        }
        out[j * stride] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
        h[3] = h[2], h[2] = h[1], h[1] = h[0], h[0] = s;
    }
}

void push_frames(Voice &v, const int16_t *s, int frames, int ch) {
    for (int i = 0; i < frames; i++) {
        const float l = s[i * ch] / 32768.f, r = ch > 1 ? s[i * ch + 1] / 32768.f : l;
        v.pcm.push_back(l);
        v.pcm.push_back(r);
    }
}

// Decodifica mais um pedaço para v.pcm. Devolve false quando os dados acabaram.
bool decode_more(arc_ctx *ctx, std::unique_lock<std::mutex> &lock, Voice &v) {
    BufRef b = buf_param(v, v.cur_buffer);
    if (v.cur_buffer < 0 || !b.ptr || b.bytes <= 0) return false;
    if (v.byte_pos >= b.bytes) return advance_buffer(ctx, lock, v);

    if (v.kind == K_SCREAM) {
        const int ch = *ptr<int8_t>(v.params[0] + 76) == 1 ? 1 : 2;
        if (*ptr<int8_t>(v.params[0] + 79) != 0) { // HE-ADPCM: quadros intercalados por canal
            const int groups = (std::min)((b.bytes - v.byte_pos) / (16 * ch), 64);
            if (groups <= 0) {
                v.byte_pos = b.bytes;
                return true;
            }
            std::vector<int16_t> pcm((size_t)groups * 28 * ch);
            const uint8_t *src = ptr<uint8_t>(b.ptr + v.byte_pos);
            for (int gi = 0; gi < groups; gi++)
                for (int c = 0; c < ch; c++) hevag_frame(src + (gi * ch + c) * 16, v.hist[c], &pcm[(size_t)gi * 28 * ch + c], ch);
            push_frames(v, pcm.data(), groups * 28, ch);
            v.byte_pos += groups * 16 * ch;
            v.bytes_since_key_on += groups * 16 * ch, v.bytes_total += groups * 16 * ch;
            v.samples_since_key_on += groups * 28 * ch, v.samples_total += groups * 28 * ch;
            return true;
        }
        const int frames = (std::min)((b.bytes - v.byte_pos) / (2 * ch), 4096);
        if (frames <= 0) {
            v.byte_pos = b.bytes;
            return true;
        }
        push_frames(v, ptr<int16_t>(b.ptr + v.byte_pos), frames, ch);
        v.byte_pos += frames * 2 * ch;
        v.bytes_since_key_on += frames * 2 * ch, v.bytes_total += frames * 2 * ch;
        v.samples_since_key_on += frames * ch, v.samples_total += frames * ch;
        return true;
    }

    // ATRAC9: um superquadro por vez.
    const uint32_t config = rd32(v.params[0] + 92);
    if (!v.at9 || config != v.at9_config) {
        if (v.at9) Atrac9ReleaseHandle(v.at9);
        v.at9 = Atrac9GetHandle();
        uint8_t cfg[4];
        std::memcpy(cfg, &config, 4);
        if (Atrac9InitDecoder(v.at9, cfg) != 0 || Atrac9GetCodecInfo(v.at9, &v.at9_info) != 0) {
            logf("vitart: NGS: configuração ATRAC9 inválida 0x%08x", config);
            Atrac9ReleaseHandle(v.at9);
            v.at9 = nullptr;
            return false;
        }
        v.at9_config = config;
        v.staging.clear();
    }
    const Atrac9CodecInfo &ci = v.at9_info;
    const uint8_t *in = ptr<uint8_t>(b.ptr + v.byte_pos);
    // Alguns jogos passam o arquivo com o cabeçalho RIFF: pula até o bloco "data".
    if (v.staging.empty() && b.bytes - v.byte_pos >= 12 && !std::memcmp(in, "RIFF", 4) && !std::memcmp(in + 8, "WAVE", 4)) {
        uint32_t p = v.byte_pos + 12;
        while ((int32_t)p + 8 <= b.bytes && std::memcmp(ptr<void>(b.ptr + p), "data", 4))
            p += 8 + rd32(b.ptr + p + 4);
        v.byte_pos = (int32_t)p + 8;
        return true;
    }
    const int32_t left = b.bytes - v.byte_pos;
    const int sps = ci.frameSamples * ci.framesInSuperframe;
    int32_t sf_index = v.byte_pos / ci.superframeSize, sf_total = b.bytes / ci.superframeSize;
    if (left < ci.superframeSize || !v.staging.empty()) {
        const size_t take = (std::min)((size_t)left, (size_t)ci.superframeSize - v.staging.size());
        v.staging.insert(v.staging.end(), in, in + take);
        v.byte_pos += (int32_t)take;
        if ((int)v.staging.size() < ci.superframeSize) return true; // completa no próximo buffer
        in = v.staging.data();
        sf_index = sf_total = 0x7FFFFFF; // sem descarte no superquadro dividido
    } else {
        v.byte_pos += ci.superframeSize;
    }
    std::vector<int16_t> pcm((size_t)ci.frameSamples * ci.channels);
    std::vector<int16_t> sf;
    for (int f = 0; f < ci.framesInSuperframe; f++) {
        int used = 0;
        if (Atrac9Decode(v.at9, in, pcm.data(), &used) != 0) {
            invoke(ctx, lock, v, v.mod_cb[0], v.mod_ud[0], kAt9Id, 3 /* erro de decodificação */, (uint32_t)v.byte_pos, b.ptr);
            break;
        }
        in += used;
        sf.insert(sf.end(), pcm.begin(), pcm.end());
    }
    v.staging.clear();
    int start = 0, count = (int)(sf.size() / ci.channels);
    if (sf_index != 0x7FFFFFF) {
        const int idx = sf_index * sps;
        if (b.skip_start > idx) start = (std::min)(count, b.skip_start - idx);
        const int after = (sf_total - sf_index - 1) * sps;
        if (b.skip_end > after) count -= (std::min)(count - start, b.skip_end - after);
    }
    if (count > start) push_frames(v, sf.data() + (size_t)start * ci.channels, count - start, ci.channels);
    v.bytes_since_key_on += ci.superframeSize, v.bytes_total += ci.superframeSize;
    v.samples_since_key_on += (count - start) * ci.channels, v.samples_total += (count - start) * ci.channels;
    return true;
}

// Um grão de saída da fonte (estéreo float em v.out). Devolve true quando a voz terminou.
bool render_source(arc_ctx *ctx, std::unique_lock<std::mutex> &lock, Voice &v) {
    const int G = g_granularity;
    v.out.assign((size_t)G * 2, 0.f);
    v.has_out = true;
    const double rate = play_rate(v);
    auto avail = [&] { return (v.pcm.size() / 2) - v.pcm_read; };
    for (int i = 0; i < G; i++) {
        const size_t idx = (size_t)v.frac;
        while (!v.src_done && avail() < idx + 2) {
            if (!decode_more(ctx, lock, v)) v.src_done = true;
        }
        if (avail() < idx + 1) return true; // acabou o PCM
        const float t = (float)(v.frac - (double)idx);
        const float *a = &v.pcm[(v.pcm_read + idx) * 2];
        const float *b2 = avail() > idx + 1 ? a + 2 : a;
        v.out[2 * i] = a[0] + (b2[0] - a[0]) * t;
        v.out[2 * i + 1] = a[1] + (b2[1] - a[1]) * t;
        v.frac += rate;
    }
    const size_t used = (std::min)((size_t)v.frac, avail());
    v.pcm_read += used;
    v.frac -= (double)used;
    if (v.pcm_read > 8192) { // compacta
        v.pcm.erase(v.pcm.begin(), v.pcm.begin() + (ptrdiff_t)v.pcm_read * 2);
        v.pcm_read = 0;
    }
    return v.src_done && avail() == 0;
}

void reset_source(Voice &v) {
    v.byte_pos = 0, v.cur_buffer = 0, v.loops = 0;
    if (v.kind == K_SCREAM && v.params[0]) {
        v.cur_buffer = *ptr<int8_t>(v.params[0] + 81);
        v.byte_pos = (int32_t)rd32(v.params[0] + 72);
        if (v.cur_buffer < 0 || v.cur_buffer > 3) v.cur_buffer = 0;
        if (v.byte_pos < 0) v.byte_pos = 0;
    }
    v.samples_since_key_on = v.bytes_since_key_on = 0;
    v.pcm.clear(), v.pcm_read = 0, v.frac = 0.0, v.src_done = false;
    v.staging.clear();
    std::memset(v.hist, 0, sizeof v.hist);
    if (v.at9) Atrac9ReleaseHandle(v.at9), v.at9 = nullptr;
    v.at9_config = 0;
}

void mix_into(Voice &dst, const Voice &src, const Patch &p) {
    const int G = g_granularity;
    if (dst.in.size() != (size_t)G * 2) dst.in.assign((size_t)G * 2, 0.f);
    float m00 = p.m[0][0], m01 = p.m[0][1], m10 = p.m[1][0], m11 = p.m[1][1];
    if (src.channels == 1) m10 = m11 = 0.f;
    if (dst.channels == 1) m01 = m11 = 0.f;
    for (int k = 0; k < G; k++) {
        const float l = src.out[2 * k], r = src.out[2 * k + 1];
        dst.in[2 * k] += l * m00 + r * m10;
        dst.in[2 * k + 1] += l * m01 + r * m11;
    }
}

} // namespace

// ------------------------------------------------------------------- sistema, racks e vozes
VT_HLE(sceNgsVoiceDefGetScreamVoice) { ret(ctx, voice_def(K_SCREAM)); }
VT_HLE(sceNgsVoiceDefGetScreamAtrac9Voice) { ret(ctx, voice_def(K_SCREAM_AT9)); }
VT_HLE(sceNgsVoiceDefGetMasterBuss) { ret(ctx, voice_def(K_MASTER)); }
VT_HLE(sceNgsVoiceDefGetReverbBuss) { ret(ctx, voice_def(K_REVERB)); }
VT_HLE(sceNgsVoiceDefGetCompressorBuss) { ret(ctx, voice_def(K_COMPRESSOR)); }
VT_HLE(sceNgsVoiceDefGetCompressorSideChainBuss) { ret(ctx, voice_def(K_SIDECHAIN)); }

VT_HLE(sceNgsSystemGetRequiredMemorySize) {
    if (arg(ctx, 1)) wr32(arg(ctx, 1), 0x10000);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsSystemInit) {
    // (mem, size, SceNgsSystemInitParams {racks, vozes, granularidade, taxa}, out handle)
    const uint32_t mem = arg(ctx, 0), p = arg(ctx, 2);
    if (!mem || !arg(ctx, 3)) return ret(ctx, NGS_ERROR_INVALID_ARG);
    wr32(arg(ctx, 3), mem);
    if (p) {
        const int gr = (int)rd32(p + 8), rate = (int)rd32(p + 12);
        if (gr > 0 && gr <= 4096) g_granularity = gr;
        if (rate > 0) g_rate = rate;
        logf("vitart: NGS iniciado: %u racks, %u vozes, granularidade %d, %d Hz", rd32(p), rd32(p + 4), g_granularity, g_rate);
    }
    g_log = getenv("VITART_NGS_LOG") != nullptr;
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
    // (system, SceNgsBufferInfo *buffer, SceNgsRackDescription {def, vozes, canais, patches por
    // entrada, patches por saída}, SceNgsHRack *out): o handle é o próprio bloco do jogo.
    const uint32_t buf = arg(ctx, 1), desc = arg(ctx, 2), out = arg(ctx, 3);
    if (!buf || !desc || !out) return ret(ctx, NGS_ERROR_INVALID_ARG);
    const uint32_t rack = rd32(buf);
    Rack r;
    r.def = rd32(desc);
    r.kind = kind_of_def(r.def);
    r.channels = rd32(desc + 8) == 1 ? 1 : 2;
    r.patches_per_output = rd32(desc + 16);
    const uint32_t n = rd32(desc + 4);
    std::lock_guard<std::mutex> g(g_mutex);
    for (uint32_t i = 0; i < n; ++i) {
        auto v = std::make_shared<Voice>();
        v->handle = zalloc(0x20, "ngs:voice");
        v->rack = rack;
        v->kind = r.kind;
        v->channels = r.channels;
        for (uint32_t m = 0; m < kModules[r.kind]; m++) v->params[m] = zalloc(kParamBytes, "ngs:params");
        r.voices.push_back(v->handle);
        g_voices[v->handle] = v;
    }
    if (g_log) logf("vitart: NGS rack 0x%08x: tipo %d, %u vozes, %d canais", rack, (int)r.kind, n, r.channels);
    g_racks[rack] = std::move(r);
    wr32(out, rack);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsRackRelease) {
    // O callback opcional é chamado quando a liberação termina: no próximo update.
    const uint32_t rack = arg(ctx, 0), cb = arg(ctx, 1);
    std::lock_guard<std::mutex> g(g_mutex);
    auto it = g_racks.find(rack);
    if (it != g_racks.end()) {
        for (uint32_t v : it->second.voices) {
            g_voices.erase(v);
            for (auto p = g_patches.begin(); p != g_patches.end();)
                p = (p->second.src == v || p->second.dst == v) ? g_patches.erase(p) : std::next(p);
        }
        g_racks.erase(it);
    }
    if (cb) g_release_cbs.push_back({cb, rack});
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsRackGetVoiceHandle) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto it = g_racks.find(arg(ctx, 0));
    if (it == g_racks.end() || arg(ctx, 1) >= it->second.voices.size() || !arg(ctx, 2))
        return ret(ctx, NGS_ERROR_INVALID_ARG);
    wr32(arg(ctx, 2), it->second.voices[arg(ctx, 1)]);
    ret(ctx, SCE_OK);
}

VT_HLE(sceNgsVoiceInit) {
    // (voz, preset, flags): volta ao estado inicial.
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    if (!v) return ret(ctx, NGS_ERROR_INVALID_ARG);
    v->state = 0, v->updates = 0, v->key_off = false;
    reset_source(*v);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceLockParams) {
    // (voz, módulo, id da interface, SceNgsBufferInfo *out): o jogo escreve direto no buffer devolvido.
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    const uint32_t module = arg(ctx, 1), out = arg(ctx, 3);
    if (!v || module >= 8 || !v->params[module] || !out) return ret(ctx, NGS_ERROR_INVALID_ARG);
    if (!rd32(v->params[module])) wr32(v->params[module], arg(ctx, 2));
    wr32(out, v->params[module]);
    wr32(out + 4, kParamBytes);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceUnlockParams) { ret(ctx, SCE_OK); }
VT_HLE(sceNgsVoiceSetParamsBlock) {
    // (voz, bloco, tamanho, int *erros): sequência de {módulo, canal} + descritor {id, tamanho} + dados,
    // o tamanho contando o descritor.
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    uint32_t p = arg(ctx, 1);
    const uint32_t end = p + arg(ctx, 2);
    int errors = 0;
    while (v && p + 16 <= end) {
        const uint32_t module = rd32(p), size = rd32(p + 12);
        if (module < 8 && v->params[module] && size >= 8 && size <= kParamBytes)
            std::memcpy(ptr<void>(v->params[module]), ptr<void>(p + 8), size);
        else
            errors++;
        if (size < 8) break;
        p += 8 + size;
    }
    if (g_log && v && is_source(*v)) {
        const uint32_t q = v->params[0];
        logf("vitart: NGS voz 0x%08x (%s): buf0 0x%08x %d bytes, laço %d, freq %.0f, canais %d, %s 0x%08x",
             v->handle, v->kind == K_SCREAM_AT9 ? "AT9" : "player", rd32(q + 8), (int)rd32(q + 12), *ptr<int16_t>(q + 16),
             rdf(q + (v->kind == K_SCREAM_AT9 ? 72 : 56)), *ptr<int8_t>(q + (v->kind == K_SCREAM_AT9 ? 88 : 76)),
             v->kind == K_SCREAM_AT9 ? "config" : "tipo",
             v->kind == K_SCREAM_AT9 ? rd32(q + 92) : (uint32_t)*ptr<int8_t>(q + 79));
    }
    if (arg(ctx, 3)) wr32(arg(ctx, 3), (uint32_t)errors);
    ret(ctx, v ? SCE_OK : NGS_ERROR_INVALID_ARG);
}
VT_HLE(sceNgsVoiceGetParamsOutOfRange) {
    if (arg(ctx, 2)) ptr<char>(arg(ctx, 2))[0] = 0;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceBypassModule) { ret(ctx, SCE_OK); }
VT_HLE(sceNgsVoiceSetModuleCallback) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    const uint32_t module = arg(ctx, 1);
    if (!v || module >= 8) return ret(ctx, NGS_ERROR_INVALID_ARG);
    v->mod_cb[module] = arg(ctx, 2), v->mod_ud[module] = arg(ctx, 3);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceSetFinishedCallback) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    if (!v) return ret(ctx, NGS_ERROR_INVALID_ARG);
    v->fin_cb = arg(ctx, 1), v->fin_ud = arg(ctx, 2);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoicePlay) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    if (!v) return ret(ctx, NGS_ERROR_INVALID_ARG);
    if (!(v->state & ST_ACTIVE)) {
        reset_source(*v);
        v->updates = 0;
    }
    v->state = ST_ACTIVE;
    v->key_off = false;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoicePause) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    if (!v) return ret(ctx, NGS_ERROR_INVALID_ARG);
    if (v->state & ST_ACTIVE) v->state |= ST_PAUSED;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceResume) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    if (!v) return ret(ctx, NGS_ERROR_INVALID_ARG);
    v->state &= ~ST_PAUSED;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceKeyOff) {
    // Sem envelope de liberação: a voz termina no próximo update (com o callback de fim).
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    if (!v) return ret(ctx, NGS_ERROR_INVALID_ARG);
    if (v->state & ST_ACTIVE) v->state |= ST_KEY_OFF | ST_FINALIZE, v->key_off = true;
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceKill) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    if (!v) return ret(ctx, NGS_ERROR_INVALID_ARG);
    v->state = 0, v->key_off = false;
    reset_source(*v);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceGetInfo) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    const uint32_t info = arg(ctx, 1);
    if (!v || !info) return ret(ctx, NGS_ERROR_INVALID_ARG);
    const Rack *r = g_racks.count(v->rack) ? &g_racks[v->rack] : nullptr;
    wr32(info, v->state);
    wr32(info + 4, kModules[v->kind]);
    wr32(info + 8, kInputs[v->kind]);
    wr32(info + 12, kOutputs[v->kind]);
    wr32(info + 16, r ? r->patches_per_output : 1);
    wr32(info + 20, v->updates);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoiceGetStateData) {
    // (voz, módulo, mem, tamanho): estado do player/ATRAC9 (6 int32) ou o grão PCM do master (módulo 1).
    std::lock_guard<std::mutex> g(g_mutex);
    auto v = voice_of(arg(ctx, 0));
    const uint32_t module = arg(ctx, 1), mem = arg(ctx, 2), size = arg(ctx, 3);
    if (!mem) return ret(ctx, NGS_ERROR_INVALID_ARG);
    std::memset(ptr<void>(mem), 0, size);
    if (v && v->kind == K_MASTER && module == 1 && !v->master_pcm.empty()) {
        std::memcpy(ptr<void>(mem), v->master_pcm.data(), (std::min)((size_t)size, v->master_pcm.size() * 2));
    } else if (v && is_source(*v) && module == 0) {
        const int32_t st[6] = {v->byte_pos, v->cur_buffer, v->samples_since_key_on, v->bytes_since_key_on, v->samples_total, v->bytes_total};
        std::memcpy(ptr<void>(mem), st, (std::min)((size_t)size, sizeof st));
    }
    ret(ctx, v ? SCE_OK : NGS_ERROR_INVALID_ARG);
}

// ------------------------------------------------------------------- patches
VT_HLE(sceNgsPatchCreateRouting) {
    // SceNgsPatchSetupInfo {voz fonte, saída, sub-índice, voz destino, entrada}; a matriz começa em zero.
    const uint32_t info = arg(ctx, 0), out = arg(ctx, 1);
    if (!info || !out) return ret(ctx, NGS_ERROR_INVALID_ARG);
    const uint32_t h = zalloc(0x20, "ngs:patch");
    std::memcpy(ptr<void>(h), ptr<void>(info), 0x14);
    Patch p;
    p.src = rd32(info), p.out_index = (int32_t)rd32(info + 4), p.out_sub = (int32_t)rd32(info + 8);
    p.dst = rd32(info + 12), p.in_index = (int32_t)rd32(info + 16);
    {
        std::lock_guard<std::mutex> g(g_mutex);
        g_patches[h] = p;
    }
    wr32(out, h);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsPatchRemoveRouting) {
    std::lock_guard<std::mutex> g(g_mutex);
    g_patches.erase(arg(ctx, 0));
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsPatchGetInfo) {
    // (patch, SceNgsPatchRouteInfo {canais de saída, canais de entrada, matriz}, SceNgsPatchSetupInfo)
    std::lock_guard<std::mutex> g(g_mutex);
    const uint32_t h = arg(ctx, 0), route = arg(ctx, 1), setup = arg(ctx, 2);
    auto it = g_patches.find(h);
    if (it == g_patches.end()) return ret(ctx, NGS_ERROR_INVALID_ARG);
    if (route) {
        auto s = voice_of(it->second.src), d = voice_of(it->second.dst);
        wr32(route, s ? s->channels : 2);
        wr32(route + 4, d ? d->channels : 2);
        std::memcpy(ptr<void>(route + 8), it->second.m, 16);
    }
    if (setup) std::memcpy(ptr<void>(setup), ptr<void>(h), 0x14);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsVoicePatchSetVolumesMatrix) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto it = g_patches.find(arg(ctx, 0));
    if (it == g_patches.end() || !arg(ctx, 1)) return ret(ctx, NGS_ERROR_INVALID_ARG);
    std::memcpy(it->second.m, ptr<void>(arg(ctx, 1)), 16);
    ret(ctx, SCE_OK);
}
VT_HLE(sceNgsAT9GetSectionDetails) {
    // (amostra inicial, número de amostras, config, SceNgsAT9SkipBufferInfo *out): trecho em
    // superquadros inteiros e as amostras a descartar no início e no fim.
    const uint32_t start = arg(ctx, 0), count = arg(ctx, 1), config = arg(ctx, 2), out = arg(ctx, 3);
    if (!out) return ret(ctx, NGS_ERROR_INVALID_ARG);
    std::memset(ptr<void>(out), 0, 0x10);
    void *h = Atrac9GetHandle();
    uint8_t cfg[4];
    std::memcpy(cfg, &config, 4);
    Atrac9CodecInfo ci{};
    if (Atrac9InitDecoder(h, cfg) == 0 && Atrac9GetCodecInfo(h, &ci) == 0) {
        const uint32_t sps = (uint32_t)(ci.frameSamples * ci.framesInSuperframe);
        const uint32_t first = start / sps, last = (start + count + sps - 1) / sps;
        wr32(out, first * ci.superframeSize);
        wr32(out + 4, (last - first) * ci.superframeSize);
        *ptr<int16_t>(out + 8) = (int16_t)(start - first * sps);
        *ptr<int16_t>(out + 10) = (int16_t)(last * sps - (start + count));
    }
    Atrac9ReleaseHandle(h);
    ret(ctx, SCE_OK);
}

// ------------------------------------------------------------------- update: um grão
VT_HLE(sceNgsSystemUpdate) {
    std::unique_lock<std::mutex> lock(g_mutex);
    const int G = g_granularity;
    std::vector<std::shared_ptr<Voice>> voices;
    for (auto &[h, v] : g_voices) {
        v->has_out = false;
        v->in.assign((size_t)G * 2, 0.f);
        voices.push_back(v);
    }
    // 1. Fontes ativas: decodificam um grão (callbacks com o mutex solto).
    for (auto &v : voices) {
        if (!is_source(*v) || !(v->state & ST_ACTIVE) || (v->state & ST_PAUSED)) continue;
        bool finished = v->key_off;
        if (!finished) finished = render_source(ctx, lock, *v);
        v->updates++;
        if (finished) {
            if (v->key_off) v->has_out = false;
            v->state = 0;
            v->key_off = false;
            invoke(ctx, lock, *v, v->fin_cb, v->fin_ud, v->kind == K_SCREAM_AT9 ? kAt9Id : kPlayerId, 0, 0, 0);
        }
    }
    // 2. Entrega das fontes; 3. busses na ordem das dependências (cada um quando todas as suas
    // fontes já saíram), somando as entradas e repassando.
    auto deliver = [&](const Voice &src) {
        if (!src.has_out) return;
        for (auto &[h, p] : g_patches) {
            if (p.src != src.handle || p.out_index != 0) continue;
            auto d = voice_of(p.dst);
            if (d) mix_into(*d, src, p);
        }
    };
    for (auto &v : voices)
        if (is_source(*v)) deliver(*v);
    std::vector<std::shared_ptr<Voice>> busses;
    for (auto &v : voices)
        if (!is_source(*v)) busses.push_back(v);
    std::vector<bool> done(busses.size(), false);
    for (size_t pass = 0; pass <= busses.size(); pass++) {
        bool progress = false;
        for (size_t i = 0; i < busses.size(); i++) {
            if (done[i]) continue;
            bool ready = true;
            for (auto &[h, p] : g_patches) {
                if (p.dst != busses[i]->handle) continue;
                for (size_t j = 0; j < busses.size(); j++)
                    if (!done[j] && busses[j]->handle == p.src) ready = false;
            }
            if (!ready && pass < busses.size()) continue;
            Voice &b = *busses[i];
            done[i] = progress = true;
            b.updates++;
            if (b.kind == K_REVERB) continue; // sem reverb ainda: não devolve nada
            b.out = b.in;
            b.has_out = true;
            if (b.kind == K_MASTER) {
                b.master_pcm.resize((size_t)G * 2);
                static int peak = 0, updates = 0;
                for (int k = 0; k < G * 2; k++) {
                    const float s = b.in[k] * 32768.f;
                    b.master_pcm[k] = (int16_t)(s > 32767.f ? 32767 : s < -32768.f ? -32768 : (int)s);
                    peak = (std::max)(peak, std::abs((int)b.master_pcm[k]));
                }
                if (g_log && ++updates % 500 == 0) {
                    int active = 0;
                    for (auto &v : voices) active += is_source(*v) && (v->state & ST_ACTIVE);
                    logf("vitart: NGS: %d updates, %d vozes fonte ativas, pico do master %d; HE-ADPCM %llu quadros, %llu com coeficiente desconhecido",
                         updates, active, peak, (unsigned long long)g_hevag_frames, (unsigned long long)g_hevag_unknown);
                    peak = 0;
                }
            }
            deliver(b);
        }
        if (!progress) break;
    }
    auto rel = std::move(g_release_cbs);
    g_release_cbs.clear();
    lock.unlock();
    for (auto &[cb, rack] : rel) call_guest(ctx, cb, {rack});
    ret(ctx, SCE_OK);
}

} // namespace vt
