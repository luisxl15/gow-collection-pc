// PBR no vitart (ver pbr.h e docs/pbr.md).
#include "pbr.h"
#include "vitart.h"

#include "stb_image.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace vt {
int game_id(); // game_options.cpp
}

namespace vt::pbr {
void drop_all();

namespace {

// ------------------------------------------------------------------ pbr.ini
Config g_cfg;
std::filesystem::file_time_type g_ini_time{};
bool g_ini_read = false;
uint32_t g_ini_checked = 0;

std::string ini_path() { return data_path() + "/pbr.ini"; }

void write_default_ini() {
    if (FILE *f = fopen(ini_path().c_str(), "w")) {
        fprintf(f,
                "# PBR do vitart (docs/pbr.md). O arquivo é relido com o jogo rodando: salve e veja na hora.\n"
                "enabled=0\n"
                "# 1 = também nas texturas sem material (material neutro, rugosidade default_roughness)\n"
                "all_surfaces=0\n"
                "default_roughness=0.8\n"
                "# 1 = normal maps no padrão OpenGL (verde para cima); o gerador grava no padrão DirectX\n"
                "normal_opengl=0\n"
                "normal_strength=1.0\n"
                "specular=1.0\n"
                "env_specular=0.5\n"
                "roughness_scale=0.75\n"
                "metallic_scale=1.0\n"
                "ao_strength=1.0\n"
                "# luz-chave de relevo: a luz do cenário do GoW vem assada nos vértices, e sem ela o normal map\n"
                "# quase não aparece. relief = força do relevo (0 = só as luzes do jogo), key_spec = brilho,\n"
                "# key_dir = direção para a luz em espaço de câmera\n"
                "relief=0.8\n"
                "key_spec=0.5\n"
                "key_dir=-0.35 0.75 0.55\n"
                "# iluminação de tela, antes do HUD: post=0 desliga tudo. ssao = força da oclusão de ambiente,\n"
                "# ssao_radius em unidades do jogo, bloom = intensidade, bloom_threshold = limiar (linear),\n"
                "# exposure, tonemap = força da curva fílmica (0 = cor do jogo), saturation, vignette\n"
                "post=0\n"
                "ssao=1.6\n"
                "ssao_radius=40\n"
                "bloom=0.35\n"
                "bloom_threshold=0.7\n"
                "exposure=1.08\n"
                "tonemap=0.6\n"
                "saturation=1.08\n"
                "vignette=0.15\n"
                "# profundidade de campo: dof = força (0 desliga), dof_range = faixa nítida em volta do foco\n"
                "# (fração da distância), dof_near = desfocar também o que está perto, focus_speed = adaptação\n"
                "dof=0.65\n"
                "dof_range=0.6\n"
                "dof_near=0\n"
                "focus_speed=0.08\n"
                "# raios de luz (da direção da luz do jogo, onde o céu aparece), granulação, aberração cromática,\n"
                "# nitidez e color grading (lift = sombras, gain = altas luzes, contrast em torno do cinza médio)\n"
                "god_rays=0.6\n"
                "grain=0.03\n"
                "chromatic_aberration=0.004\n"
                "sharpen=0.4\n"
                "grade=1.0\n"
                "contrast=1.08\n"
                "lift=0.004 0.008 0.016\n"
                "gain=1.04 1.0 0.95\n"
                "# teto da luz assada extraída do jogo (cor original / textura)\n"
                "baked_max=4.0\n"
                "# cores lineares do ambiente refletido e o 'para cima' em espaço de câmera\n"
                "sky=0.55 0.60 0.70\n"
                "ground=0.22 0.20 0.18\n"
                "up=0 1 0\n"
                "# luz usada quando nenhum desenho iluminado do jogo passou há pouco (espaço de câmera)\n"
                "fallback_dir=-0.35 0.75 0.55\n"
                "fallback_color=0.65 0.62 0.58\n"
                "# depuração: 0 desligada, 1 normal, 2 luz assada, 3 rugosidade, 4 metal, 5 oclusão, 6 posição,\n"
                "# 7 detalhe do normal map\n"
                "debug=0\n");
        fclose(f);
    }
}

void read_vec(const char *s, float *v) {
    float a, b, c;
    if (std::sscanf(s, "%f %f %f", &a, &b, &c) == 3) v[0] = a, v[1] = b, v[2] = c;
}

void load_ini() {
    Config c;
    FILE *f = fopen(ini_path().c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        char *eq = std::strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const std::string k = line;
        const char *v = eq + 1;
        const float x = (float)std::atof(v);
        if (k == "enabled") c.enabled = x != 0;
        else if (k == "all_surfaces") c.all_surfaces = x != 0;
        else if (k == "normal_opengl") c.normal_opengl = x != 0;
        else if (k == "normal_strength") c.normal_strength = x;
        else if (k == "specular") c.specular = x;
        else if (k == "env_specular") c.env_specular = x;
        else if (k == "baked_max") c.baked_max = x;
        else if (k == "roughness_scale") c.roughness_scale = x;
        else if (k == "metallic_scale") c.metallic_scale = x;
        else if (k == "ao_strength") c.ao_strength = x;
        else if (k == "default_roughness") c.default_roughness = x;
        else if (k == "relief") c.relief = x;
        else if (k == "key_spec") c.key_spec = x;
        else if (k == "key_dir") read_vec(v, c.key_dir);
        else if (k == "post") c.post = x != 0;
        else if (k == "ssao") c.ssao = x;
        else if (k == "ssao_radius") c.ssao_radius = x;
        else if (k == "bloom") c.bloom = x;
        else if (k == "bloom_threshold") c.bloom_threshold = x;
        else if (k == "exposure") c.exposure = x;
        else if (k == "tonemap") c.tonemap = x;
        else if (k == "saturation") c.saturation = x;
        else if (k == "vignette") c.vignette = x;
        else if (k == "dof") c.dof = x;
        else if (k == "dof_range") c.dof_range = x;
        else if (k == "dof_near") c.dof_near = x != 0;
        else if (k == "focus_speed") c.focus_speed = x;
        else if (k == "god_rays") c.shafts = x;
        else if (k == "grain") c.grain = x;
        else if (k == "chromatic_aberration") c.chroma = x;
        else if (k == "sharpen") c.sharpen = x;
        else if (k == "grade") c.grade = x;
        else if (k == "post_split") c.post_split = x != 0;
        else if (k == "post_debug") c.post_debug = std::atoi(v);
        else if (k == "contrast") c.contrast = x;
        else if (k == "lift") read_vec(v, c.lift);
        else if (k == "gain") read_vec(v, c.gain);
        else if (k == "debug") c.debug = std::atoi(v);
        else if (k == "sky") read_vec(v, c.sky);
        else if (k == "ground") read_vec(v, c.ground);
        else if (k == "up") read_vec(v, c.up);
        else if (k == "fallback_dir") read_vec(v, c.fallback_dir);
        else if (k == "fallback_color") read_vec(v, c.fallback_color);
    }
    fclose(f);
    const bool flip_changed = c.normal_opengl != g_cfg.normal_opengl;
    g_cfg = c;
    if (flip_changed) drop_all(); // os normal maps já carregados foram convertidos com a convenção anterior
    logf("vitart: PBR %s (pbr.ini lido%s)", c.enabled ? "ligado" : "desligado", c.debug ? ", modo de depuração" : "");
}

void check_ini() {
    std::error_code ec;
    const auto path = ini_path();
    if (!std::filesystem::exists(path, ec)) write_default_ini();
    const auto t = std::filesystem::last_write_time(path, ec);
    if (ec) return;
    if (!g_ini_read || t != g_ini_time) {
        g_ini_time = t, g_ini_read = true;
        load_ini();
    }
}

// ------------------------------------------------------------------ índice de materiais
struct Files {
    std::string normal, orm;
};
std::unordered_map<uint64_t, Files> g_index;

void index_load() {
    static bool done = false;
    if (done) return;
    const int game = vt::game_id(); // só os materiais do jogo em execução (como o pack de texturas)
    if (!game) return;
    done = true;
    const std::string want = game == 1 ? "gow1" : "gow2";
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::path(data_path()) / "pbr";
    for (auto &dir : std::filesystem::directory_iterator(root, ec)) {
        if (dir.path().filename().string() != want) continue;
        std::ifstream f(dir.path() / "index.tsv");
        std::string line;
        size_t n = 0;
        while (std::getline(f, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            const size_t t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const size_t t2 = line.find('\t', t1 + 1);
            const std::string nrm = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
            const std::string orm = t2 == std::string::npos ? "-" : line.substr(t2 + 1);
            Files e;
            if (nrm != "-" && !nrm.empty()) e.normal = (dir.path() / nrm).string();
            if (orm != "-" && !orm.empty()) e.orm = (dir.path() / orm).string();
            if (e.normal.empty() && e.orm.empty()) continue;
            g_index[std::strtoull(line.substr(0, t1).c_str(), nullptr, 16)] = e;
            n++;
        }
        if (n) logf("vitart: materiais PBR %s: %zu texturas", dir.path().filename().string().c_str(), n);
    }
}

// ------------------------------------------------------------------ carga em segundo plano
struct Job {
    std::atomic<bool> done{false};
    std::shared_ptr<Material> mat;
};
struct Slot {
    std::shared_ptr<const Material> mat;
    std::shared_ptr<Job> job;
    uint32_t used = 0;
    bool failed = false;
};
std::unordered_map<uint64_t, Slot> g_slots;
std::mutex g_sem_mutex;
std::condition_variable g_sem_cv;
int g_running = 0;

std::shared_ptr<std::vector<uint8_t>> load_png(const std::string &path, uint32_t &w, uint32_t &h) {
    int x = 0, y = 0, c = 0;
    stbi_uc *img = stbi_load(path.c_str(), &x, &y, &c, 4);
    if (!img) {
        logf("vitart: PBR: não abriu %s", path.c_str());
        return nullptr;
    }
    auto px = std::make_shared<std::vector<uint8_t>>(img, img + (size_t)x * y * 4);
    stbi_image_free(img);
    w = (uint32_t)x, h = (uint32_t)y;
    return px;
}

std::shared_ptr<std::vector<uint8_t>> solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return std::make_shared<std::vector<uint8_t>>(std::vector<uint8_t>{r, g, b, a});
}

void start(uint64_t key, Slot &s) {
    auto job = std::make_shared<Job>();
    s.job = job;
    const Files files = g_index[key];
    const bool flip = g_cfg.normal_opengl;
    std::thread([job, files, flip, key] {
        {
            std::unique_lock<std::mutex> l(g_sem_mutex);
            g_sem_cv.wait(l, [] { return g_running < 2; });
            g_running++;
        }
        auto m = std::make_shared<Material>();
        m->key = key;
        bool ok = true;
        if (!files.normal.empty()) {
            m->normal = load_png(files.normal, m->nw, m->nh);
            ok = ok && m->normal;
            if (m->normal && flip)
                for (size_t i = 1; i < m->normal->size(); i += 4) (*m->normal)[i] = (uint8_t)(255 - (*m->normal)[i]);
        } else {
            m->normal = solid(128, 128, 255, 255);
        }
        if (!files.orm.empty()) {
            m->orm = load_png(files.orm, m->ow, m->oh);
            ok = ok && m->orm;
        } else {
            m->orm = solid(255, 204, 0, 255); // sem ORM: dielétrico de rugosidade 0,8
        }
        if (ok) job->mat = m;
        {
            std::lock_guard<std::mutex> l(g_sem_mutex);
            g_running--;
        }
        g_sem_cv.notify_one();
        job->done = true;
    }).detach();
}

// Última luz real vista (desenho com g_LightDir) para os desenhos do cenário, que não têm luz própria.
struct Lights {
    float dir[2][3] = {}, col[2][3] = {}, amb[3] = {};
    uint32_t frame = 0;
    bool valid = false;
};
Lights g_last;

bool finite3(const float *v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

// Inversa 4x4 (cofatores); false se singular.
bool invert4(const float *m, float *out) {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const double det = (double)m[0] * inv[0] + (double)m[1] * inv[4] + (double)m[2] * inv[8] + (double)m[3] * inv[12];
    if (!std::isfinite(det) || std::fabs(det) < 1e-20) return false;
    for (int i = 0; i < 16; i++) out[i] = (float)(inv[i] / det);
    return true;
}

// Linha do w numa matriz de perspectiva: (0, 0, ±k, 0). Índices dos quatro elementos dessa linha.
bool w_row_is_perspective(const float *m, int a, int b, int c, int d) {
    const float z = std::fabs(m[c]);
    if (!(z > 1e-6f) || !std::isfinite(z)) return false;
    const float tol = 1e-3f * z;
    return std::fabs(m[a]) < tol && std::fabs(m[b]) < tol && std::fabs(m[d]) < tol;
}

} // namespace

void drop_all() {
    for (auto &[k, s] : g_slots) s.mat.reset(), s.job.reset(), s.failed = false;
}

const Config &config() { return g_cfg; }

bool enabled() {
    static const bool env_off = [] {
        const char *e = getenv("VITART_PBR");
        return e && *e == '0';
    }();
    if (!g_ini_read) check_ini();
    return !env_off && g_cfg.enabled;
}

void tick(uint32_t present_frame) {
    if (present_frame - g_ini_checked >= 30) { // ~meio segundo
        g_ini_checked = present_frame;
        check_ini();
    }
    if (present_frame % 64 == 0)
        for (auto &[k, s] : g_slots)
            if (s.mat && present_frame - s.used > 600) s.mat.reset();
}

uint64_t key_for(const uint8_t *base, size_t base_bytes, uint32_t w, uint32_t h, const uint8_t *palette) {
    index_load();
    if (g_index.empty() || !base_bytes) return 0;
    auto fnv = [](const uint8_t *p, size_t n, uint64_t hsh) {
        for (size_t i = 0; i < n; i++) hsh = (hsh ^ p[i]) * 0x100000001B3ull;
        return hsh;
    };
    const uint32_t wh[2] = {w, h};
    const uint64_t key = fnv(base, base_bytes < 4096 ? base_bytes : 4096,
                             fnv(reinterpret_cast<const uint8_t *>(wh), 8, 0xCBF29CE484222325ull));
    if (palette) {
        const uint64_t kp = fnv(palette, 1024, key); // variantes de paleta (como texpack_key)
        if (g_index.count(kp)) return kp;
    }
    return g_index.count(key) ? key : 0;
}

std::shared_ptr<const Material> material(uint64_t key, uint32_t present_frame) {
    if (!key || !g_index.count(key)) return nullptr;
    Slot &s = g_slots[key];
    s.used = present_frame;
    if (s.mat || s.failed) return s.mat;
    if (!s.job) {
        start(key, s);
        return nullptr;
    }
    if (!s.job->done) return nullptr;
    s.mat = s.job->mat;
    s.failed = !s.mat;
    s.job.reset();
    return s.mat;
}

std::shared_ptr<const Material> neutral() {
    static std::shared_ptr<Material> m;
    static float made_for = -1.f;
    const float r = g_cfg.default_roughness < 0 ? 0 : g_cfg.default_roughness > 1 ? 1 : g_cfg.default_roughness;
    if (!m || made_for != r) {
        m = std::make_shared<Material>();
        m->normal = solid(128, 128, 255, 255);
        m->orm = solid(255, (uint8_t)(r * 255.f + 0.5f), 0, 255);
        m->key = 0x4E45555452414C00ull | (uint64_t)(r * 255.f + 0.5f); // "NEUTRAL" + rugosidade
        made_for = r;
    }
    return m;
}

bool inv_projection(const VertexInfo &vi, const float *vu, uint32_t vu_words, float out[16]) {
    if (vi.proj < 0 || (uint32_t)vi.proj + 16 > vu_words) return false;
    float p[16], cm[16];
    std::memcpy(p, vu + vi.proj, sizeof p);
    if (w_row_is_perspective(p, 3, 7, 11, 15)) {
        std::memcpy(cm, p, sizeof cm);
    } else if (w_row_is_perspective(p, 12, 13, 14, 15)) {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) cm[c * 4 + r] = p[r * 4 + c];
    } else {
        return false;
    }
    return invert4(cm, out);
}

bool params(const VertexInfo &vi, const float *vu, uint32_t vu_words, uint32_t present_frame,
            float out[gvk::kPbrParamFloats]) {
    if (vi.proj < 0 || (uint32_t)vi.proj + 16 > vu_words) return false;
    // g_Projection chega como 4 vec4; no GoW são colunas (clip = soma col_k * v_k, medido no shader de
    // vértice). Na dúvida, a linha do w (0, 0, ±k, 0) diz a orientação; sem ela não é perspectiva.
    float p[16], cm[16];
    std::memcpy(p, vu + vi.proj, sizeof p);
    if (w_row_is_perspective(p, 3, 7, 11, 15)) {
        std::memcpy(cm, p, sizeof cm);
    } else if (w_row_is_perspective(p, 12, 13, 14, 15)) {
        for (int r = 0; r < 4; r++)
            for (int c = 0; c < 4; c++) cm[c * 4 + r] = p[r * 4 + c];
    } else {
        return false;
    }
    std::memset(out, 0, sizeof(float) * gvk::kPbrParamFloats);
    if (!invert4(cm, out)) return false;

    const Config &c = g_cfg;
    Lights L;
    if (vi.light_dir >= 0 && vi.light_color >= 0 && (uint32_t)vi.light_dir + 7 <= vu_words &&
        (uint32_t)vi.light_color + 8 <= vu_words) {
        // float3[2] alinhado a 16 bytes por elemento; float4[2] contíguo (ver sceGxmSetUniformDataF).
        for (int i = 0; i < 2; i++) {
            std::memcpy(L.dir[i], vu + vi.light_dir + 4 * i, 12);
            std::memcpy(L.col[i], vu + vi.light_color + 4 * i, 12);
            if (!finite3(L.dir[i]) || !finite3(L.col[i])) std::memset(L.col[i], 0, 12);
        }
        if (vi.ambient >= 0 && (uint32_t)vi.ambient + 3 <= vu_words) std::memcpy(L.amb, vu + vi.ambient, 12);
        if (!finite3(L.amb)) std::memset(L.amb, 0, 12);
        L.frame = present_frame, L.valid = true;
        g_last = L;
    } else if (g_last.valid && present_frame - g_last.frame <= 2) {
        L = g_last;
    } else {
        std::memcpy(L.dir[0], c.fallback_dir, 12);
        std::memcpy(L.col[0], c.fallback_color, 12);
        for (int k = 0; k < 3; k++) L.amb[k] = 0.4f * c.fallback_color[k];
    }
    // Layout PARAMS de vitarecomp/usse/pbr.py.
    for (int i = 0; i < 2; i++) {
        std::memcpy(out + 16 + 8 * i, L.dir[i], 12);
        std::memcpy(out + 20 + 8 * i, L.col[i], 12);
    }
    std::memcpy(out + 32, L.amb, 12);
    out[36] = c.normal_strength, out[37] = c.specular, out[38] = c.env_specular, out[39] = c.baked_max;
    out[40] = (float)c.debug, out[41] = c.roughness_scale, out[42] = c.metallic_scale, out[43] = c.ao_strength;
    std::memcpy(out + 44, c.sky, 12);
    std::memcpy(out + 48, c.ground, 12);
    std::memcpy(out + 52, c.up, 12);
    out[19] = c.key_dir[0], out[23] = c.key_dir[1], out[27] = c.key_dir[2];
    out[31] = c.relief, out[35] = c.key_spec;
    return true;
}

void screen_to_view(const float inv_proj[16], float vx, float vy, float vw, float vh, float min_depth, float max_depth,
                    float out[16]) {
    const float hx = vw * 0.5f, hy = vh * 0.5f;
    float dz = max_depth - min_depth;
    if (dz > -1e-9f && dz < 1e-9f) dz = 1e-9f;
    // Inversa do viewport: pixel (x, y, profundidade) -> NDC.
    float sm[16] = {};
    sm[0] = 1.f / hx, sm[5] = 1.f / hy, sm[10] = 1.f / dz, sm[15] = 1.f;
    sm[12] = -(vx + hx) / hx, sm[13] = -(vy + hy) / hy, sm[14] = -min_depth / dz;
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float acc = 0.f;
            for (int k = 0; k < 4; k++) acc += inv_proj[k * 4 + r] * sm[c * 4 + k];
            out[c * 4 + r] = acc;
        }
}

} // namespace vt::pbr
