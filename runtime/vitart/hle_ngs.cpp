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

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
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

// Reverb estéreo (rede de Schroeder/Moorer: 8 filtros comb com amortecimento + 4 allpass por canal,
// afinações do Freeverb escaladas para a taxa do sistema), guiado pelos parâmetros I3DL2 do NGS:
// tempo de decaimento, razão HF, nível de sala e de reverb (mB), atraso das reflexões e do reverb.
struct Reverb {
    static constexpr int kCombs = 8, kAll = 4;
    std::vector<float> comb[2][kCombs], ap[2][kAll], pre;
    size_t ci[2][kCombs] = {}, ai[2][kAll] = {}, pw = 0;
    float damp_state[2][kCombs] = {}, hf_state[2] = {};
    explicit Reverb(int rate) {
        static const int combs[kCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int alls[kAll] = {556, 441, 341, 225};
        const double k = rate / 44100.0;
        for (int c = 0; c < 2; c++) {
            for (int i = 0; i < kCombs; i++) comb[c][i].assign((size_t)((combs[i] + 23 * c) * k) + 1, 0.f);
            for (int i = 0; i < kAll; i++) ap[c][i].assign((size_t)((alls[i] + 23 * c) * k) + 1, 0.f);
        }
        pre.assign((size_t)(rate * 0.5) * 2 + 2, 0.f); // até 0,5 s de pré-atraso, estéreo intercalado
    }
};

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
    int32_t ps2_file = -1, ps2_sf = -1; // último superquadro trocado pelo áudio do PS2
    int32_t src_ch = 2; // canais da fonte: as amostras geradas contam quadros entregues x canais (por grão)
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
    // Envelope (módulo 1 das vozes Scream): tempo desde o key-on e a soltura depois do key-off.
    double env_ms = 0, rel_ms = 0;
    float env_level = 1.f, rel_from = 1.f;
    bool releasing = false, env_done = false;
    // Busses de efeito.
    std::unique_ptr<Reverb> reverb;
    float comp_env[2] = {};
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
    // Matriz aplicada no grão anterior: a nova é alcançada em rampa linear dentro do grão (passo seco a
    // cada mudança de volume/pan dava um estalo). prev_valid = false: a fonte acabou de começar (rampa de 0).
    float prev[2][2] = {};
    bool prev_valid = false;
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
uint64_t g_comp_samples = 0, g_comp_reduced = 0, g_clip_samples = 0; // diagnóstico do volume do mix
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

// ------------------------------------------------- áudio do PS2 no lugar do ATRAC9 (docs/audio-ps2.md)
// VITART_PS2_AUDIO=<iso do GoW1 de PS2>: cada superquadro ATRAC9 de stream é procurado pelo
// crc32<<32|adler32 no índice <dados>/ps2audio.idx (tools/ps2_audio_index.py); achando, o trecho
// correspondente do VPK/VAG do PS2 (lido da ISO, PS-ADPCM) sai no lugar do ATRAC9. Bytes consumidos e
// amostras geradas não mudam, então o jogo não percebe a troca.
namespace ps2a {
struct File { uint32_t lba, size, vita_samples, sps, ch, rsv; };
struct Key { uint64_t key; uint32_t file, sf; };
struct Pcm { uint32_t rate = 0, ch = 0; size_t frames = 0; std::vector<int16_t> s; };
static std::vector<File> g_files;
static std::vector<Key> g_keys;
static FILE *g_iso = nullptr;
static std::map<uint32_t, std::shared_ptr<Pcm>> g_cache;
static std::deque<uint32_t> g_lru;

static bool on() {
    static const bool ok = [] {
        const char *iso = getenv("VITART_PS2_AUDIO");
        if (!iso || !*iso) return false;
        g_iso = fopen(iso, "rb");
        FILE *f = fopen((data_path() + "/ps2audio.idx").c_str(), "rb");
        uint32_t h[3] = {};
        if (!g_iso || !f || fread(h, 4, 3, f) != 3 || h[0] != 0x32535047u) {
            logf("vitart: áudio do PS2: ISO ou índice (ps2audio.idx) indisponível");
            if (f) fclose(f);
            return false;
        }
        g_files.resize(h[1]);
        g_keys.resize(h[2]);
        fread(g_files.data(), sizeof(File), g_files.size(), f);
        fread(g_keys.data(), sizeof(Key), g_keys.size(), f);
        fclose(f);
        logf("vitart: áudio do PS2 ligado: %zu arquivos, %zu superquadros", g_files.size(), g_keys.size());
        return true;
    }();
    return ok;
}

static uint32_t crc32(const uint8_t *p, size_t n) {
    static uint32_t t[256];
    static const bool init = [] {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return true;
    }();
    (void)init;
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = t[(c ^ p[i]) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static uint32_t adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; i++) a = (a + p[i]) % 65521, b = (b + a) % 65521;
    return b << 16 | a;
}

static void adpcm(const uint8_t *d, size_t n, std::vector<int16_t> &out) {
    static const int F[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
    int s1 = 0, s2 = 0;
    for (size_t o = 0; o + 16 <= n; o += 16) {
        const int shift = d[o] & 15, filt = ((d[o] >> 4) & 7) > 4 ? 0 : (d[o] >> 4) & 7;
        for (int k = 0; k < 28; k++) {
            int nib = (d[o + 2 + k / 2] >> (4 * (k & 1))) & 15;
            if (nib >= 8) nib -= 16;
            int v = (nib << 12) >> shift;
            v += (s1 * F[filt][0] + s2 * F[filt][1] + 32) >> 6;
            v = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
            out.push_back((int16_t)v);
            s2 = s1, s1 = v;
        }
    }
}

static std::shared_ptr<Pcm> pcm_of(uint32_t fi) {
    auto it = g_cache.find(fi);
    if (it != g_cache.end()) return it->second;
    const File &f = g_files[fi];
    std::vector<uint8_t> d(f.size);
    _fseeki64(g_iso, (long long)f.lba * 2048, SEEK_SET);
    if (fread(d.data(), 1, d.size(), g_iso) != d.size() || d.size() < 0x30) return nullptr;
    auto pcm = std::make_shared<Pcm>();
    std::vector<std::vector<int16_t>> chans;
    auto le = [&](size_t o) { return (uint32_t)d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24; };
    auto be = [&](size_t o) { return (uint32_t)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | (uint32_t)d[o + 3]; };
    if (!std::memcmp(d.data(), " KPV", 4)) {
        const uint32_t size = le(4), start = le(8), inter = le(12), ch = le(20);
        pcm->rate = le(16), pcm->ch = ch ? ch : 1;
        const uint32_t il = inter / pcm->ch;
        std::vector<std::vector<uint8_t>> raw(pcm->ch);
        for (size_t o = start; il && o < start + (size_t)size * pcm->ch && o < d.size(); o += (size_t)il * pcm->ch)
            for (uint32_t c = 0; c < pcm->ch; c++) {
                const size_t a = o + (size_t)c * il, e = (std::min)(a + il, d.size());
                if (a < e) raw[c].insert(raw[c].end(), d.begin() + a, d.begin() + e);
            }
        chans.resize(pcm->ch);
        for (uint32_t c = 0; c < pcm->ch; c++) adpcm(raw[c].data(), raw[c].size(), chans[c]);
    } else if (!std::memcmp(d.data(), "VAGp", 4)) {
        pcm->rate = be(16), pcm->ch = 1;
        chans.resize(1);
        adpcm(d.data() + 0x30, (std::min)((size_t)be(12), d.size() - 0x30), chans[0]);
    } else {
        return nullptr;
    }
    pcm->frames = SIZE_MAX;
    for (auto &c : chans) pcm->frames = (std::min)(pcm->frames, c.size());
    pcm->s.resize(pcm->frames * pcm->ch);
    for (size_t i = 0; i < pcm->frames; i++)
        for (uint32_t c = 0; c < pcm->ch; c++) pcm->s[i * pcm->ch + c] = chans[c][i];
    g_cache[fi] = pcm;
    g_lru.push_back(fi);
    while (g_lru.size() > 8) g_cache.erase(g_lru.front()), g_lru.pop_front();
    return pcm;
}

// Troca o PCM de um superquadro (sf: amostras intercaladas com `ch` canais) pelo do PS2. Devolve false
// se o superquadro não está no índice.
static bool replace(Voice &v, const uint8_t *bytes, size_t n, std::vector<int16_t> &sf, int ch) {
    const uint64_t key = (uint64_t)crc32(bytes, n) << 32 | adler32(bytes, n);
    auto lo = std::lower_bound(g_keys.begin(), g_keys.end(), key, [](const Key &k, uint64_t x) { return k.key < x; });
    if (lo == g_keys.end() || lo->key != key) return false;
    const Key *pick = &*lo;
    for (auto it = lo; it != g_keys.end() && it->key == key; ++it) // silêncio repetido: continuidade
        if ((int32_t)it->file == v.ps2_file && (int32_t)it->sf == v.ps2_sf + 1) { pick = &*it; break; }
    const File &f = g_files[pick->file];
    auto pcm = pcm_of(pick->file);
    if (!pcm || !pcm->frames || !f.vita_samples) return false;
    if ((int32_t)pick->file != v.ps2_file)
        logf("vitart: áudio do PS2: arquivo %u (setor %u, %u Hz, %u canais) a partir do superquadro %u",
             pick->file, f.lba, pcm->rate, pcm->ch, pick->sf);
    v.ps2_file = (int32_t)pick->file, v.ps2_sf = (int32_t)pick->sf;
    const size_t frames = sf.size() / ch;
    const double scale = (double)pcm->frames / f.vita_samples;
    static const bool diag = getenv("VITART_PS2_DIAG") != nullptr;
    std::vector<int16_t> orig;
    if (diag) orig = sf;
    for (size_t j = 0; j < frames; j++) {
        // 256 = atraso do codificador ATRAC9 (fact[2] dos .at9 do jogo): a amostra 0 do Vita é a 256 do stream.
        const double p = (std::max)(0.0, ((double)pick->sf * f.sps + j - 256.0) * scale);
        const size_t i0 = (std::min)((size_t)p, pcm->frames - 1), i1 = (std::min)(i0 + 1, pcm->frames - 1);
        const float t = (float)(p - (double)(size_t)p);
        float smp[2];
        for (uint32_t c = 0; c < 2; c++) {
            const uint32_t pc = (std::min)(c, pcm->ch - 1);
            smp[c] = pcm->s[i0 * pcm->ch + pc] * (1.f - t) + pcm->s[i1 * pcm->ch + pc] * t;
        }
        if (ch == 1) sf[j] = (int16_t)(pcm->ch == 1 ? smp[0] : 0.5f * (smp[0] + smp[1]));
        else for (int c = 0; c < ch; c++) sf[j * ch + c] = (int16_t)smp[c < 2 ? c : 1];
    }
    if (diag) {
        // Correlação normalizada ATRAC9 x PS2 por arquivo (alinhamento no tempo e nos canais).
        static std::map<uint32_t, std::array<double, 4>> acc; // sxy, sxx, syy, superquadros
        auto &a = acc[pick->file];
        for (size_t i = 0; i < sf.size(); i++) a[0] += (double)orig[i] * sf[i], a[1] += (double)orig[i] * orig[i], a[2] += (double)sf[i] * sf[i];
        if (++a[3] == 200)
            logf("vitart: PS2 DIAG arquivo %u: correlação %.3f, nível PS2/Vita %.2f", pick->file,
                 a[0] / std::sqrt(a[1] * a[2] + 1e-9), std::sqrt(a[2] / (a[1] + 1e-9)));
    }
    return true;
}
} // namespace ps2a
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
            v.src_ch = ch;
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
        v.src_ch = ch;
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
    const uint8_t *sf_bytes = in;
    for (int f = 0; f < ci.framesInSuperframe; f++) {
        int used = 0;
        if (Atrac9Decode(v.at9, in, pcm.data(), &used) != 0) {
            invoke(ctx, lock, v, v.mod_cb[0], v.mod_ud[0], kAt9Id, 3 /* erro de decodificação */, (uint32_t)v.byte_pos, b.ptr);
            break;
        }
        in += used;
        sf.insert(sf.end(), pcm.begin(), pcm.end());
    }
    if (ps2a::on() && !ps2a::replace(v, sf_bytes, (size_t)ci.superframeSize, sf, ci.channels)) v.ps2_file = -1;
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
    v.src_ch = ci.channels;
    return true;
}

// Envelope do módulo 1 (SceNgsEnvelopeParams, id 0x01015CE3): até 4 pontos {ms até o próximo, amplitude,
// curva}, tempo de soltura (+56) e número de pontos (+60). Curva "curved" = quadrática.
constexpr uint32_t kEnvelopeId = 0x01015CE3;
bool has_envelope(const Voice &v) { return v.params[1] && rd32(v.params[1]) == kEnvelopeId; }
uint32_t release_ms(const Voice &v) { return has_envelope(v) ? rd32(v.params[1] + 56) : 0; }
float envelope_gain(Voice &v, double dt_ms) {
    if (!has_envelope(v)) return 1.f;
    const uint32_t q = v.params[1];
    if (v.releasing) {
        const double rel = (double)rd32(q + 56);
        v.rel_ms += dt_ms;
        if (rel <= 0 || v.rel_ms >= rel) {
            v.env_done = true;
            return 0.f;
        }
        float t = (float)(1.0 - v.rel_ms / rel);
        if (rd32(q + 8 + 8) == 1) t *= t; // curva do primeiro ponto vale para a soltura
        return v.rel_from * t;
    }
    const uint32_t np = std::min(rd32(q + 60), 4u);
    double t = v.env_ms;
    v.env_ms += dt_ms;
    float level = np ? rdf(q + 8 + 4) : 1.f;
    for (uint32_t i = 0; i + 1 < np; i++) {
        const double d = (double)rd32(q + 8 + 12 * i);
        const float a = rdf(q + 8 + 12 * i + 4), b = rdf(q + 8 + 12 * (i + 1) + 4);
        if (t < d) {
            float f = d > 0 ? (float)(t / d) : 1.f;
            if (rd32(q + 8 + 12 * i + 8) == 1) f = b > a ? f * (2.f - f) : f * f; // curva: sobe rápido, cai devagar
            level = a + (b - a) * f;
            break;
        }
        t -= d;
        level = b;
    }
    v.env_level = level;
    return level;
}

// Catmull-Rom entre p1 e p2 (amostras vizinhas p0 e p3).
inline float cubic(float p0, float p1, float p2, float p3, float t) {
    return p1 + 0.5f * t * (p2 - p0 + t * (2.f * p0 - 5.f * p1 + 4.f * p2 - p3 + t * (3.f * (p1 - p2) + p3 - p0)));
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
        while (!v.src_done && avail() < idx + 3) {
            if (!decode_more(ctx, lock, v)) v.src_done = true;
        }
        if (avail() < idx + 1) return true; // acabou o PCM
        const float t = (float)(v.frac - (double)idx);
        const size_t at = v.pcm_read + idx;
        const float *a = &v.pcm[at * 2];
        const float *b2 = avail() > idx + 1 ? a + 2 : a;
        const float *a0 = at > 0 ? a - 2 : a;
        const float *b3 = avail() > idx + 2 ? a + 4 : b2;
        const float g = envelope_gain(v, 1000.0 / g_rate);
        if (t == 0.f) {
            v.out[2 * i] = a[0] * g, v.out[2 * i + 1] = a[1] * g;
        } else {
            v.out[2 * i] = cubic(a0[0], a[0], b2[0], b3[0], t) * g;
            v.out[2 * i + 1] = cubic(a0[1], a[1], b2[1], b3[1], t) * g;
        }
        v.frac += rate;
    }
    const size_t used = (std::min)((size_t)v.frac, avail());
    v.pcm_read += used;
    // Amostras geradas avançam por grão, como no NGS real: contar na decodificação (em rajadas de
    // 1024+ quadros) deixava o contador parado em metade das atualizações, e o leitor de stream do
    // jogo (sub_811333f2) devolvia 0, zerava a posição anterior e na seguinte limitava o avanço a
    // 480000 ("locationDelta is too big"), adiantando o stream.
    v.samples_since_key_on += (int32_t)used * v.src_ch, v.samples_total += (int32_t)used * v.src_ch;
    v.frac -= (double)used;
    if (v.pcm_read > 8192) { // compacta (guarda uma amostra para a interpolação cúbica)
        v.pcm.erase(v.pcm.begin(), v.pcm.begin() + (ptrdiff_t)(v.pcm_read - 1) * 2);
        v.pcm_read = 1;
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
    v.env_ms = v.rel_ms = 0, v.env_level = v.rel_from = 1.f, v.releasing = v.env_done = false;
    std::memset(v.hist, 0, sizeof v.hist);
    if (v.at9) Atrac9ReleaseHandle(v.at9), v.at9 = nullptr;
    v.at9_config = 0;
}

float mb_gain(float mb) { return std::pow(10.f, mb / 2000.f); } // milibéis -> ganho linear

// Reverb do bus (módulo 1, SceNgsReverbParams id 0x01015CE7/0x01025CE7): fRoom +8, fRoomHF +12,
// fDecayTime +16, fDecayHFRatio +20, fReflections +24, fReflectionsDelay +28, fReverb +32,
// fReverbDelay +36, fDryMB +72. Entrada em b.in, saída em b.out.
void run_reverb(Voice &b) {
    const int G = g_granularity;
    const uint32_t q = b.params[1];
    b.out.assign((size_t)G * 2, 0.f);
    const uint32_t id = q ? rd32(q) : 0;
    if (id != 0x01015CE7u && id != 0x01025CE7u) return;
    if (!b.reverb) b.reverb = std::make_unique<Reverb>(g_rate);
    Reverb &r = *b.reverb;
    const float room = rdf(q + 8), room_hf = rdf(q + 12), decay = std::max(0.1f, rdf(q + 16));
    const float hf_ratio = std::clamp(rdf(q + 20), 0.1f, 2.f), refl = rdf(q + 24), refl_delay = std::clamp(rdf(q + 28), 0.f, 0.3f);
    const float rev = rdf(q + 32), rev_delay = std::clamp(rdf(q + 36), 0.f, 0.1f), dry = mb_gain(std::clamp(rdf(q + 72), -10000.f, 0.f));
    const float late = mb_gain(std::clamp(room + rev, -10000.f, 2000.f)), early = mb_gain(std::clamp(room + refl, -10000.f, 1000.f));
    // Amortecimento: razão HF < 1 escurece a cauda; fRoomHF filtra a entrada (passa-baixas de 1 polo).
    const float damp = std::clamp(1.f - hf_ratio * 0.8f, 0.f, 0.7f);
    const float hf_in = std::clamp(mb_gain(std::clamp(room_hf, -10000.f, 0.f)), 0.05f, 1.f);
    const size_t prelen = r.pre.size() / 2;
    const size_t d_early = std::min(prelen - 1, (size_t)(refl_delay * g_rate) + 1);
    const size_t d_late = std::min(prelen - 1, (size_t)((refl_delay + rev_delay) * g_rate) + 1);
    for (int i = 0; i < G; i++) {
        for (int c = 0; c < 2; c++) {
            float x = b.in[2 * i + c];
            r.hf_state[c] += hf_in * (x - r.hf_state[c]);
            r.pre[2 * r.pw + c] = r.hf_state[c];
        }
        const size_t pe = (r.pw + prelen - d_early) % prelen, pl = (r.pw + prelen - d_late) % prelen;
        for (int c = 0; c < 2; c++) {
            const float in = r.pre[2 * pl + c] * 0.015f;
            float acc = 0;
            for (int k = 0; k < Reverb::kCombs; k++) {
                std::vector<float> &buf = r.comb[c][k];
                size_t &ix = r.ci[c][k];
                const float y = buf[ix];
                const float fb = std::pow(10.f, -3.f * (float)buf.size() / (decay * g_rate)); // T60
                r.damp_state[c][k] = y * (1.f - damp) + r.damp_state[c][k] * damp;
                buf[ix] = in + r.damp_state[c][k] * fb;
                ix = (ix + 1) % buf.size();
                acc += y;
            }
            for (int k = 0; k < Reverb::kAll; k++) {
                std::vector<float> &buf = r.ap[c][k];
                size_t &ix = r.ai[c][k];
                const float bo = buf[ix];
                buf[ix] = acc + bo * 0.5f;
                acc = bo - acc;
                ix = (ix + 1) % buf.size();
            }
            b.out[2 * i + c] = acc * 3.f * late + r.pre[2 * pe + c] * early * 0.5f + b.in[2 * i + c] * dry;
        }
        r.pw = (r.pw + 1) % prelen;
    }
}

// Compressor do bus (módulo 1, SceNgsCompressorParams id 0x01015CE1/0x01025CE1): razão +8, limiar dB
// +12, ataque ms +16, soltura ms +20, ganho de compensação dB +24, ligação estéreo +28, modo pico +32.
// Sem parâmetros, repassa.
void run_compressor(Voice &b) {
    b.out = b.in;
    const uint32_t q = b.params[1];
    const uint32_t id = q ? rd32(q) : 0;
    if (g_log) {
        static int n = 0;
        if (n++ % 3000 == 0) {
            float pk = 0;
            for (float x : b.in) pk = std::max(pk, std::fabs(x));
            logf("vitart: NGS compressor id 0x%08x: %.3g %.3g %.3g %.3g %.3g %u %u %.3g; pico da entrada %.3f", id, rdf(q + 8),
                 rdf(q + 12), rdf(q + 16), rdf(q + 20), rdf(q + 24), rd32(q + 28), rd32(q + 32), rdf(q + 36), pk);
        }
    }
    // Sem parâmetros (o GoW nunca os define) vale o padrão: limitador suave que segura a soma das vozes
    // antes do master (−6 dB, 4:1, ataque 5 ms, soltura 100 ms, estéreo ligado). Sem isso o master satura.
    const bool set = id == 0x01015CE1u || id == 0x01025CE1u;
    const float ratio = set ? std::max(1.f, rdf(q + 8)) : 4.f, thr = set ? rdf(q + 12) : -6.f;
    const float att = std::exp(-1.f / ((set ? std::max(0.1f, rdf(q + 16)) : 5.f) * 0.001f * g_rate));
    const float rel = std::exp(-1.f / ((set ? std::max(1.f, rdf(q + 20)) : 100.f) * 0.001f * g_rate));
    const float makeup = set ? std::pow(10.f, rdf(q + 24) / 20.f) : 1.f;
    const bool link = set ? rd32(q + 28) != 0 : true;
    const int G = g_granularity;
    for (int i = 0; i < G; i++) {
        float lv[2] = {std::fabs(b.in[2 * i]), std::fabs(b.in[2 * i + 1])};
        if (link) lv[0] = lv[1] = std::max(lv[0], lv[1]);
        for (int c = 0; c < 2; c++) {
            float &e = b.comp_env[c];
            e = lv[c] > e ? att * e + (1 - att) * lv[c] : rel * e + (1 - rel) * lv[c];
            const float db = 20.f * std::log10(std::max(e, 1e-6f));
            const float over = db - thr;
            const float gr = over > 0 ? over * (1.f - 1.f / ratio) : 0.f;
            g_comp_samples++, g_comp_reduced += gr > 1.f;
            b.out[2 * i + c] = b.in[2 * i + c] * std::pow(10.f, -gr / 20.f) * makeup;
        }
    }
}

void mix_into(Voice &dst, const Voice &src, Patch &p) {
    const int G = g_granularity;
    if (dst.in.size() != (size_t)G * 2) dst.in.assign((size_t)G * 2, 0.f);
    float m[2][2] = {{p.m[0][0], p.m[0][1]}, {p.m[1][0], p.m[1][1]}};
    if (src.channels == 1) m[1][0] = m[1][1] = 0.f;
    if (dst.channels == 1) m[0][1] = m[1][1] = 0.f;
    float a[2][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) a[i][j] = p.prev_valid ? p.prev[i][j] : 0.f;
    const float inv = 1.f / G;
    for (int k = 0; k < G; k++) {
        const float t = (k + 1) * inv, l = src.out[2 * k], r = src.out[2 * k + 1];
        const float m00 = a[0][0] + (m[0][0] - a[0][0]) * t, m01 = a[0][1] + (m[0][1] - a[0][1]) * t;
        const float m10 = a[1][0] + (m[1][0] - a[1][0]) * t, m11 = a[1][1] + (m[1][1] - a[1][1]) * t;
        dst.in[2 * k] += l * m00 + r * m10;
        dst.in[2 * k + 1] += l * m01 + r * m11;
    }
    std::memcpy(p.prev, m, sizeof m);
    p.prev_valid = true;
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
        if (g_log && module < 8) {
            static std::map<uint64_t, int> seen;
            const uint32_t id = rd32(p + 8);
            int &n = seen[(uint64_t)v->kind << 40 | (uint64_t)module << 32 | id];
            if (n++ < 3 && module > 0) {
                char buf[512];
                int o = std::snprintf(buf, sizeof buf, "vitart: NGS parâmetros tipo %d módulo %u id 0x%08x (%u bytes):", (int)v->kind,
                                      module, id, size);
                for (uint32_t k = 16; k < size && k < 8 + 4 * 24 && o < 480; k += 4) {
                    const float f = rdf(p + k);
                    if (f == f && std::fabs(f) < 1e6f && std::fabs(f) > 1e-6f)
                        o += std::snprintf(buf + o, sizeof buf - o, " %.3g", (double)f);
                    else
                        o += std::snprintf(buf + o, sizeof buf - o, " #%x", rd32(p + k));
                }
                logf("%s", buf);
            }
        }
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
        for (auto &[h, p] : g_patches)
            if (p.src == v->handle) p.prev_valid = false; // entra em rampa a partir do silêncio
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
    // A voz entra na soltura do envelope no próximo update e termina ao fim dela (callback de fim).
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
        if (g_log) {
            static std::map<uint32_t, int> seen;
            auto s = voice_of(p.src), d = voice_of(p.dst);
            const uint32_t k = (s ? s->kind : 9) << 16 | p.out_index << 8 | (d ? d->kind : 9) << 4 | p.in_index;
            if (seen[k]++ == 0)
                logf("vitart: NGS patch: tipo %d saída %d -> tipo %d entrada %d", s ? (int)s->kind : -1, p.out_index,
                     d ? (int)d->kind : -1, p.in_index);
        }
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
    if (g_log) {
        static int n = 0;
        auto s = voice_of(it->second.src);
        if (n < 12 && s && it->second.out_index == 1)
            n++, logf("vitart: NGS matriz saída 1 (tipo %d): %.3f %.3f %.3f %.3f", (int)s->kind, it->second.m[0][0],
                      it->second.m[0][1], it->second.m[1][0], it->second.m[1][1]);
    }
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
        bool finished = false, cut = false;
        if (v->key_off && !v->releasing) {
            // Key-off: a soltura do envelope esmaece a voz; sem soltura, ela para já.
            v->releasing = true, v->rel_ms = 0, v->rel_from = v->env_level;
            if (release_ms(*v) == 0) cut = true;
        }
        finished = render_source(ctx, lock, *v) || v->env_done || cut;
        if (cut && v->has_out) // corte sem soltura: esmaece ao longo deste grão em vez de parar numa amostra
            for (int k = 0; k < G; k++) v->out[2 * k] *= 1.f - (k + 1.f) / G, v->out[2 * k + 1] *= 1.f - (k + 1.f) / G;
        v->updates++;
        if (finished) {
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
            // Saída 0 = seco; nas fontes, a saída 1 é o envio (para o reverb) com o mesmo sinal.
            if (p.src != src.handle || p.out_index > (is_source(src) ? 1 : 0)) continue;
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
            if (b.kind == K_REVERB) {
                if (g_log) {
                    static int n = 0;
                    if (n++ % 2000 == 0) {
                        char buf[400];
                        int o = std::snprintf(buf, sizeof buf, "vitart: NGS reverb id 0x%08x:", rd32(b.params[1]));
                        for (uint32_t k = 8; k < 8 + 4 * 18 && o < 380; k += 4)
                            o += std::snprintf(buf + o, sizeof buf - o, " %.3g", (double)rdf(b.params[1] + k));
                        float pk = 0;
                        for (float x : b.in) pk = std::max(pk, std::fabs(x));
                        logf("%s; pico da entrada %.3f", buf, pk);
                    }
                }
                run_reverb(b);
                b.has_out = true;
                deliver(b);
                continue;
            }
            if (b.kind == K_COMPRESSOR || b.kind == K_SIDECHAIN)
                run_compressor(b);
            else
                b.out = b.in;
            b.has_out = true;
            if (b.kind == K_MASTER) {
                b.master_pcm.resize((size_t)G * 2);
                static int peak = 0, updates = 0;
                for (int k = 0; k < G * 2; k++) {
                    // Saturação suave acima de 0,9 (no lugar do corte seco em 1,0).
                    float x = b.in[k];
                    const float ax = std::fabs(x);
                    if (ax > 0.9f) x = std::copysign(0.9f + 0.1f * std::tanh((ax - 0.9f) / 0.1f), x), g_clip_samples++;
                    const float s = x * 32767.f;
                    b.master_pcm[k] = (int16_t)(s > 32767.f ? 32767 : s < -32768.f ? -32768 : (int)s);
                    peak = (std::max)(peak, std::abs((int)b.master_pcm[k]));
                }
                if (g_log && ++updates % 500 == 0) {
                    int active = 0;
                    for (auto &v : voices) active += is_source(*v) && (v->state & ST_ACTIVE);
                    logf("vitart: NGS: %d updates, %d vozes fonte ativas, pico do master %d; HE-ADPCM %llu quadros, %llu com coeficiente desconhecido; compressor reduzindo >1 dB em %.1f%% das amostras, saturação suave em %.3f%%",
                         updates, active, peak, (unsigned long long)g_hevag_frames, (unsigned long long)g_hevag_unknown,
                         g_comp_samples ? 100.0 * g_comp_reduced / g_comp_samples : 0.0, 100.0 * g_clip_samples / (500.0 * G * 2));
                    g_comp_samples = g_comp_reduced = g_clip_samples = 0;
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
