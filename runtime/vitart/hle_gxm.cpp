// HLE do sceGxm para o vitart: camada de ESTADO. Objetos (contexto, shader patcher, programas,
// render targets, sync objects) existem como blocos na memória do guest (para o jogo guardar
// ponteiros válidos) com o estado no host. Ainda não desenha: grava o que cada cena pede e
// conclui as notificações na hora, para o jogo seguir o laço. O renderizador Vulkan entra aqui.
#include "vitart.h"
#include "gxm_tex.h"
#include "gxm_vk.h"

#include <array>
#include <cmath>
#include <cstring>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <thread>
#include <unordered_map>

namespace vt {

constexpr uint32_t SCE_OK = 0;
constexpr uint32_t GXM_ERROR_INVALID_POINTER = 0x805B0009u;
constexpr uint32_t GXM_ERROR_INVALID_VALUE = 0x805B0004u;

// ------------------------------------------------------------------ objetos
struct VertexProgram {
    uint32_t program = 0; // SceGxmProgram* no guest
    std::vector<uint8_t> attributes, streams;
    uint32_t attr_count = 0, stream_count = 0;
};
struct FragmentProgram {
    uint32_t program = 0, vertex_program = 0, output_format = 0, msaa = 0;
    std::vector<uint8_t> blend;
};
struct DrawCall {
    uint32_t prim, index_type, index_data, index_count, vprog, fprog;
};
struct Scene {
    uint32_t render_target = 0, color_surface = 0, depth_surface = 0;
    std::vector<DrawCall> draws;
};
struct Context {
    uint32_t vertex_ring = 0, vertex_ring_size = 0, vertex_ring_pos = 0;
    uint32_t fragment_ring = 0, fragment_ring_size = 0, fragment_ring_pos = 0;
    uint32_t vprog = 0, fprog = 0;
    uint32_t streams[16] = {};
    uint32_t textures[16] = {};
    uint32_t vuniform = 0, funiform = 0; // último default uniform buffer reservado
    uint32_t viewport_r[6] = {}, viewport_s[6] = {}; // argumentos crus (ABI de float ainda a confirmar)
    uint32_t depth_func = 7, depth_write = 1, cull = 0, two_sided = 0;
    bool in_scene = false;
    Scene scene;
};

static std::mutex g_gxm_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<Context>> g_contexts;
static std::unordered_map<uint32_t, std::shared_ptr<VertexProgram>> g_vprogs;
static std::unordered_map<uint32_t, std::shared_ptr<FragmentProgram>> g_fprogs;
static std::unordered_map<uint32_t, uint32_t> g_program_ids; // id -> SceGxmProgram*
static std::unordered_map<std::string, uint32_t> g_vprog_cache, g_fprog_cache;
static std::unordered_map<uint32_t, uint32_t> g_id_of_program;

static uint32_t new_object(uint32_t size, const char *what) {
    const uint32_t a = galloc(Pool::kernel, size, 16, what);
    if (!a)
        fatal("sem memória para objeto GXM %s", what);
    std::memset(ptr<uint8_t>(a), 0, size);
    return a;
}

static std::shared_ptr<Context> context_of(uint32_t h) {
    const std::lock_guard<std::mutex> lock(g_gxm_mutex);
    auto it = g_contexts.find(h);
    return it == g_contexts.end() ? nullptr : it->second;
}

// ------------------------------------------------------- estatística de quadros
static std::atomic<uint64_t> g_scenes{ 0 }, g_draws{ 0 }, g_frames{ 0 };
static auto g_last_report = std::chrono::steady_clock::now();
static uint64_t g_last_frames = 0;

static void report_frame() {
    const uint64_t f = ++g_frames;
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::chrono::duration<double>(now - g_last_report).count();
    if (dt >= 5.0) {
        logf("vitart: gxm: %llu quadros (%.1f/s), %llu cenas, %llu desenhos no total", (unsigned long long)f,
            (f - g_last_frames) / dt, (unsigned long long)g_scenes.load(), (unsigned long long)g_draws.load());
        g_last_report = now;
        g_last_frames = f;
    }
}

// ============================================================ captura (diagnóstico)
// VITART_DUMP_FRAME=N e VITART_DUMP_DIR=dir: no N-ésimo quadro enviado, grava os descritores de
// textura usados nos desenhos (textures.txt) e a memória do jogo, para decodificar fora.
static uint32_t g_dump_frame = 0;
static std::string g_dump_dir;
static std::atomic<uint32_t> g_submitted{ 0 };
static std::map<std::string, uint32_t> g_dump_textures; // descritor de 16 bytes -> desenhos
static std::mutex g_dump_mutex;
static FILE *g_draws_json = nullptr, *g_draws_bin = nullptr;

static void dump_init() {
    if (const char *f = std::getenv("VITART_DUMP_FRAME")) g_dump_frame = (uint32_t)std::atoi(f);
    if (const char *d = std::getenv("VITART_DUMP_DIR")) g_dump_dir = d;
    if (g_dump_frame && g_dump_dir.empty()) g_dump_dir = "dump";
}
static bool dumping() { return g_dump_frame && g_submitted.load() + 1 == g_dump_frame; }
static void dump_texture(uint32_t t) {
    if (!t || !dumping()) return;
    const std::lock_guard<std::mutex> lock(g_dump_mutex);
    g_dump_textures[std::string(ptr<char>(t), 16)]++;
}
static void dump_write(uint32_t fb) {
    std::filesystem::create_directories(g_dump_dir);
    {
        std::ofstream f(g_dump_dir + "/textures.txt");
        const std::lock_guard<std::mutex> lock(g_dump_mutex);
        for (auto &[d, n] : g_dump_textures) {
            const uint32_t *w = reinterpret_cast<const uint32_t *>(d.data());
            char line[96];
            std::snprintf(line, sizeof line, "%08x %08x %08x %08x %u\n", w[0], w[1], w[2], w[3], n);
            f << line;
        }
    }
    dump_regions(g_dump_dir, 0x81000000u, 0xB0000000u);
    {
        const std::lock_guard<std::mutex> lock(g_dump_mutex);
        if (g_draws_json) { fclose(g_draws_json); fclose(g_draws_bin); g_draws_json = g_draws_bin = nullptr; }
    }
    logf("vitart: captura do quadro %u em %s: %zu texturas, framebuffer 0x%08x", g_dump_frame, g_dump_dir.c_str(),
        g_dump_textures.size(), fb);
}

// Captura completa dos desenhos do quadro (draws.jsonl + draws.bin) para rasterizar fora.
static uint64_t g_draws_bin_pos = 0;
static uint32_t blob(const void *p, uint32_t n) {
    const uint32_t off = (uint32_t)g_draws_bin_pos;
    fwrite(p, 1, n, g_draws_bin);
    g_draws_bin_pos += n;
    return off;
}
static void dump_draw(Context &c, uint32_t prim, uint32_t itype, uint32_t idata, uint32_t icount) {
    std::shared_ptr<VertexProgram> vp;
    std::shared_ptr<FragmentProgram> fp;
    {
        const std::lock_guard<std::mutex> lock(g_gxm_mutex);
        auto v = g_vprogs.find(c.vprog);
        auto f = g_fprogs.find(c.fprog);
        if (v != g_vprogs.end()) vp = v->second;
        if (f != g_fprogs.end()) fp = f->second;
    }
    const std::lock_guard<std::mutex> lock(g_dump_mutex);
    if (!g_draws_json) {
        std::filesystem::create_directories(g_dump_dir);
        g_draws_json = fopen((g_dump_dir + "/draws.jsonl").c_str(), "w");
        g_draws_bin = fopen((g_dump_dir + "/draws.bin").c_str(), "wb");
    }
    if (!vp || !g_draws_json) return;
    const uint32_t isz = (itype & 0x01000000u) ? 4 : 2;
    uint32_t maxi = 0;
    for (uint32_t i = 0; i < icount; i++) {
        const uint32_t v = isz == 4 ? rd32(idata + 4 * i) : *ptr<uint16_t>(idata + 2 * i);
        if (v > maxi) maxi = v;
    }
    const uint32_t ioff = blob(ptr<void>(idata), icount * isz);
    fprintf(g_draws_json, "{\"prim\":%u,\"isz\":%u,\"icount\":%u,\"ioff\":%u,\"maxi\":%u,\"vp\":%u,\"fp\":%u",
        prim >> 26, isz, icount, ioff, maxi, vp->program, fp ? fp->program : 0);
    fprintf(g_draws_json, ",\"streams\":[");
    for (uint32_t i = 0; i < vp->stream_count; i++) {
        const uint16_t stride = *reinterpret_cast<const uint16_t *>(&vp->streams[4 * i]);
        const uint16_t src = *reinterpret_cast<const uint16_t *>(&vp->streams[4 * i + 2]);
        const uint32_t n = (std::min)((uint32_t)stride * (maxi + 1), 8u << 20);
        const uint32_t off = c.streams[i] ? blob(ptr<void>(c.streams[i]), n) : 0;
        fprintf(g_draws_json, "%s{\"stride\":%u,\"src\":%u,\"off\":%u,\"size\":%u,\"addr\":%u}", i ? "," : "", stride, src, off,
            c.streams[i] ? n : 0, c.streams[i]);
    }
    fprintf(g_draws_json, "],\"attrs\":[");
    for (uint32_t i = 0; i < vp->attr_count; i++) {
        const uint8_t *a = &vp->attributes[8 * i];
        fprintf(g_draws_json, "%s[%u,%u,%u,%u,%u]", i ? "," : "", a[0] | (a[1] << 8), a[2] | (a[3] << 8), a[4], a[5], a[6] | (a[7] << 8));
    }
    const uint32_t vsize = rd32(vp->program + 0x64) * 4, fsize = fp ? rd32(fp->program + 0x64) * 4 : 0;
    const bool hv = c.vuniform && vsize, hf = c.funiform && fsize;
    const uint32_t vu = hv ? blob(ptr<void>(c.vuniform), vsize) : 0;
    const uint32_t fu = hf ? blob(ptr<void>(c.funiform), fsize) : 0;
    fprintf(g_draws_json, "],\"vu\":[%u,%u],\"fu\":[%u,%u],\"tex\":[", vu, hv ? vsize : 0, fu, hf ? fsize : 0);
    bool first = true;
    for (int i = 0; i < 16; i++)
        if (c.textures[i]) {
            const uint32_t *w = ptr<uint32_t>(c.textures[i]);
            fprintf(g_draws_json, "%s[%d,%u,%u,%u,%u]", first ? "" : ",", i, w[0], w[1], w[2], w[3]);
            first = false;
        }
    fprintf(g_draws_json, "],\"vpr\":[%u,%u,%u,%u,%u,%u],\"vps\":[%u,%u,%u,%u,%u,%u]", c.viewport_r[0], c.viewport_r[1],
        c.viewport_r[2], c.viewport_r[3], c.viewport_r[4], c.viewport_r[5], c.viewport_s[0], c.viewport_s[1], c.viewport_s[2],
        c.viewport_s[3], c.viewport_s[4], c.viewport_s[5]);
    const uint32_t blend = fp && fp->blend.size() == 4 ? *reinterpret_cast<const uint32_t *>(fp->blend.data()) : 0;
    const uint32_t cs = c.scene.color_surface;
    fprintf(g_draws_json, ",\"blend\":%u,\"hasblend\":%d,\"outfmt\":%u,\"depth\":[%u,%u],\"cull\":%u,\"cs\":[%u,%u,%u,%u,%u],\"ds\":%u}\n",
        blend, fp && !fp->blend.empty() ? 1 : 0, fp ? fp->output_format : 0, c.depth_func, c.depth_write, c.cull,
        cs ? rd32(cs) : 0, cs ? rd32(cs + 8) : 0, cs ? rd32(cs + 12) : 0, cs ? rd32(cs + 16) : 0, cs ? rd32(cs + 20) : 0,
        c.scene.depth_surface ? rd32(c.scene.depth_surface + 8) : 0);
}


// =============================================================== renderizador Vulkan (ao vivo)
// VITART_VULKAN=1 liga. Os shaders vêm do cache gerado offline por tools/usse_build_cache.py
// (VITART_SPV_DIR, padrão "spv"), pelo FNV-1a de 64 bits do binário do programa; programa sem cache
// é gravado em <dir>/miss/<hash>.gxp para a próxima geração do cache, e o desenho fica de fora.
static uint32_t gxp_param(uint32_t prog, uint32_t i);
static int g_vk = -1; // -1 = ainda não decidido
static std::string g_spv_dir;
static std::mutex g_vk_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<std::vector<uint32_t>>> g_spv; // programa -> SPIR-V
// Texturas decodificadas por descritor. O conteúdo no guest é conferido (hash) uma vez por quadro
// apresentado: o Bink escreve cada quadro do vídeo no mesmo endereço.
struct TexEntry {
    std::shared_ptr<std::vector<uint8_t>> px;
    uint64_t hash = 0;
    uint32_t checked = UINT32_MAX;
};
static std::map<std::array<uint32_t, 4>, TexEntry> g_texcache;
static std::atomic<uint32_t> g_present_frame{0};
static uint64_t content_hash(const uint8_t *p, size_t n, uint64_t h) {
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
static uint64_t g_vk_draws = 0, g_vk_skipped = 0;
// Tempo de CPU (ns) por etapa, zerado a cada relatório: onde o quadro gasta.
static std::atomic<uint64_t> g_t_draw{0}, g_t_gvk{0}, g_t_present{0};
static uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static bool vk_on() {
    if (g_vk < 0) {
        const char *e = getenv("VITART_VULKAN");
        g_vk = e && *e == '1';
        if (g_vk) {
            const char *d = getenv("VITART_SPV_DIR");
            g_spv_dir = d ? d : "spv";
            if (!gvk::init(getenv("VITART_VK_VALIDATION") != nullptr, true)) {
                logf("vitart: Vulkan indisponível: %s", gvk::last_error());
                g_vk = 0;
            } else {
                logf("vitart: renderizador Vulkan ligado (shaders em %s)", g_spv_dir.c_str());
            }
        }
    }
    return g_vk == 1;
}

static std::shared_ptr<std::vector<uint32_t>> spv_of(uint32_t prog) {
    auto it = g_spv.find(prog);
    if (it != g_spv.end()) return it->second;
    const uint32_t size = rd32(prog + 8);
    const uint8_t *b = ptr<uint8_t>(prog);
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < size; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    char name[64];
    std::snprintf(name, sizeof name, "%016llx", (unsigned long long)h);
    std::shared_ptr<std::vector<uint32_t>> code;
    std::ifstream f(g_spv_dir + "/" + name + ".spv", std::ios::binary);
    if (f) {
        std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        code = std::make_shared<std::vector<uint32_t>>(raw.size() / 4);
        std::memcpy(code->data(), raw.data(), code->size() * 4);
    } else {
        std::filesystem::create_directories(g_spv_dir + "/miss");
        std::ofstream o(g_spv_dir + "/miss/" + name + ".gxp", std::ios::binary);
        o.write(reinterpret_cast<const char *>(b), size);
        logf("vitart: programa 0x%08x sem SPIR-V no cache (%s)", prog, name);
    }
    return g_spv[prog] = code;
}

// Formato de atributo (SceGxmAttributeFormat) -> float, normalizado quando pedido.
static float attr_value(const uint8_t *p, uint32_t fmt, uint32_t i) {
    switch (fmt) {
    case 0: return p[i];
    case 1: return (int8_t)p[i];
    case 2: return reinterpret_cast<const uint16_t *>(p)[i];
    case 3: return reinterpret_cast<const int16_t *>(p)[i];
    case 4: return p[i] / 255.0f;
    case 5: return (int8_t)p[i] / 127.0f;
    case 6: return reinterpret_cast<const uint16_t *>(p)[i] / 65535.0f;
    case 7: return reinterpret_cast<const int16_t *>(p)[i] / 32767.0f;
    case 8: {
        const uint16_t hbits = reinterpret_cast<const uint16_t *>(p)[i];
        const uint32_t s = (hbits >> 15) & 1, e = (hbits >> 10) & 31, m = hbits & 1023;
        float v = e == 0 ? std::ldexp((float)m, -24) : e == 31 ? (m ? NAN : INFINITY) : std::ldexp((float)(m | 1024), (int)e - 25);
        return s ? -v : v;
    }
    case 9: return reinterpret_cast<const float *>(p)[i];
    default: return 0.0f;
    }
}
static uint32_t attr_size(uint32_t fmt) { return fmt <= 1 || fmt == 4 || fmt == 5 ? 1 : fmt == 9 ? 4 : 2; }

static void vk_draw_impl(Context &c, uint32_t prim, uint32_t itype, uint32_t idata, uint32_t icount);
static void vk_draw(Context &c, uint32_t prim, uint32_t itype, uint32_t idata, uint32_t icount) {
    const uint64_t t0 = now_ns();
    vk_draw_impl(c, prim, itype, idata, icount);
    g_t_draw += now_ns() - t0;
}
static void vk_draw_impl(Context &c, uint32_t prim, uint32_t itype, uint32_t idata, uint32_t icount) {
    std::shared_ptr<VertexProgram> vp;
    std::shared_ptr<FragmentProgram> fp;
    {
        const std::lock_guard<std::mutex> lock(g_gxm_mutex);
        auto v = g_vprogs.find(c.vprog);
        auto f = g_fprogs.find(c.fprog);
        if (v != g_vprogs.end()) vp = v->second;
        if (f != g_fprogs.end()) fp = f->second;
    }
    const uint32_t cs = c.scene.color_surface;
    if (!vp || !fp || !cs || !icount) return;
    const std::lock_guard<std::mutex> lock(g_vk_mutex);
    auto vs = spv_of(vp->program), fs = spv_of(fp->program);
    if (!vs || !fs) {
        g_vk_skipped++;
        return;
    }
    const uint32_t kind = prim >> 26; // 0 triângulos, 3 faixa, 4 leque (linhas e pontos ficam de fora)
    const uint32_t isz = (itype & 0x01000000u) ? 4 : 2;
    std::vector<uint32_t> idx(icount);
    uint32_t maxi = 0, mini = UINT32_MAX;
    for (uint32_t i = 0; i < icount; i++) {
        idx[i] = isz == 4 ? rd32(idata + 4 * i) : *ptr<uint16_t>(idata + 2 * i);
        if (idx[i] > maxi) maxi = idx[i];
        if (idx[i] < mini) mini = idx[i];
    }
    // Só a faixa de vértices usada vai para a GPU: índices rebaseados pelo menor.
    for (auto &i : idx) i -= mini;
    maxi -= mini;
    std::vector<uint32_t> tris;
    if (kind == 0)
        tris.assign(idx.begin(), idx.begin() + icount / 3 * 3);
    else if (kind == 3)
        for (uint32_t i = 0; i + 2 < icount; i++) tris.insert(tris.end(), {idx[i], idx[i + 1], idx[i + 2]});
    else if (kind == 4)
        for (uint32_t i = 1; i + 1 < icount; i++) tris.insert(tris.end(), {idx[0], idx[i], idx[i + 1]});
    if (tris.empty() || maxi > (1u << 20)) return;
    const uint32_t nv = maxi + 1;

    // Atributos: location k = k-ésimo parâmetro de entrada (categoria 0), ligado pelo índice de recurso.
    std::vector<uint32_t> locs;
    const uint32_t nparams = rd32(vp->program + 0x24);
    for (uint32_t i = 0; i < nparams; i++) {
        const uint32_t p = gxp_param(vp->program, i);
        if ((*ptr<uint8_t>(p + 4) & 15) == 0) locs.push_back(rd32(p + 12));
    }
    std::vector<float> attrs(locs.size() * nv * 4, 0.0f);
    for (size_t k = 0; k < locs.size(); k++)
        for (uint32_t a = 0; a < vp->attr_count; a++) {
            const uint8_t *d = &vp->attributes[8 * a];
            const uint32_t st = d[0] | (d[1] << 8), off = d[2] | (d[3] << 8), fmt = d[4], comps = d[5], reg = d[6] | (d[7] << 8);
            if (reg != locs[k] || st >= 16 || !c.streams[st]) continue;
            const uint32_t stride = *reinterpret_cast<const uint16_t *>(&vp->streams[4 * st]);
            const uint8_t *base = ptr<uint8_t>(c.streams[st] + mini * stride + off);
            float *dst = &attrs[k * nv * 4];
            const uint32_t nc = comps < 4 ? comps : 4;
            if (fmt == 9) { // float: cópia direta
                for (uint32_t v = 0; v < nv; v++) std::memcpy(dst + 4 * v, base + (size_t)v * stride, 4 * nc);
            } else {
                for (uint32_t v = 0; v < nv; v++)
                    for (uint32_t ch = 0; ch < nc; ch++) dst[4 * v + ch] = attr_value(base + (size_t)v * stride, fmt, ch);
            }
            break;
        }

    gvk::Draw d;
    d.prim = kind;
    d.cull = c.cull;
    d.has_blend = fp->blend.size() == 4;
    d.blend = d.has_blend ? *reinterpret_cast<const uint32_t *>(fp->blend.data()) : 0;
    d.depth_func = (c.depth_func >> 22) & 7;
    d.depth_write = c.depth_write == 0;
    std::memcpy(d.vps, c.viewport_s, sizeof d.vps);
    d.target_w = rd32(cs + 8), d.target_h = rd32(cs + 12), d.target_addr = rd32(cs + 20);
    d.depth_addr = c.scene.depth_surface ? rd32(c.scene.depth_surface + 8) : 0;
    d.vertex_count = nv;
    d.attr_count = (uint32_t)locs.size();
    d.attrs = attrs.data();
    d.indices = tris.data();
    d.index_count = (uint32_t)tris.size();
    const uint32_t vsize = rd32(vp->program + 0x64) * 4, fsize = rd32(fp->program + 0x64) * 4;
    if (c.vuniform && vsize) d.vert_uniforms = ptr<uint8_t>(c.vuniform), d.vert_uniforms_size = vsize;
    if (c.funiform && fsize) d.frag_uniforms = ptr<uint8_t>(c.funiform), d.frag_uniforms_size = fsize;
    d.vs = vs->data(), d.vs_size = (uint32_t)vs->size() * 4;
    d.fs = fs->data(), d.fs_size = (uint32_t)fs->size() * 4;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> keep;
    for (uint32_t i = 0; i < 16; i++) {
        if (!c.textures[i]) continue;
        std::array<uint32_t, 4> w;
        std::memcpy(w.data(), ptr<uint32_t>(c.textures[i]), 16);
        const gxmtex::Info info = gxmtex::describe(w.data());
        gvk::Texture t;
        t.binding = i;
        t.width = info.width, t.height = info.height;
        if (gvk::is_target(info.data)) {
            t.target_addr = info.data;
        } else if (gvk::is_depth(info.data)) {
            t.target_addr = info.data, t.depth = true;
        } else {
            TexEntry &e = g_texcache[w];
            const uint32_t frame = g_present_frame.load();
            if (e.checked != frame) {
                e.checked = frame;
                const size_t n = gxmtex::data_size(info);
                uint64_t h = content_hash(reinterpret_cast<const uint8_t *>(w.data()), 16, 1469598103934665603ull);
                if (n) h = content_hash(ptr<uint8_t>(info.data), n, h);
                if (info.base == 0x95000000u) h = content_hash(ptr<uint8_t>(info.palette), 1024, h);
                if (!e.px || h != e.hash) {
                    auto px = std::make_shared<std::vector<uint8_t>>();
                    if (!n || !gxmtex::decode(ptr<uint8_t>(0), info, *px)) px.reset();
                    e.px = px;
                    e.hash = h;
                }
            }
            if (!e.px) continue;
            keep.push_back(e.px);
            t.rgba = e.px->data();
            t.key = e.hash | 1;
        }
        d.textures.push_back(t);
    }
    const uint64_t tg = now_ns();
    const bool drew = gvk::draw(d);
    g_t_gvk += now_ns() - tg;
    if (!drew)
        logf("vitart: desenho Vulkan falhou: %s", gvk::last_error());
    else
        g_vk_draws++;
}

void gxm_present(uint32_t fb) {
    if (g_vk != 1) return;
    static uint32_t frame = 0;
    static const uint32_t dump_at = getenv("VITART_VK_DUMP") ? (uint32_t)std::atoi(getenv("VITART_VK_DUMP")) : 0;
    if (++frame == dump_at || (dump_at && frame % 300 == 0)) {
        // Quadro apresentado em PPM (RGB): medir a imagem sem captura de tela.
        std::vector<uint8_t> px;
        uint32_t w, h;
        if (gvk::read_target(fb, px, w, h)) {
            char path[64];
            std::snprintf(path, sizeof path, "vkframe_%05u.ppm", frame);
            if (FILE *f = fopen(path, "wb")) {
                fprintf(f, "P6\n%u %u\n255\n", w, h);
                for (size_t i = 0; i < (size_t)w * h; i++) fwrite(&px[4 * i], 1, 3, f);
                fclose(f);
            }
            logf("vitart: quadro %u apresentado -> %s (%llu desenhos Vulkan, %llu sem shader)", frame, path,
                 (unsigned long long)g_vk_draws, (unsigned long long)g_vk_skipped);
        }
    }
    g_present_frame++;
    const uint64_t tp = now_ns();
    gvk::present(fb);
    g_t_present += now_ns() - tp;
    if (frame % 120 == 0) {
        logf("vitart: tempo por quadro (média de 120): vk_draw %.1f ms (gvk::draw %.1f ms), apresentar %.1f ms",
             g_t_draw.exchange(0) / 120e6, g_t_gvk.exchange(0) / 120e6, g_t_present.exchange(0) / 120e6);
        const gvk::Stats s = gvk::take_stats();
        logf("vitart:   gvk por quadro: espera %.1f ms, GPU %.1f ms, imagens %.1f ms, pipelines %.1f ms, anel %.1f ms, "
             "descritores %.1f ms; %.1f envios, %.1f desenhos, %.1f cópias de alvo, %.1f texturas, %u pipelines novos",
             s.wait_ns / 120e6, s.gpu_ns / 120e6, s.alloc_ns / 120e6, s.pipe_ns / 120e6, s.ring_ns / 120e6,
             s.desc_ns / 120e6, s.submits / 120.0, s.draws / 120.0, s.snapshots / 120.0, s.uploads / 120.0, s.pipes);
    }
    if (gvk::window_closed()) {
        logf("vitart: janela fechada (%llu desenhos Vulkan, %llu sem shader)", (unsigned long long)g_vk_draws,
             (unsigned long long)g_vk_skipped);
        gvk::shutdown();
        std::exit(0);
    }
}

// =============================================================== inicialização
struct DisplayEntry {
    std::vector<uint8_t> data;
};
static uint32_t g_display_cb = 0, g_display_data_size = 0, g_display_max_pending = 2;
static std::mutex g_dq_mutex;
static std::condition_variable g_dq_cv;
static std::deque<DisplayEntry> g_dq;
static uint32_t g_dq_buffer = 0; // cópia dos dados do callback no guest (uma por entrada pendente)

static void display_thread(Thread *t) {
    for (;;) {
        DisplayEntry e;
        {
            std::unique_lock<std::mutex> lock(g_dq_mutex);
            g_dq_cv.wait(lock, [] { return !g_dq.empty(); });
            e = std::move(g_dq.front());
        }
        if (g_display_cb) {
            std::memcpy(ptr<uint8_t>(g_dq_buffer), e.data.data(), e.data.size());
            call_guest(&t->ctx, g_display_cb, { g_dq_buffer });
        }
        {
            const std::lock_guard<std::mutex> lock(g_dq_mutex);
            g_dq.pop_front();
        }
        g_dq_cv.notify_all();
        report_frame();
    }
}

VT_HLE(sceGxmInitialize) {
    const uint32_t p = arg(ctx, 0);
    if (!p) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    g_display_max_pending = rd32(p + 4) ? rd32(p + 4) : 2;
    g_display_cb = rd32(p + 8);
    g_display_data_size = rd32(p + 12);
    g_dq_buffer = new_object(g_display_data_size ? g_display_data_size : 16, "gxm:display-data");
    dump_init();
    spawn_host_thread("SceGxmDisplayQueue", display_thread);
    logf("vitart: sceGxmInitialize: callback 0x%08x, dados %u bytes, até %u pendentes, parameter buffer 0x%x",
        g_display_cb, g_display_data_size, g_display_max_pending, rd32(p + 16));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmTerminate) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmMapMemory) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmUnmapMemory) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmMapVertexUsseMemory) {
    if (arg(ctx, 2)) wr32(arg(ctx, 2), arg(ctx, 0));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmMapFragmentUsseMemory) {
    if (arg(ctx, 2)) wr32(arg(ctx, 2), arg(ctx, 0));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmUnmapVertexUsseMemory) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmUnmapFragmentUsseMemory) { ret(ctx, SCE_OK); }

VT_HLE(sceGxmCreateContext) {
    // (const SceGxmContextParams *params, SceGxmContext **context)
    const uint32_t p = arg(ctx, 0);
    if (!p || !arg(ctx, 1)) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    auto c = std::make_shared<Context>();
    c->vertex_ring = rd32(p + 16);
    c->vertex_ring_size = rd32(p + 20);
    c->fragment_ring = rd32(p + 24);
    c->fragment_ring_size = rd32(p + 28);
    const uint32_t h = new_object(0x100, "gxm:context");
    {
        const std::lock_guard<std::mutex> lock(g_gxm_mutex);
        g_contexts[h] = c;
    }
    wr32(arg(ctx, 1), h);
    logf("vitart: sceGxmCreateContext -> 0x%08x (anel de vértices 0x%08x+0x%x, de fragmentos 0x%08x+0x%x)", h,
        c->vertex_ring, c->vertex_ring_size, c->fragment_ring, c->fragment_ring_size);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmDestroyContext) { ret(ctx, SCE_OK); }

VT_HLE(sceGxmGetRenderTargetMemSize) {
    if (!arg(ctx, 1)) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    wr32(arg(ctx, 1), 0x10000);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmCreateRenderTarget) {
    // (const SceGxmRenderTargetParams *params, SceGxmRenderTarget **renderTarget)
    if (!arg(ctx, 1)) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    const uint32_t h = new_object(0x40, "gxm:render-target");
    std::memcpy(ptr<uint8_t>(h), ptr<uint8_t>(arg(ctx, 0)), 0x14); // flags, largura/altura, regiões...
    wr32(arg(ctx, 1), h);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmDestroyRenderTarget) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmSyncObjectCreate) {
    if (!arg(ctx, 0)) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    wr32(arg(ctx, 0), new_object(0x10, "gxm:sync"));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmSyncObjectDestroy) { ret(ctx, SCE_OK); }

// Superfícies: o layout de SceGxmColorSurface é privado do sistema; guardamos os parâmetros nele
// mesmo (o jogo não lê de volta), no nosso formato.
VT_HLE(sceGxmColorSurfaceInit) {
    // (surface, colorFormat, surfaceType, scaleMode, outputRegisterSize, width, height, strideInPixels, data)
    const uint32_t s = arg(ctx, 0);
    if (!s) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    std::memset(ptr<uint8_t>(s), 0, 0x30);
    wr32(s + 0, arg(ctx, 1));  // formato
    wr32(s + 4, arg(ctx, 2));  // tipo (linear/tiled/swizzled)
    wr32(s + 8, arg(ctx, 5));  // largura
    wr32(s + 12, arg(ctx, 6)); // altura
    wr32(s + 16, arg(ctx, 7)); // stride em pixels
    wr32(s + 20, arg(ctx, 8)); // dados
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmDepthStencilSurfaceInit) {
    // (surface, depthStencilFormat, surfaceType, strideInSamples, depthData, stencilData)
    const uint32_t s = arg(ctx, 0);
    if (!s) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    std::memset(ptr<uint8_t>(s), 0, 0x18);
    wr32(s + 0, arg(ctx, 1));
    wr32(s + 4, arg(ctx, 3));
    wr32(s + 8, arg(ctx, 4));
    wr32(s + 12, arg(ctx, 5));
    ret(ctx, SCE_OK);
}
static std::set<uint32_t> g_force_load; // superfícies de profundidade com "force load" ligado
VT_HLE(sceGxmDepthStencilSurfaceSetForceLoadMode) {
    const std::lock_guard<std::mutex> lock(g_gxm_mutex);
    if (arg(ctx, 1)) g_force_load.insert(arg(ctx, 0)); else g_force_load.erase(arg(ctx, 0));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmDepthStencilSurfaceSetForceStoreMode) { ret(ctx, SCE_OK); }

// ============================================================ shader patcher
VT_HLE(sceGxmShaderPatcherCreate) {
    // (const SceGxmShaderPatcherParams *params, SceGxmShaderPatcher **shaderPatcher)
    if (!arg(ctx, 1)) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    wr32(arg(ctx, 1), new_object(0x40, "gxm:shader-patcher"));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmShaderPatcherDestroy) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmShaderPatcherRegisterProgram) {
    // (patcher, const SceGxmProgram *programHeader, SceGxmShaderPatcherId *programId)
    const uint32_t prog = arg(ctx, 1);
    if (!prog || !arg(ctx, 2)) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    if (rd32(prog) != 0x00505847u) // "GXP\0"
        logf("vitart: RegisterProgram: 0x%08x não começa com GXP", prog);
    // O jogo registra o mesmo programa várias vezes: um id por programa basta.
    const std::lock_guard<std::mutex> lock(g_gxm_mutex);
    uint32_t &id = g_id_of_program[prog];
    if (!id) {
        id = new_object(0x10, "gxm:program-id");
        g_program_ids[id] = prog;
    }
    wr32(id, prog);
    wr32(arg(ctx, 2), id);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmShaderPatcherUnregisterProgram) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmShaderPatcherGetProgramFromId) {
    const uint32_t id = arg(ctx, 0);
    ret(ctx, id ? rd32(id) : 0);
}
VT_HLE(sceGxmShaderPatcherCreateVertexProgram) {
    // (patcher, programId, attributes, attributeCount, streams, streamCount, SceGxmVertexProgram **out)
    const uint32_t id = arg(ctx, 1), out = arg(ctx, 6);
    if (!id || !out) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    auto vp = std::make_shared<VertexProgram>();
    vp->program = rd32(id);
    vp->attr_count = arg(ctx, 3);
    vp->stream_count = arg(ctx, 5);
    if (arg(ctx, 2)) vp->attributes.assign(ptr<uint8_t>(arg(ctx, 2)), ptr<uint8_t>(arg(ctx, 2)) + 8 * vp->attr_count);
    if (arg(ctx, 4)) vp->streams.assign(ptr<uint8_t>(arg(ctx, 4)), ptr<uint8_t>(arg(ctx, 4)) + 4 * vp->stream_count);
    // Como o patcher real: a mesma combinação devolve o mesmo programa (o jogo pede a cada uso).
    std::string key(reinterpret_cast<const char *>(&vp->program), 4);
    key.append(vp->attributes.begin(), vp->attributes.end()).push_back('|');
    key.append(vp->streams.begin(), vp->streams.end());
    const std::lock_guard<std::mutex> lock(g_gxm_mutex);
    uint32_t &h = g_vprog_cache[key];
    if (!h) {
        h = new_object(0x20, "gxm:vertex-program");
        wr32(h, vp->program);
        g_vprogs[h] = vp;
    }
    wr32(out, h);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmShaderPatcherCreateFragmentProgram) {
    // (patcher, programId, outputFormat, multisampleMode, blendInfo, vertexProgram, SceGxmFragmentProgram **out)
    const uint32_t id = arg(ctx, 1), out = arg(ctx, 6);
    if (!id || !out) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    auto fp = std::make_shared<FragmentProgram>();
    fp->program = rd32(id);
    fp->output_format = arg(ctx, 2);
    fp->msaa = arg(ctx, 3);
    if (arg(ctx, 4)) fp->blend.assign(ptr<uint8_t>(arg(ctx, 4)), ptr<uint8_t>(arg(ctx, 4)) + 4); // SceGxmBlendInfo: 4 bytes
    fp->vertex_program = arg(ctx, 5) ? rd32(arg(ctx, 5)) : 0;
    std::string key(reinterpret_cast<const char *>(&fp->program), 4);
    key.append(reinterpret_cast<const char *>(&fp->output_format), 4);
    key.append(reinterpret_cast<const char *>(&fp->msaa), 4);
    key.append(reinterpret_cast<const char *>(&fp->vertex_program), 4);
    key.append(fp->blend.begin(), fp->blend.end());
    const std::lock_guard<std::mutex> lock(g_gxm_mutex);
    uint32_t &h = g_fprog_cache[key];
    if (!h) {
        h = new_object(0x20, "gxm:fragment-program");
        wr32(h, fp->program);
        g_fprogs[h] = fp;
    }
    wr32(out, h);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmShaderPatcherReleaseVertexProgram) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmShaderPatcherReleaseFragmentProgram) { ret(ctx, SCE_OK); }

// ============================================== consultas ao binário GXP
// Cabeçalho: +0x24 parameter_count, +0x28 parameters_offset (relativo ao próprio campo),
// +0x64 default_uniform_buffer_count (em palavras de 4 bytes). Parâmetro (16 bytes): +0 name_offset
// (relativo ao parâmetro), +4 categoria:4 tipo:4 componentes:4 contêiner:4, +6 semântica,
// +7 índice de semântica, +8 tamanho do array, +12 índice de recurso.
static uint32_t gxp_param(uint32_t prog, uint32_t i) { return prog + 0x28 + rd32(prog + 0x28) + 16 * i; }

VT_HLE(sceGxmProgramGetParameterCount) { ret(ctx, arg(ctx, 0) ? rd32(arg(ctx, 0) + 0x24) : 0); }
VT_HLE(sceGxmProgramGetParameter) {
    const uint32_t prog = arg(ctx, 0), i = arg(ctx, 1);
    ret(ctx, (prog && i < rd32(prog + 0x24)) ? gxp_param(prog, i) : 0);
}
VT_HLE(sceGxmProgramFindParameterByName) {
    const uint32_t prog = arg(ctx, 0);
    const char *name = gstr(arg(ctx, 1));
    if (!prog) return ret(ctx, 0);
    const uint32_t n = rd32(prog + 0x24);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t p = gxp_param(prog, i);
        if (std::strcmp(gstr(p + rd32(p)), name) == 0)
            return ret(ctx, p);
    }
    ret(ctx, 0);
}
VT_HLE(sceGxmProgramParameterGetName) { ret(ctx, arg(ctx, 0) ? arg(ctx, 0) + rd32(arg(ctx, 0)) : 0); }
VT_HLE(sceGxmProgramParameterGetCategory) { ret(ctx, ptr<uint8_t>(arg(ctx, 0))[4] & 0xF); }
VT_HLE(sceGxmProgramParameterGetType) { ret(ctx, ptr<uint8_t>(arg(ctx, 0))[4] >> 4); }
VT_HLE(sceGxmProgramParameterGetComponentCount) { ret(ctx, ptr<uint8_t>(arg(ctx, 0))[5] & 0xF); }
VT_HLE(sceGxmProgramParameterGetContainerIndex) { ret(ctx, ptr<uint8_t>(arg(ctx, 0))[5] >> 4); }
VT_HLE(sceGxmProgramParameterGetSemantic) { ret(ctx, ptr<uint8_t>(arg(ctx, 0))[6]); }
VT_HLE(sceGxmProgramParameterGetSemanticIndex) { ret(ctx, ptr<uint8_t>(arg(ctx, 0))[7] & 0xF); }
VT_HLE(sceGxmProgramParameterGetArraySize) { ret(ctx, rd32(arg(ctx, 0) + 8)); }
VT_HLE(sceGxmProgramParameterGetResourceIndex) { ret(ctx, rd32(arg(ctx, 0) + 12)); }

// ================================================================= texturas
// Bitfields de SceGxmTexture (vita-headers/psp2/gxm.h), palavra a palavra.
namespace tex {
inline uint32_t &w(uint32_t t, int k) { return *ptr<uint32_t>(t + 4 * k); }
inline uint32_t get(uint32_t v, int lo, int n) { return (v >> lo) & ((1u << n) - 1); }
inline void set(uint32_t &v, int lo, int n, uint32_t x) { const uint32_t m = ((1u << n) - 1) << lo; v = (v & ~m) | ((x << lo) & m); }
inline uint32_t type(uint32_t t) { return get(w(t, 1), 29, 3) << 29; }
inline bool pot_dims(uint32_t ty) { return ty == 0x00000000u || ty == 0x40000000u; } // SWIZZLED, CUBE
inline int bit_width(uint32_t x) { int n = 0; while (x) { n++; x >>= 1; } return n; }
} // namespace tex

static uint32_t texture_init(arc_ctx *ctx, uint32_t ty) {
    // (texture, data, texFormat, width, height, mipCount)
    using namespace tex;
    const uint32_t t = arg(ctx, 0), data = arg(ctx, 1), fmt = arg(ctx, 2), wd = arg(ctx, 3), ht = arg(ctx, 4), mips = arg(ctx, 5);
    if (!t) return GXM_ERROR_INVALID_POINTER;
    if (wd == 0 || ht == 0 || wd > 4096 || ht > 4096 || mips > 13) return GXM_ERROR_INVALID_VALUE;
    std::memset(ptr<uint8_t>(t), 0, 16);
    uint32_t &w0 = w(t, 0), &w1 = w(t, 1), &w2 = w(t, 2), &w3 = w(t, 3);
    set(w0, 17, 4, std::min<uint32_t>(15, mips ? mips - 1 : 0)); // mip_count
    set(w0, 21, 6, 31);                                          // lod_bias neutro
    set(w0, 31, 1, fmt >> 31);                                   // format0
    set(w0, 3, 3, 1);                                            // v: CLAMP
    set(w0, 6, 3, 1);                                            // u: CLAMP
    if (pot_dims(ty)) {
        set(w1, 0, 4, bit_width(ht) - 1);
        set(w1, 16, 4, bit_width(wd) - 1);
    } else {
        set(w1, 0, 12, ht - 1);
        set(w1, 12, 12, wd - 1);
    }
    set(w1, 24, 5, (fmt >> 24) & 0x1F);
    set(w1, 29, 3, ty >> 29);
    w2 = data & ~3u;
    set(w3, 28, 3, (fmt >> 12) & 7);
    set(w3, 31, 1, 1); // normalize
    return SCE_OK;
}
VT_HLE(sceGxmTextureInitSwizzled) { ret(ctx, texture_init(ctx, 0x00000000u)); }
VT_HLE(sceGxmTextureInitSwizzledArbitrary) { ret(ctx, texture_init(ctx, 0xA0000000u)); }
VT_HLE(sceGxmTextureInitLinear) { ret(ctx, texture_init(ctx, 0x60000000u)); }
VT_HLE(sceGxmTextureInitTiled) { ret(ctx, texture_init(ctx, 0x80000000u)); }
VT_HLE(sceGxmTextureInitCube) { ret(ctx, texture_init(ctx, 0x40000000u)); }
VT_HLE(sceGxmTextureInitCubeArbitrary) { ret(ctx, texture_init(ctx, 0xE0000000u)); }
VT_HLE(sceGxmTextureInitLinearStrided) {
    // (texture, data, texFormat, width, height, byteStride): o stride ocupa os campos de mip/filtro.
    using namespace tex;
    const uint32_t t = arg(ctx, 0), data = arg(ctx, 1), fmt = arg(ctx, 2), wd = arg(ctx, 3), ht = arg(ctx, 4), stride = arg(ctx, 5);
    if (!t) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    if (wd == 0 || ht == 0 || wd > 4096 || ht > 4096 || (stride & 3) || stride < 4 || stride > 131072)
        return ret(ctx, GXM_ERROR_INVALID_VALUE);
    std::memset(ptr<uint8_t>(t), 0, 16);
    uint32_t &w0 = w(t, 0), &w1 = w(t, 1), &w2 = w(t, 2), &w3 = w(t, 3);
    const uint32_t sc = (stride >> 2) - 1;
    set(w0, 9, 1, sc & 1);
    set(w0, 10, 2, (sc >> 1) & 3);
    set(w0, 17, 4, (sc >> 3) & 0xF);
    set(w0, 21, 6, (sc >> 7) & 0x3F);
    set(w0, 31, 1, fmt >> 31);
    set(w0, 3, 3, 1);
    set(w0, 6, 3, 1);
    set(w1, 0, 12, ht - 1);
    set(w1, 12, 12, wd - 1);
    set(w1, 24, 5, (fmt >> 24) & 0x1F);
    set(w1, 29, 3, 0xC0000000u >> 29);
    w2 = data & ~3u;
    set(w3, 28, 3, (fmt >> 12) & 7);
    set(w3, 31, 1, 1);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmTextureGetFormat) {
    using namespace tex;
    const uint32_t t = arg(ctx, 0);
    ret(ctx, (get(w(t, 0), 31, 1) << 31) | (get(w(t, 1), 24, 5) << 24) | (get(w(t, 3), 28, 3) << 12));
}
VT_HLE(sceGxmTextureGetType) { ret(ctx, tex::type(arg(ctx, 0))); }
VT_HLE(sceGxmTextureGetWidth) {
    using namespace tex;
    const uint32_t t = arg(ctx, 0);
    ret(ctx, pot_dims(type(t)) ? 1u << get(w(t, 1), 16, 4) : get(w(t, 1), 12, 12) + 1);
}
VT_HLE(sceGxmTextureGetHeight) {
    using namespace tex;
    const uint32_t t = arg(ctx, 0);
    ret(ctx, pot_dims(type(t)) ? 1u << get(w(t, 1), 0, 4) : get(w(t, 1), 0, 12) + 1);
}
VT_HLE(sceGxmTextureGetStride) {
    using namespace tex;
    const uint32_t t = arg(ctx, 0);
    if (type(t) != 0xC0000000u) return ret(ctx, 0);
    const uint32_t w0 = w(t, 0);
    const uint32_t sc = get(w0, 9, 1) | (get(w0, 10, 2) << 1) | (get(w0, 17, 4) << 3) | (get(w0, 21, 6) << 7);
    ret(ctx, (sc + 1) << 2);
}
VT_HLE(sceGxmTextureGetMipmapCount) {
    using namespace tex;
    const uint32_t t = arg(ctx, 0);
    ret(ctx, type(t) == 0xC0000000u ? 0 : get(w(t, 0), 17, 4) + 1);
}
VT_HLE(sceGxmTextureGetPalette) { ret(ctx, tex::get(tex::w(arg(ctx, 0), 3), 0, 26) << 6); }
VT_HLE(sceGxmTextureSetPalette) {
    tex::set(tex::w(arg(ctx, 0), 3), 0, 26, arg(ctx, 1) >> 6);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmTextureSetMinFilter) {
    if (tex::type(arg(ctx, 0)) != 0xC0000000u) tex::set(tex::w(arg(ctx, 0), 0), 10, 2, arg(ctx, 1));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmTextureSetMagFilter) {
    tex::set(tex::w(arg(ctx, 0), 0), 12, 2, arg(ctx, 1));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmTextureSetUAddrMode) {
    tex::set(tex::w(arg(ctx, 0), 0), 6, 3, arg(ctx, 1));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmTextureSetVAddrMode) {
    tex::set(tex::w(arg(ctx, 0), 0), 3, 3, arg(ctx, 1));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmTextureSetLodBias) {
    if (tex::type(arg(ctx, 0)) != 0xC0000000u) tex::set(tex::w(arg(ctx, 0), 0), 21, 6, arg(ctx, 1));
    ret(ctx, SCE_OK);
}

// ============================================================= cenas e desenho
VT_HLE(sceGxmBeginScene) {
    // (context, flags, renderTarget, validRegion, vertexSyncObject, fragmentSyncObject, colorSurface, depthStencil)
    auto c = context_of(arg(ctx, 0));
    if (!c) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    c->in_scene = true;
    c->scene = Scene{ arg(ctx, 2), arg(ctx, 6), arg(ctx, 7), {} };
    if (vk_on() && arg(ctx, 6)) {
        bool load;
        {
            const std::lock_guard<std::mutex> lock(g_gxm_mutex);
            load = g_force_load.count(arg(ctx, 7)) != 0;
        }
        if (!load) gvk::begin_scene(rd32(arg(ctx, 6) + 20));
    }
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmEndScene) {
    // (context, const SceGxmNotification *vertexNotification, const SceGxmNotification *fragmentNotification)
    auto c = context_of(arg(ctx, 0));
    if (!c) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    c->in_scene = false;
    g_scenes++;
    // Sem GPU ainda: a cena "termina" já; as notificações liberam o jogo para reusar os buffers.
    for (int k = 1; k <= 2; k++)
        if (const uint32_t n = arg(ctx, k))
            if (const uint32_t addr = rd32(n))
                __atomic_store_n(ptr<uint32_t>(addr), rd32(n + 4), __ATOMIC_RELEASE);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmDisplayQueueAddEntry) {
    // (oldBuffer, newBuffer, const void *callbackData)
    if (dumping()) dump_write(arg(ctx, 1));
    g_submitted++;
    DisplayEntry e;
    if (arg(ctx, 2) && g_display_data_size)
        e.data.assign(ptr<uint8_t>(arg(ctx, 2)), ptr<uint8_t>(arg(ctx, 2)) + g_display_data_size);
    std::unique_lock<std::mutex> lock(g_dq_mutex);
    g_dq_cv.wait(lock, [] { return g_dq.size() < g_display_max_pending; });
    g_dq.push_back(std::move(e));
    lock.unlock();
    g_dq_cv.notify_all();
    ret(ctx, SCE_OK);
}

VT_HLE(sceGxmSetVertexProgram) { if (auto c = context_of(arg(ctx, 0))) c->vprog = arg(ctx, 1); }
VT_HLE(sceGxmSetFragmentProgram) { if (auto c = context_of(arg(ctx, 0))) c->fprog = arg(ctx, 1); }
VT_HLE(sceGxmSetVertexStream) {
    auto c = context_of(arg(ctx, 0));
    if (c && arg(ctx, 1) < 16) c->streams[arg(ctx, 1)] = arg(ctx, 2);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmSetFragmentTexture) {
    auto c = context_of(arg(ctx, 0));
    if (c && arg(ctx, 1) < 16) c->textures[arg(ctx, 1)] = arg(ctx, 2);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmDraw) {
    // (context, primType, indexType, indexData, indexCount)
    auto c = context_of(arg(ctx, 0));
    if (!c) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    g_draws++;
    if (dumping()) {
        for (uint32_t t : c->textures) dump_texture(t);
        dump_draw(*c, arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 4));
    }
    if (vk_on()) vk_draw(*c, arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 4));
    if (c->in_scene && c->scene.draws.size() < 100000)
        c->scene.draws.push_back(DrawCall{ arg(ctx, 1), arg(ctx, 2), arg(ctx, 3), arg(ctx, 4), c->vprog, c->fprog });
    ret(ctx, SCE_OK);
}

// Reserva no anel do contexto o default uniform buffer do programa ativo.
static uint32_t reserve_uniforms(uint32_t &ring, uint32_t &size, uint32_t &pos, uint32_t prog_handle) {
    const uint32_t prog = prog_handle ? rd32(prog_handle) : 0;
    const uint32_t bytes = prog ? rd32(prog + 0x64) * 4 : 0;
    const uint32_t need = (bytes + 15) & ~15u;
    if (!ring || !size) return 0;
    if (pos + need > size) pos = 0;
    const uint32_t a = ring + pos;
    pos += need ? need : 16;
    return a;
}
VT_HLE(sceGxmReserveVertexDefaultUniformBuffer) {
    auto c = context_of(arg(ctx, 0));
    if (!c || !arg(ctx, 1)) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    c->vuniform = reserve_uniforms(c->vertex_ring, c->vertex_ring_size, c->vertex_ring_pos, c->vprog);
    wr32(arg(ctx, 1), c->vuniform);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmReserveFragmentDefaultUniformBuffer) {
    auto c = context_of(arg(ctx, 0));
    if (!c || !arg(ctx, 1)) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    c->funiform = reserve_uniforms(c->fragment_ring, c->fragment_ring_size, c->fragment_ring_pos, c->fprog);
    wr32(arg(ctx, 1), c->funiform);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmSetUniformDataF) {
    // (uniformBuffer, parameter, componentOffset, componentCount, const float *sourceData)
    const uint32_t buf = arg(ctx, 0), param = arg(ctx, 1), off = arg(ctx, 2), n = arg(ctx, 3), src = arg(ctx, 4);
    if (!buf || !param || !src) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    const uint32_t comps = *ptr<uint8_t>(param + 5) & 15, type = *ptr<uint8_t>(param + 4) >> 4, array = rd32(param + 8);
    if (type == 0 && array > 1 && comps > 1) {
        // Array de vetores float: cada elemento ocupa comps*4 bytes alinhados a 8 (float3 -> 16), e o
        // deslocamento conta componentes dentro desses elementos. Copiar contíguo punha o 2º vec3 de
        // g_LightDir uma palavra antes e o shader lia sobra do anel (manchas no rosto do Kratos).
        const uint32_t stride = (comps * 4 + 7) & ~7u;
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t c = off + k;
            wr32(buf + 4 * rd32(param + 12) + (c / comps) * stride + (c % comps) * 4, rd32(src + 4 * k));
        }
    } else {
        const uint32_t base = buf + 4 * (rd32(param + 12) + off);
        std::memcpy(ptr<uint8_t>(base), ptr<uint8_t>(src), 4 * n);
    }
    if (std::getenv("VITART_TRACE_UNIFORMS")) {
        // Diagnóstico: quem envia cada uniform (uma linha por par chamador/nome).
        static std::mutex mu;
        static std::set<std::pair<uint32_t, std::string>> seen;
        const std::string name = gstr(param + rd32(param));
        const std::lock_guard<std::mutex> lock(mu);
        if (seen.insert({ ctx->r[14], name }).second) {
            const float *f = ptr<float>(src);
            logf("vitart: uniform %s n=%u off=%u src=0x%08x lr=0x%08x primeiros: %g %g %g %g | %g %g %g %g", name.c_str(), n, off,
                src, ctx->r[14], f[0], f[1], f[2], f[3], n > 15 ? f[12] : 0, n > 15 ? f[13] : 0, n > 15 ? f[14] : 0, n > 15 ? f[15] : 0);
        }
    }
    ret(ctx, SCE_OK);
}

// Desenho pré-computado: o estado vive na memória que o jogo fornece.
VT_HLE(sceGxmGetPrecomputedDrawSize) { ret(ctx, 0x40); }
VT_HLE(sceGxmPrecomputedDrawInit) {
    // (state, vertexProgram, memory)
    std::memset(ptr<uint8_t>(arg(ctx, 0)), 0, 0x14);
    wr32(arg(ctx, 0) + 0, arg(ctx, 1));
    wr32(arg(ctx, 0) + 4, arg(ctx, 2));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmPrecomputedDrawSetParams) {
    // (state, primType, indexType, indexData, indexCount): guardado na memória do estado
    const uint32_t mem = rd32(arg(ctx, 0) + 4);
    if (mem) {
        wr32(mem + 0, arg(ctx, 1));
        wr32(mem + 4, arg(ctx, 2));
        wr32(mem + 8, arg(ctx, 3));
        wr32(mem + 12, arg(ctx, 4));
    }
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmPrecomputedDrawSetVertexStream) {
    const uint32_t mem = rd32(arg(ctx, 0) + 4);
    if (mem && arg(ctx, 1) < 8) wr32(mem + 16 + 4 * arg(ctx, 1), arg(ctx, 2));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmDrawPrecomputed) {
    // (context, const SceGxmPrecomputedDraw *state): programa de vértice e streams vêm do estado.
    g_draws++;
    const uint32_t st = arg(ctx, 1);
    if (st && vk_on())
        if (auto c = context_of(arg(ctx, 0))) {
            Context tmp = *c;
            tmp.vprog = rd32(st);
            const uint32_t mem = rd32(st + 4);
            for (int i = 0; i < 8; i++) tmp.streams[i] = rd32(mem + 16 + 4 * i);
            vk_draw(tmp, rd32(mem), rd32(mem + 4), rd32(mem + 8), rd32(mem + 12));
        }
    if (st && dumping())
        if (auto c = context_of(arg(ctx, 0))) {
            Context tmp = *c;
            tmp.vprog = rd32(st);
            const uint32_t mem = rd32(st + 4);
            for (int i = 0; i < 8; i++) tmp.streams[i] = rd32(mem + 16 + 4 * i);
            for (uint32_t t : tmp.textures) dump_texture(t);
            dump_draw(tmp, rd32(mem), rd32(mem + 4), rd32(mem + 8), rd32(mem + 12));
        }
    ret(ctx, SCE_OK);
}

// Estado fixo: só registrado quando o renderizador existir; por ora aceito.
VT_HLE(sceGxmSetViewport) {
    // (context, xOffset, xScale, yOffset, yScale, zOffset, zScale): guarda r1..r3+pilha e s0..s5
    if (auto c = context_of(arg(ctx, 0)))
        for (int i = 0; i < 6; i++) {
            c->viewport_r[i] = arg(ctx, 1 + i);
            c->viewport_s[i] = (uint32_t)(ctx->d[i / 2] >> (32 * (i & 1)));
        }
}
VT_HLE(sceGxmSetCullMode) { if (auto c = context_of(arg(ctx, 0))) c->cull = arg(ctx, 1); }
VT_HLE(sceGxmSetTwoSidedEnable) { if (auto c = context_of(arg(ctx, 0))) c->two_sided = arg(ctx, 1); }
VT_HLE(sceGxmSetFrontDepthFunc) { if (auto c = context_of(arg(ctx, 0))) c->depth_func = arg(ctx, 1); }
VT_HLE(sceGxmSetBackDepthFunc) {}
VT_HLE(sceGxmSetFrontDepthWriteEnable) { if (auto c = context_of(arg(ctx, 0))) c->depth_write = arg(ctx, 1); }
VT_HLE(sceGxmSetBackDepthWriteEnable) {}
VT_HLE(sceGxmSetFrontStencilFunc) {}
VT_HLE(sceGxmSetBackStencilFunc) {}
VT_HLE(sceGxmSetFrontStencilRef) {}
VT_HLE(sceGxmSetBackStencilRef) {}
VT_HLE(sceGxmSetFrontPolygonMode) {}
VT_HLE(sceGxmSetUserMarker) { ret(ctx, SCE_OK); }

} // namespace vt
