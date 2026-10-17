// HLE do sceGxm para o vitart: camada de ESTADO. Objetos (contexto, shader patcher, programas,
// render targets, sync objects) existem como blocos na memória do guest (para o jogo guardar
// ponteiros válidos) com o estado no host. Ainda não desenha: grava o que cada cena pede e
// conclui as notificações na hora, para o jogo seguir o laço. O renderizador Vulkan entra aqui.
#include "vitart.h"
#include "gxm_tex.h"
#include "gxm_vk.h"
#include "pbr.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO_WARNINGS
#include "stb_image.h"

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
    gvk::Stencil stencil_front, stencil_back; // estado de stencil por face (frente/verso)
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
    {
        const uint32_t dsurf = c.scene.depth_surface;
        const uint32_t fmt = dsurf ? rd32(dsurf) : 0;
        const bool has = dsurf && (fmt == 0x00022000u || fmt == 0x00066000u || fmt == 0x000EE000u || fmt == 0x01266000u ||
                                   rd32(dsurf + 12) != 0);
        const gvk::Stencil &f = c.stencil_front, &bk = c.two_sided ? c.stencil_back : c.stencil_front;
        fprintf(g_draws_json, ",\"st\":[%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]", has ? 1 : 0, f.func, f.fail, f.zfail,
                f.zpass, f.cmask, f.wmask, f.ref, bk.func, bk.fail, bk.zfail, bk.zpass, bk.cmask, bk.wmask, bk.ref);
    }
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
    uint32_t rw = 0, rh = 0; // tamanho do PNG do pack de texturas (0 = textura original)
    uint64_t tp_key = 0;      // PNG do pack sendo decodificado em segundo plano
    uint64_t mat_key = 0;     // material PBR da textura (pbr.h; 0 = sem material)
};

// Pack de texturas (vitart-data/texpack/*/index.tsv, montado por tools/texpack_build.py a partir de um
// pack do PCSX2): chave = FNV-1a 64 de (largura, altura) + os primeiros min(4096, nível base) bytes da
// textura do Vita; valor = PNG em HD. Os PNGs decodificados passam de ~1,5 GB -> os parados há mais de
// 10 s saem da memória (recarregam se voltarem à cena).
static std::unordered_map<uint64_t, std::string> g_texpack;
static uint64_t g_texpack_bytes = 0;
// Opção "HD Textures" do menu de pausa: desligada, as texturas voltam às do Vita (o cache é refeito).
static std::atomic<bool> g_texpack_on{true}, g_texcache_flush{false};
void texpack_set_enabled(bool on) {
    if (g_texpack_on.exchange(on) != on) g_texcache_flush = true;
}
static void texpack_load();
bool texpack_available() {
    texpack_load();
    return !g_texpack.empty();
}
static void texpack_load() {
    static bool done = false;
    if (done) return;
    done = true;
    if (getenv("VITART_NO_TEXPACK")) return;
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::path(data_path()) / "texpack";
    for (auto &dir : std::filesystem::directory_iterator(root, ec)) {
        std::ifstream f(dir.path() / "index.tsv");
        std::string line;
        size_t n = 0;
        while (std::getline(f, line)) {
            const size_t tab = line.find('\t');
            if (tab == std::string::npos) continue;
            g_texpack[std::strtoull(line.substr(0, tab).c_str(), nullptr, 16)] = (dir.path() / line.substr(tab + 1)).string();
            n++;
        }
        if (n) logf("vitart: pack de texturas %s: %zu texturas", dir.path().filename().string().c_str(), n);
    }
}
static uint64_t fnv64(const uint8_t *p, size_t n, uint64_t h = 0xCBF29CE484222325ull) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}
// Assinatura 16x16 (luminância pré-multiplicada pelo alfa, e o alfa) de uma imagem RGBA.
static std::vector<float> texsig(const uint8_t *px, uint32_t w, uint32_t h) {
    std::vector<float> v(512, 0.f), n(256, 0.f);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t *p = px + 4 * ((size_t)y * w + x);
            const int c = (int)(y * 16 / h) * 16 + (int)(x * 16 / w);
            const float a = p[3] / 255.f;
            v[c] += (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) * a, v[256 + c] += p[3], n[c] += 1.f;
        }
    for (int c = 0; c < 256; c++)
        if (n[c] > 0) v[c] /= n[c], v[256 + c] /= n[c];
    return v;
}
static float texsig_corr(const std::vector<float> &a, const std::vector<float> &b) {
    double ma = 0, mb = 0;
    for (size_t i = 0; i < a.size(); i++) ma += a[i], mb += b[i];
    ma /= a.size(), mb /= b.size();
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < a.size(); i++) sab += (a[i] - ma) * (b[i] - mb), saa += (a[i] - ma) * (a[i] - ma), sbb += (b[i] - mb) * (b[i] - mb);
    if (saa < 1e-6 && sbb < 1e-6) return 1.f; // as duas lisas
    return (float)(sab / std::sqrt(saa * sbb + 1e-9));
}
static std::unordered_map<uint64_t, bool> g_texpack_rejected; // chave -> PNG não parece a textura do Vita

// PNG do pack para esta textura, se houver e se for a mesma imagem (o Vita redesenhou algumas, como o
// HUD; o PS2 com o mesmo nome tinha outra arte e o PNG não encaixa). orig: a textura do Vita decodificada.
// Decodificação dos PNGs em segundo plano (até 3 ao mesmo tempo): no primeiro uso aparece a textura
// original e o HD entra quando fica pronto, sem engasgar a renderização ao entrar numa área nova.
struct TexpackJob {
    std::atomic<bool> done{false};
    std::shared_ptr<std::vector<uint8_t>> px;
    uint32_t w = 0, h = 0;
};
static std::unordered_map<uint64_t, std::shared_ptr<TexpackJob>> g_tp_jobs;
static std::mutex g_tp_sem_mutex;
static std::condition_variable g_tp_sem_cv;
static int g_tp_running = 0;
static uint64_t texpack_key(const gxmtex::Info &info, size_t base_bytes) {
    texpack_load();
    if (g_texpack.empty() || !base_bytes || !g_texpack_on) return 0;
    const uint32_t wh[2] = {info.width, info.height};
    const uint64_t key = fnv64(ptr<uint8_t>(info.data), std::min<size_t>(4096, base_bytes),
                               fnv64(reinterpret_cast<const uint8_t *>(wh), 8));
    // Com paleta (P8), a chave inclui a paleta inteira: variantes de cor da mesma imagem são texturas
    // diferentes (tools/texpack_match.py). Índices antigos, sem a paleta, continuam valendo.
    if (info.base == 0x95000000u && info.palette) {
        const uint64_t kp = fnv64(ptr<uint8_t>(info.palette), 1024, key);
        if (g_texpack.count(kp)) return g_texpack_rejected.count(kp) ? 0 : kp;
    }
    return g_texpack.count(key) && !g_texpack_rejected.count(key) ? key : 0;
}
static void texpack_start(uint64_t key) {
    if (g_tp_jobs.count(key)) return;
    auto job = std::make_shared<TexpackJob>();
    g_tp_jobs[key] = job;
    const std::string path = g_texpack[key];
    std::thread([job, path] {
        {
            std::unique_lock<std::mutex> l(g_tp_sem_mutex);
            g_tp_sem_cv.wait(l, [] { return g_tp_running < 3; });
            g_tp_running++;
        }
        int x = 0, y = 0, c = 0;
        if (stbi_uc *img = stbi_load(path.c_str(), &x, &y, &c, 4)) {
            job->px = std::make_shared<std::vector<uint8_t>>(img, img + (size_t)x * y * 4);
            job->w = (uint32_t)x, job->h = (uint32_t)y;
            stbi_image_free(img);
        } else {
            logf("vitart: pack de texturas: não abriu %s", path.c_str());
        }
        {
            std::lock_guard<std::mutex> l(g_tp_sem_mutex);
            g_tp_running--;
        }
        g_tp_sem_cv.notify_one();
        job->done = true;
    }).detach();
}
// PNG pronto para a chave: valida pelo conteúdo contra a textura original e devolve o HD (ou nulo).
static std::shared_ptr<std::vector<uint8_t>> texpack_take(uint64_t key, const gxmtex::Info &info,
                                                          const std::vector<uint8_t> &orig, uint32_t &w, uint32_t &h,
                                                          bool &finished) {
    finished = false;
    auto it = g_tp_jobs.find(key);
    if (it == g_tp_jobs.end()) return finished = true, nullptr;
    if (!it->second->done) return nullptr;
    auto job = it->second;
    g_tp_jobs.erase(it);
    finished = true;
    if (!job->px || orig.empty()) return nullptr;
    const std::vector<float> so = texsig(orig.data(), info.width, info.height), sp = texsig(job->px->data(), job->w, job->h);
    static int rejected = 0, logged = 0;
    // O pack é feito por fãs: o PNG pode ser bem diferente da textura original e entra assim mesmo (o
    // usuário quer a troca). Só um PNG vazio (todo transparente) no lugar de uma textura visível fica de
    // fora: apagaria o objeto.
    double ao = 0, ap = 0;
    for (int c = 0; c < 256; c++) ao += so[256 + c], ap += sp[256 + c];
    if (ap < 0.01 * ao) {
        g_texpack_rejected[key] = true;
        if (rejected++ < 8) logf("vitart: pack de texturas: %s recusado (PNG vazio)", g_texpack[key].c_str());
        return nullptr;
    }
    // Alfa na escala do Vita. O pack vem do PS2: lá 128 é opaco, e várias texturas usam o alfa para outra
    // coisa (valor uniforme, p. ex. 93, ou máscara de brilho), enquanto o shader do Vita lê o alfa como o
    // Vita o gravou. O PNG cru deixava inimigos meio transparentes e recortes onde o Vita não tem nenhum.
    //  - Vita com alfa uniforme: o PNG fica com esse mesmo alfa, constante;
    //  - desenho do alfa diferente (correlação < 0,55 nas 16x16 células, ou PNG sem recorte onde o Vita
    //    tem): o alfa vem da textura do Vita, ampliada (o recorte fica igual ao do jogo);
    //  - mesmo desenho: o alfa do PNG é escalado para a média do Vita (cortes e bordas em HD ficam).
    double mo = 0, mp = 0;
    for (int c = 0; c < 256; c++) mo += so[256 + c], mp += sp[256 + c];
    mo /= 256, mp /= 256;
    double vo = 0, vp = 0, cov = 0;
    for (int c = 0; c < 256; c++) {
        const double a = so[256 + c] - mo, b = sp[256 + c] - mp;
        vo += a * a, vp += b * b, cov += a * b;
    }
    const double sdo = std::sqrt(vo / 256), sdp = std::sqrt(vp / 256);
    const double corr = sdo > 1e-6 && sdp > 1e-6 ? cov / std::sqrt(vo * vp) : 0.0;
    uint8_t *px = job->px->data();
    const size_t npx = (size_t)job->w * job->h;
    static int fixed = 0;
    if (sdo < 6.0) {
        const uint8_t a = (uint8_t)std::lround(std::min(255.0, mo));
        for (size_t i = 0; i < npx; i++) px[4 * i + 3] = a;
        if (fixed++ < 8) logf("vitart: pack de texturas: alfa de %s = %u (uniforme como no Vita)", g_texpack[key].c_str(), a);
    } else if (sdp < 6.0 || corr < 0.55) {
        const uint32_t ow = info.width, oh = info.height;
        for (uint32_t y = 0; y < job->h; y++) {
            const float fy = std::max(0.f, (y + 0.5f) * oh / job->h - 0.5f);
            const uint32_t y0 = std::min((uint32_t)fy, oh - 1), y1 = std::min(y0 + 1, oh - 1);
            const float ty = fy - y0;
            for (uint32_t x = 0; x < job->w; x++) {
                const float fx = std::max(0.f, (x + 0.5f) * ow / job->w - 0.5f);
                const uint32_t x0 = std::min((uint32_t)fx, ow - 1), x1 = std::min(x0 + 1, ow - 1);
                const float tx = fx - x0;
                auto A = [&](uint32_t xx, uint32_t yy) { return (float)orig[4 * ((size_t)yy * ow + xx) + 3]; };
                const float a = (A(x0, y0) * (1 - tx) + A(x1, y0) * tx) * (1 - ty) + (A(x0, y1) * (1 - tx) + A(x1, y1) * tx) * ty;
                px[4 * ((size_t)y * job->w + x) + 3] = (uint8_t)(a + 0.5f);
            }
        }
        if (fixed++ < 8) logf("vitart: pack de texturas: alfa de %s vindo do Vita (correlação %.2f)", g_texpack[key].c_str(), corr);
    } else if (mp > 0.5 && std::fabs(mo / mp - 1.0) > 0.06) {
        const double k = mo / mp;
        for (size_t i = 3, n = job->px->size(); i < n; i += 4) px[i] = (uint8_t)std::min(255.0, px[i] * k + 0.5);
        if (fixed++ < 8) logf("vitart: pack de texturas: alfa de %s ajustado ao do Vita (x%.2f)", g_texpack[key].c_str(), k);
    }
    w = job->w, h = job->h;
    g_texpack_bytes += job->px->size();
    if (logged++ < 5) logf("vitart: textura trocada pelo pack (%ux%u -> %ux%u)", info.width, info.height, w, h);
    return job->px;
}
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
static uint64_t g_vk_draws = 0, g_vk_skipped = 0, g_pbr_draws = 0;
// Censo do que fica de fora (VITART_VK_CENSUS=1): primitivas por tipo e formatos de textura sem decodificador.
static std::map<std::string, uint64_t> g_census;
static bool census() {
    static const bool on = getenv("VITART_VK_CENSUS") != nullptr;
    return on;
}
// Tempo de CPU (ns) por etapa, zerado a cada relatório: onde o quadro gasta.
static std::atomic<uint64_t> g_t_draw{0}, g_t_gvk{0}, g_t_present{0}, g_t_idx{0}, g_t_attr{0}, g_t_tex{0};
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

// Nome do programa no cache: FNV-1a de 64 bits do binário (tools/usse_build_cache.py).
static std::string spv_name(uint32_t prog) {
    const uint32_t size = rd32(prog + 8);
    const uint8_t *b = ptr<uint8_t>(prog);
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < size; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    char name[64];
    std::snprintf(name, sizeof name, "%016llx", (unsigned long long)h);
    return name;
}
static std::shared_ptr<std::vector<uint32_t>> spv_read(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return nullptr;
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (raw.size() < 20) return nullptr;
    auto code = std::make_shared<std::vector<uint32_t>>(raw.size() / 4);
    std::memcpy(code->data(), raw.data(), code->size() * 4);
    return code;
}

static std::shared_ptr<std::vector<uint32_t>> spv_of(uint32_t prog) {
    auto it = g_spv.find(prog);
    if (it != g_spv.end()) return it->second;
    const uint32_t size = rd32(prog + 8);
    const uint8_t *b = ptr<uint8_t>(prog);
    const std::string name = spv_name(prog);
    std::shared_ptr<std::vector<uint32_t>> code = spv_read(g_spv_dir + "/" + name + ".spv");
    if (!code) {
        std::filesystem::create_directories(g_spv_dir + "/miss");
        std::ofstream o(g_spv_dir + "/miss/" + name + ".gxp", std::ios::binary);
        o.write(reinterpret_cast<const char *>(b), size);
        logf("vitart: programa 0x%08x sem SPIR-V no cache (%s)", prog, name.c_str());
    }
    return g_spv[prog] = code;
}

// ------------------------------------------------------------------ PBR (pbr.h, docs/pbr.md)
// Variante PBR do fragmento (<hash>.pbr.spv, gerada pelo usse_build_cache.py); nula se não existe.
static std::unordered_map<uint32_t, std::shared_ptr<std::vector<uint32_t>>> g_spv_pbr;
// VITART_PBR_AB=1 com VITART_VK_DUMP: depois de cada captura, 4 quadros sem PBR e o último também é gravado
// (comparar com e sem PBR na mesma cena, um quadro de distância).
static bool g_pbr_ab_off = false;
static std::shared_ptr<std::vector<uint32_t>> spv_pbr_of(uint32_t prog) {
    auto it = g_spv_pbr.find(prog);
    if (it != g_spv_pbr.end()) return it->second;
    auto code = spv_read(g_spv_dir + "/" + spv_name(prog) + ".pbr.spv");
    static int missing = 0;
    if (!code) {
        // o .spv normal pode ter vindo de uma captura que não existe mais: o programa vai para miss/ e a
        // próxima rodada do usse_build_cache.py gera a variante (sem textura difusa ele só o ignora)
        const std::string gxp = g_spv_dir + "/miss/" + spv_name(prog) + ".gxp";
        std::error_code ec;
        if (!std::filesystem::exists(gxp, ec)) {
            std::filesystem::create_directories(g_spv_dir + "/miss", ec);
            std::ofstream o(gxp, std::ios::binary);
            o.write(reinterpret_cast<const char *>(ptr<uint8_t>(prog)), rd32(prog + 8));
        }
        if (missing++ < 3)
            logf("vitart: PBR: sem %s.pbr.spv no cache (rode tools/usse_build_cache.py de novo)", spv_name(prog).c_str());
    }
    return g_spv_pbr[prog] = code;
}
// O que o PBR lê dos parâmetros de um programa: o recurso da textura difusa (fragmento) e as palavras
// de g_Projection e das luzes no default uniform buffer (vértice, contêiner 14).
struct PbrProgInfo {
    int diffuse = -1;
    pbr::VertexInfo vi;
};
static const PbrProgInfo &pbr_prog(uint32_t prog) {
    static std::unordered_map<uint32_t, PbrProgInfo> cache;
    auto it = cache.find(prog);
    if (it != cache.end()) return it->second;
    PbrProgInfo info;
    const uint32_t n = rd32(prog + 0x24);
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t p = gxp_param(prog, i);
        const char *name = gstr(p + rd32(p));
        const uint32_t cat = *ptr<uint8_t>(p + 4) & 15, cont = *ptr<uint8_t>(p + 5) >> 4;
        const int res = (int)rd32(p + 12);
        if (cat == 2 && !std::strcmp(name, "sTextureSampler")) info.diffuse = res;
        if (cat != 1 || cont != 14) continue;
        if (!std::strcmp(name, "g_Projection")) info.vi.proj = res;
        else if (!std::strcmp(name, "g_LightDir")) info.vi.light_dir = res;
        else if (!std::strcmp(name, "g_LightColor")) info.vi.light_color = res;
        else if (!std::strcmp(name, "g_Ambient")) info.vi.ambient = res;
    }
    return cache[prog] = info;
}
// O PBR só entra em superfícies opacas (sem blend). Qualquer desenho com blend fica com a cor original:
// aditivo e multiplicativo (fogo, brilho, sombras pintadas) e também o alfa comum, que no GoW é névoa,
// fumaça e camadas de chuva; com PBR elas perdiam a névoa e ganhavam brilho onde não há superfície.
static bool pbr_blend_ok(const gvk::Draw &d) {
    return !d.has_blend || ((d.blend >> 8) & 15) == 0;
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
        if (census()) g_census["sem shader"]++;
        return;
    }
    const uint64_t t_idx0 = now_ns();
    const uint32_t kind = prim >> 26; // 0 triângulos, 3 faixa, 4 leque (linhas e pontos ficam de fora)
    const uint32_t isz = (itype & 0x01000000u) ? 4 : 2;
    // Os índices do jogo vão para a GPU como estão (16 ou 32 bits), com a topologia nativa (lista, faixa,
    // leque); só a faixa de vértices usada é enviada e o deslocamento vira o vertexOffset do desenho.
    if (census()) g_census["primitiva " + std::to_string(kind) + (kind == 0 || kind == 3 || kind == 4 ? "" : " (pulada)")]++;
    if (kind != 0 && kind != 3 && kind != 4) return;
    const uint32_t nidx = kind == 0 ? icount / 3 * 3 : icount;
    if (nidx < 3) return;
    uint32_t maxi = 0, mini = UINT32_MAX;
    if (isz == 4) {
        const uint32_t *ix = ptr<uint32_t>(idata);
        for (uint32_t i = 0; i < nidx; i++) maxi = std::max(maxi, ix[i]), mini = std::min(mini, ix[i]);
    } else {
        const uint16_t *ix = ptr<uint16_t>(idata);
        uint32_t lo = 0xFFFF, hi = 0;
        for (uint32_t i = 0; i < nidx; i++) hi = std::max<uint32_t>(hi, ix[i]), lo = std::min<uint32_t>(lo, ix[i]);
        maxi = hi, mini = lo;
    }
    maxi -= mini;
    if (maxi > (1u << 20)) return;
    const uint32_t nv = maxi + 1;

    const uint64_t t_attr0 = now_ns();
    g_t_idx += t_attr0 - t_idx0;
    // Atributos: location k = k-ésimo parâmetro de entrada (categoria 0), ligado pelo índice de recurso.
    std::vector<uint32_t> locs;
    const uint32_t nparams = rd32(vp->program + 0x24);
    for (uint32_t i = 0; i < nparams; i++) {
        const uint32_t p = gxp_param(vp->program, i);
        if ((*ptr<uint8_t>(p + 4) & 15) == 0) locs.push_back(rd32(p + 12));
    }
    // Caminho cru: os streams vão como estão e a GPU converte (formatos nativos de vértice). Só se cada
    // location tem atributo com formato suportado; senão, conversão para vec4 na CPU como antes.
    std::vector<gvk::VertexStream> raw_streams;
    std::vector<gvk::VertexAttr> raw_attrs;
    {
        static const bool no_raw = getenv("VITART_VK_CPU_ATTRS") != nullptr;
        bool ok = !no_raw && !locs.empty();
        int binding_of[16];
        uint32_t end_of[16] = {};
        std::fill(std::begin(binding_of), std::end(binding_of), -1);
        for (size_t k = 0; ok && k < locs.size(); k++) {
            bool found = false;
            for (uint32_t a = 0; a < vp->attr_count; a++) {
                const uint8_t *d = &vp->attributes[8 * a];
                const uint32_t st = d[0] | (d[1] << 8), off = d[2] | (d[3] << 8), fmt = d[4], comps = d[5], reg = d[6] | (d[7] << 8);
                if (reg != locs[k] || st >= 16 || !c.streams[st]) continue;
                const uint32_t nc = comps < 4 ? comps : 4;
                if (!gvk::vertex_format_ok(fmt, nc)) break;
                if (binding_of[st] < 0) {
                    binding_of[st] = (int)raw_streams.size();
                    gvk::VertexStream vs_;
                    vs_.stride = *reinterpret_cast<const uint16_t *>(&vp->streams[4 * st]);
                    vs_.data = ptr<uint8_t>(c.streams[st] + mini * vs_.stride);
                    raw_streams.push_back(vs_);
                }
                end_of[st] = std::max(end_of[st], off + attr_size(fmt) * nc);
                gvk::VertexAttr va;
                va.location = (uint32_t)k, va.stream = (uint32_t)binding_of[st], va.offset = off, va.fmt = fmt, va.comps = nc;
                raw_attrs.push_back(va);
                found = true;
                break;
            }
            ok = found;
        }
        if (ok) {
            for (uint32_t st = 0; st < 16; st++)
                if (binding_of[st] >= 0) {
                    gvk::VertexStream &vs_ = raw_streams[binding_of[st]];
                    vs_.size = (nv - 1) * vs_.stride + end_of[st];
                }
        } else {
            raw_streams.clear(), raw_attrs.clear();
        }
    }
    std::vector<float> attrs(raw_attrs.empty() ? locs.size() * nv * 4 : 0, 0.0f);
    for (size_t k = 0; raw_attrs.empty() && k < locs.size(); k++)
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

    const uint64_t t_tex0 = now_ns();
    g_t_attr += t_tex0 - t_attr0;
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
    // Stencil só existe se a superfície tem o plano S8 (formato com S8 ou ponteiro de stencil).
    if (c.scene.depth_surface) {
        const uint32_t fmt = rd32(c.scene.depth_surface);
        d.stencil = fmt == 0x00022000u || fmt == 0x00066000u || fmt == 0x000EE000u || fmt == 0x01266000u ||
                    rd32(c.scene.depth_surface + 12) != 0;
    }
    d.front = c.stencil_front;
    d.back = c.two_sided ? c.stencil_back : c.stencil_front;
    d.vertex_count = nv;
    d.attr_count = (uint32_t)locs.size();
    d.attrs = attrs.data();
    d.raw_streams = std::move(raw_streams);
    d.raw_attrs = std::move(raw_attrs);
    d.index_data = ptr<void>(idata);
    d.index_size = isz;
    d.vertex_offset = -(int32_t)mini;
    d.topology = kind == 3 ? 1 : kind == 4 ? 2 : 0;
    d.index_count = nidx;
    const uint32_t vsize = rd32(vp->program + 0x64) * 4, fsize = rd32(fp->program + 0x64) * 4;
    if (c.vuniform && vsize) d.vert_uniforms = ptr<uint8_t>(c.vuniform), d.vert_uniforms_size = vsize;
    if (c.funiform && fsize) d.frag_uniforms = ptr<uint8_t>(c.funiform), d.frag_uniforms_size = fsize;
    d.vs = vs->data(), d.vs_size = (uint32_t)vs->size() * 4;
    d.fs = fs->data(), d.fs_size = (uint32_t)fs->size() * 4;
    std::vector<std::shared_ptr<std::vector<uint8_t>>> keep;
    const PbrProgInfo &pinfo_f = pbr_prog(fp->program);
    int diffuse_idx = -1;      // posição da textura difusa em d.textures (só textura comum, não alvo)
    uint64_t diffuse_key = 0;  // material PBR dela
    for (uint32_t i = 0; i < 16; i++) {
        if (!c.textures[i]) continue;
        std::array<uint32_t, 4> w;
        std::memcpy(w.data(), ptr<uint32_t>(c.textures[i]), 16);
        const gxmtex::Info info = gxmtex::describe(w.data());
        if (census()) g_census["textura tipo " + std::to_string(info.type) + " base " + std::to_string(info.base >> 24)]++;
        gvk::Texture t;
        t.binding = i;
        // Palavra 0: endereçamento v em 3..5, u em 6..8, filtro de minificação em 10..11, de magnificação em 12..13.
        t.addr_v = (uint8_t)((w[0] >> 3) & 7), t.addr_u = (uint8_t)((w[0] >> 6) & 7);
        t.min = (uint8_t)((w[0] >> 10) & 3), t.mag = (uint8_t)((w[0] >> 12) & 3);
        // mip_filter no bit 9, número de níveis em 17..20, lod bias em 21..26.
        t.mip_filter = (uint8_t)((w[0] >> 9) & 1), t.mip_count = (uint8_t)((w[0] >> 17) & 15);
        t.lod_bias = (uint8_t)((w[0] >> 21) & 63);
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
                    const bool ok = n && gxmtex::decode(ptr<uint8_t>(0), info, *px);
                    e.rw = e.rh = 0, e.tp_key = 0;
                    if (ok)
                        if (const uint64_t k = texpack_key(info, n)) texpack_start(k), e.tp_key = k; // HD em segundo plano
                    e.mat_key = ok ? pbr::key_for(ptr<uint8_t>(info.data), n, info.width, info.height,
                                                  info.base == 0x95000000u && info.palette ? ptr<uint8_t>(info.palette) : nullptr)
                                   : 0;
                    if (!ok) {
                        px.reset();
                        if (census()) {
                            char k[64];
                            std::snprintf(k, sizeof k, "textura sem decodificador: base 0x%08x tipo %u", info.base, info.type);
                            g_census[k]++;
                        }
                    }
                    e.px = px;
                    e.hash = h;
                }
            }
            if (e.tp_key && e.px) {
                uint32_t rw = 0, rh = 0;
                bool finished = false;
                if (auto hd = texpack_take(e.tp_key, info, *e.px, rw, rh, finished)) e.px = hd, e.rw = rw, e.rh = rh;
                if (finished) e.tp_key = 0;
            }
            if (!e.px) continue;
            if ((int)i == pinfo_f.diffuse) diffuse_idx = (int)d.textures.size(), diffuse_key = e.mat_key;
            keep.push_back(e.px);
            t.rgba = e.px->data();
            t.key = (e.rw ? e.hash ^ 0x9E3779B97F4A7C15ull : e.hash) | 1; // HD do pack é outra textura na GPU
            if (e.rw) {
                // HD do pack: tamanho do PNG e cadeia de mipmaps completa (sem cintilar de longe).
                t.width = e.rw, t.height = e.rh;
                uint32_t levels = 0;
                while ((std::max(e.rw, e.rh) >> (levels + 1)) > 0) levels++;
                t.mip_count = (uint8_t)std::min<uint32_t>(levels, 15), t.mip_filter = 1;
            }
        }
        d.textures.push_back(t);
    }
    if (c.vuniform && pbr::config().post)
        d.view_inv_ok = pbr::inv_projection(pbr_prog(vp->program).vi, ptr<float>(c.vuniform), vsize / 4, d.view_inv);
    // PBR: troca o fragmento pela variante PBR quando a textura difusa tem material, o vértice tem
    // projeção em perspectiva e a superfície é opaca.
    if (diffuse_idx >= 0 && c.vuniform && pbr::enabled() && !g_pbr_ab_off) {
        const uint32_t frame = g_present_frame.load();
        std::shared_ptr<const pbr::Material> mat = pbr::material(diffuse_key, frame);
        if (!mat && pbr::config().all_surfaces) mat = pbr::neutral();
        std::shared_ptr<std::vector<uint32_t>> pfs;
        if (mat && pbr_blend_ok(d) && (pfs = spv_pbr_of(fp->program)) &&
            pbr::params(pbr_prog(vp->program).vi, ptr<float>(c.vuniform), vsize / 4, frame, d.pbr_params)) {
            d.pbr = true;
            d.fs = pfs->data(), d.fs_size = (uint32_t)pfs->size() * 4;
            const gvk::Texture &dt = d.textures[diffuse_idx];
            for (int k = 0; k < 2; k++) {
                gvk::Texture &m = d.pbr_maps[k];
                const auto &px = k ? mat->orm : mat->normal;
                m = dt; // mesma amostragem (endereçamento e filtro) da difusa
                m.target_addr = 0, m.depth = false;
                m.width = k ? mat->ow : mat->nw, m.height = k ? mat->oh : mat->nh;
                m.rgba = px->data();
                m.key = ((mat->key ^ (k ? 0x4F524D0000000000ull : 0x4E524D0000000000ull)) * 0x9E3779B97F4A7C15ull) | 1;
                m.mip_count = 1, m.mip_filter = 1, m.lod_bias = 31;
                keep.push_back(px);
            }
            g_pbr_draws++;
        }
    }
    const uint64_t tg = now_ns();
    g_t_tex += tg - t_tex0;
    const bool drew = gvk::draw(d);
    g_t_gvk += now_ns() - tg;
    if (!drew)
        logf("vitart: desenho Vulkan falhou: %s", gvk::last_error());
    else
        g_vk_draws++;
}

void game_options_tick();
// Travadas: quadro com intervalo > 25 ms (depois do carregamento) vai para o log com o que aconteceu
// nele -- texturas e pipelines novos, tempos de criação --, e a cada 10 s sai um resumo do ritmo.
static void hitch_tick() {
    using clk = std::chrono::steady_clock;
    static clk::time_point last = clk::now(), window = clk::now();
    static gvk::Stats prev{};
    static uint32_t frames = 0, wframes = 0, slow = 0, logged = 0;
    static double worst = 0;
    const auto now = clk::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    const gvk::Stats st = gvk::peek_stats();
    auto d64 = [](uint64_t a, uint64_t b) { return a >= b ? a - b : a; };
    auto d32 = [](uint32_t a, uint32_t b) { return a >= b ? a - b : a; };
    frames++, wframes++;
    if (frames > 300 && ms > 25.0) {
        slow++;
        worst = ms > worst ? ms : worst;
        if (logged++ < 400)
            logf("vitart: travada %.1f ms (quadro %u): texturas novas %u (%.1f ms), pipelines %u (%.1f ms), cópias %u, "
                 "espera GPU %.1f ms",
                 ms, g_present_frame.load(), d32(st.uploads, prev.uploads), d64(st.alloc_ns, prev.alloc_ns) / 1e6,
                 d32(st.pipes, prev.pipes), d64(st.pipe_ns, prev.pipe_ns) / 1e6, d32(st.snapshots, prev.snapshots),
                 d64(st.wait_ns, prev.wait_ns) / 1e6);
    }
    prev = st;
    if (now - window > std::chrono::seconds(10)) {
        if (frames > 300) logf("vitart: ritmo: %u quadros em 10 s, %u acima de 25 ms (pior %.1f ms)", wframes, slow, worst);
        static uint64_t pbr_prev = 0, draws_prev = 0;
        if (g_pbr_draws != pbr_prev)
            logf("vitart: PBR em %llu de %llu desenhos nos últimos 10 s", (unsigned long long)(g_pbr_draws - pbr_prev),
                 (unsigned long long)(g_vk_draws - draws_prev));
        pbr_prev = g_pbr_draws, draws_prev = g_vk_draws;
        window = now, slow = 0, worst = 0, wframes = 0;
    }
}

void gxm_present(uint32_t fb) {
    if (g_vk != 1) return;
    hitch_tick();
    game_options_tick(); // opções do PC no menu de pausa do jogo (game_options.cpp)
    if (g_texcache_flush.exchange(false)) {
        const std::lock_guard<std::mutex> lock(g_vk_mutex);
        g_texcache.clear(); // tudo é decodificado de novo no próximo uso, com ou sem o pack
        g_texpack_bytes = 0;
        logf("vitart: texturas HD %s", g_texpack_on ? "ligadas" : "desligadas");
    }
    {
        const std::lock_guard<std::mutex> lock(g_vk_mutex);
        pbr::tick(g_present_frame.load()); // pbr.ini ao vivo e mapas parados fora da memória
    }
    if (g_texpack_bytes > (1536ull << 20)) {
        const std::lock_guard<std::mutex> lock(g_vk_mutex);
        const uint32_t now = g_present_frame.load();
        for (auto &[k, e] : g_texcache)
            if (e.rw && e.px && now - e.checked > 300) {
                g_texpack_bytes -= std::min<uint64_t>(g_texpack_bytes, e.px->size());
                e.px.reset();
            }
    }
    static uint32_t frame = 0;
    static const uint32_t dump_at = getenv("VITART_VK_DUMP") ? (uint32_t)std::atoi(getenv("VITART_VK_DUMP")) : 0;
    static const bool pbr_ab = getenv("VITART_PBR_AB") != nullptr;
    // a leitura do alvo pode ver um quadro anterior (quadros em voo): o PBR fica desligado por 4 quadros
    // e a captura sem ele é a do último
    static int ab_left = 0;
    const bool ab_dump = ab_left > 0 && --ab_left == 0;
    if (ab_dump) g_pbr_ab_off = false, gvk::set_post_bypass(false);
    if (++frame == dump_at || ab_dump || (dump_at && frame % (dump_at < 300 ? dump_at : 300) == 0)) {
        if (pbr_ab && !ab_dump) g_pbr_ab_off = true, ab_left = 4, gvk::set_post_bypass(true);
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
    if (!gvk::is_target(fb)) {
        static int warned = 0;
        if (warned++ < 3) logf("vitart: apresentar 0x%08x: não é alvo de nenhuma cena", fb);
    }
    const uint64_t tp = now_ns();
    gvk::present(fb);
    g_t_present += now_ns() - tp;
    if (census() && frame % 600 == 0) {
        for (auto &[k, n] : g_census) logf("vitart: censo: %s: %llu", k.c_str(), (unsigned long long)n);
    }
    if (frame % 120 == 0) {
        logf("vitart: tempo por quadro (média de 120): vk_draw %.1f ms (gvk::draw %.1f ms), apresentar %.1f ms",
             g_t_draw.exchange(0) / 120e6, g_t_gvk.exchange(0) / 120e6, g_t_present.exchange(0) / 120e6);
        logf("vitart:   hle por quadro: índices %.1f ms, atributos %.1f ms, estado+texturas %.1f ms",
             g_t_idx.exchange(0) / 120e6, g_t_attr.exchange(0) / 120e6, g_t_tex.exchange(0) / 120e6);
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
    bool fresh = false;
    {
        const std::lock_guard<std::mutex> lock(g_gxm_mutex);
        uint32_t &id = g_id_of_program[prog];
        if (!id) {
            id = new_object(0x10, "gxm:program-id");
            g_program_ids[id] = prog;
            fresh = true;
        }
        wr32(id, prog);
        wr32(arg(ctx, 2), id);
    }
    // SPIR-V carregado no registro (no carregamento da fase, não no 1º desenho); sem cache, o .gxp vai
    // para spv/miss já aqui, então uma passagem pelo boot colhe o conjunto inteiro de shaders.
    if (fresh && vk_on()) {
        const std::lock_guard<std::mutex> lock(g_vk_mutex);
        g_spv.erase(prog); // endereço reaproveitado por outro programa
        spv_of(prog);
    }
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
    // Como o patcher real: a mesma combinação devolve o mesmo programa (o jogo pede a cada uso, então
    // a chave é montada direto da memória do guest e o objeto só é criado na falta).
    const uint32_t program = rd32(id), attr_count = arg(ctx, 3), stream_count = arg(ctx, 5);
    thread_local std::string key;
    key.assign(reinterpret_cast<const char *>(&program), 4);
    if (arg(ctx, 2)) key.append(ptr<char>(arg(ctx, 2)), 8 * attr_count);
    key.push_back('|');
    if (arg(ctx, 4)) key.append(ptr<char>(arg(ctx, 4)), 4 * stream_count);
    const std::lock_guard<std::mutex> lock(g_gxm_mutex);
    uint32_t &h = g_vprog_cache[key];
    if (!h) {
        auto vp = std::make_shared<VertexProgram>();
        vp->program = program;
        vp->attr_count = attr_count;
        vp->stream_count = stream_count;
        if (arg(ctx, 2)) vp->attributes.assign(ptr<uint8_t>(arg(ctx, 2)), ptr<uint8_t>(arg(ctx, 2)) + 8 * attr_count);
        if (arg(ctx, 4)) vp->streams.assign(ptr<uint8_t>(arg(ctx, 4)), ptr<uint8_t>(arg(ctx, 4)) + 4 * stream_count);
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
    const uint32_t f[4] = {rd32(id), arg(ctx, 2), arg(ctx, 3), arg(ctx, 5) ? rd32(arg(ctx, 5)) : 0};
    thread_local std::string key;
    key.assign(reinterpret_cast<const char *>(f), sizeof f);
    if (arg(ctx, 4)) key.append(ptr<char>(arg(ctx, 4)), 4); // SceGxmBlendInfo: 4 bytes
    const std::lock_guard<std::mutex> lock(g_gxm_mutex);
    uint32_t &h = g_fprog_cache[key];
    if (!h) {
        auto fp = std::make_shared<FragmentProgram>();
        fp->program = f[0];
        fp->output_format = f[1];
        fp->msaa = f[2];
        if (arg(ctx, 4)) fp->blend.assign(ptr<uint8_t>(arg(ctx, 4)), ptr<uint8_t>(arg(ctx, 4)) + 4);
        fp->vertex_program = f[3];
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
VT_HLE(sceGxmTextureSetMipFilter) {
    if (tex::type(arg(ctx, 0)) != 0xC0000000u) tex::set(tex::w(arg(ctx, 0), 0), 9, 1, arg(ctx, 1) ? 1 : 0);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmTextureSetMipmapCount) {
    // mip_count guarda níveis - 1 (até 13 níveis).
    const uint32_t n = arg(ctx, 1);
    if (n > 13) return ret(ctx, GXM_ERROR_INVALID_VALUE);
    if (tex::type(arg(ctx, 0)) != 0xC0000000u) tex::set(tex::w(arg(ctx, 0), 0), 17, 4, n ? n - 1 : 0);
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmTextureSetLodMin) {
    // lod_min: 2 bits baixos na palavra 2 (bits 0..1), 2 altos na palavra 3 (bits 26..27).
    const uint32_t v = arg(ctx, 1);
    if (tex::type(arg(ctx, 0)) != 0xC0000000u) {
        tex::set(tex::w(arg(ctx, 0), 2), 0, 2, v & 3);
        tex::set(tex::w(arg(ctx, 0), 3), 26, 2, v >> 2);
    }
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
    static const bool trace_uniforms = std::getenv("VITART_TRACE_UNIFORMS") != nullptr; // getenv é caro: 1x
    if (trace_uniforms) {
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
// (context, func, stencilFail, depthFail, depthPass, compareMask, writeMask): func em bits 25..27, as
// operações e as funções na mesma ordem do Vulkan.
static void set_stencil(arc_ctx *ctx, bool front) {
    auto c = context_of(arg(ctx, 0));
    if (!c) return;
    gvk::Stencil &st = front ? c->stencil_front : c->stencil_back;
    st.func = (uint8_t)((arg(ctx, 1) >> 25) & 7);
    st.fail = (uint8_t)(arg(ctx, 2) & 7);
    st.zfail = (uint8_t)(arg(ctx, 3) & 7);
    st.zpass = (uint8_t)(arg(ctx, 4) & 7);
    st.cmask = (uint8_t)arg(ctx, 5);
    st.wmask = (uint8_t)arg(ctx, 6);
}
VT_HLE(sceGxmSetFrontStencilFunc) { set_stencil(ctx, true); }
VT_HLE(sceGxmSetBackStencilFunc) { set_stencil(ctx, false); }
VT_HLE(sceGxmSetFrontStencilRef) { if (auto c = context_of(arg(ctx, 0))) c->stencil_front.ref = (uint8_t)arg(ctx, 1); }
VT_HLE(sceGxmSetBackStencilRef) { if (auto c = context_of(arg(ctx, 0))) c->stencil_back.ref = (uint8_t)arg(ctx, 1); }
VT_HLE(sceGxmSetFrontPolygonMode) {}
VT_HLE(sceGxmSetUserMarker) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmPushUserMarker) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmPopUserMarker) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmPadHeartbeat) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmSetRegionClip) {}
VT_HLE(sceGxmSetFrontPointLineWidth) {}
VT_HLE(sceGxmFinish) { ret(ctx, SCE_OK); }
VT_HLE(sceGxmGetNotificationRegion) {
    // Região de notificações (palavras que a GPU escreve ao terminar): memória do guest zerada.
    static const uint32_t region = new_object(0x1000, "gxm:notification-region");
    ret(ctx, region);
}
VT_HLE(sceGxmProgramParameterGetIndex) {
    const uint32_t prog = arg(ctx, 0), param = arg(ctx, 1);
    ret(ctx, prog && param ? (param - gxp_param(prog, 0)) / 16 : 0);
}
// Transferências 2D: o resultado não é desenhado (os alvos vivem na GPU); a notificação é escrita para
// o jogo não esperar para sempre. Registro do uso para saber se fazem falta.
static void transfer_notify(uint32_t note, const char *what) {
    static std::atomic<int> n{0};
    if (n++ < 4) logf("vitart: %s (sem efeito; notificação 0x%08x)", what, note);
    if (note && rd32(note)) wr32(rd32(note), rd32(note + 4)); // SceGxmNotification {address, value}
}
VT_HLE(sceGxmTransferCopy) { transfer_notify(arg(ctx, 19), "sceGxmTransferCopy"); ret(ctx, SCE_OK); }
VT_HLE(sceGxmTransferDownscale) { transfer_notify(arg(ctx, 14), "sceGxmTransferDownscale"); ret(ctx, SCE_OK); }

} // namespace vt
