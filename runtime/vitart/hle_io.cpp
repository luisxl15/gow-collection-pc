// HLE de arquivos (sceIo*) e de sceClib* para o vitart.
// Dispositivos do Vita -> pastas do PC: app0: = diretório do app; ux0:, savedata0:, addcont0: e
// afins ficam sob o diretório de dados (--data).
#include "vitart.h"
#include "video_hd.h"
#include <thread>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <vector>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace fs = std::filesystem;

namespace vt {

constexpr uint32_t SCE_ERROR_ERRNO_ENOENT = 0x80010002u;
constexpr uint32_t SCE_ERROR_ERRNO_EBADF = 0x80010009u;
constexpr uint32_t SCE_ERROR_ERRNO_EEXIST = 0x80010011u;

static std::string host_path(const std::string &p) {
    const size_t colon = p.find(':');
    std::string dev = colon == std::string::npos ? "" : p.substr(0, colon);
    std::string rest = colon == std::string::npos ? p : p.substr(colon + 1);
    while (!rest.empty() && (rest[0] == '/' || rest[0] == '\\'))
        rest.erase(0, 1);
    if (dev == "app0")
        return app_path() + "/" + rest;
    if (dev.empty())
        return app_path() + "/" + rest;
    return data_path() + "/" + dev + "/" + rest;
}

std::string vita_host_path(const std::string &p) { return host_path(p); }

// VITART_SKIP_VIDEOS=1: os vídeos Bink (.bk2) aparecem como inexistentes, e o jogo segue sem tocá-los.
static bool skipped_video(const std::string &path) {
    static const bool skip = getenv("VITART_SKIP_VIDEOS") && *getenv("VITART_SKIP_VIDEOS") == '1';
    if (!skip || path.size() < 4) return false;
    std::string ext = path.substr(path.size() - 4);
    for (char &c : ext) c = (char)std::tolower((unsigned char)c);
    return ext == ".bk2";
}

// Trocas dentro de um .psarc (mods do vitart, p. ex. o menu de pausa com as opções do PC): o arquivo
// <dados>/mods/<nome>.psarc.vtpatch traz a TOC inteira já remendada (mesmo tamanho da original) e os
// arquivos novos, que ficam "depois do fim" do psarc. O jogo (FIOS2) lê o psarc como sempre; o vitart
// junta as três fontes na leitura. O psarc original não é tocado. Formato: "VTPP", u64 tamanho do psarc
// original, u32 tamanho da TOC, u32 tamanho dos dados extras, TOC, extras (gerado por tools/psarc_patch.py).
struct PsarcPatch {
    uint64_t orig = 0;
    std::vector<uint8_t> toc, extra;
};
// Carregador de mods (docs/modding/README.md). Por cima do patch do vitart, cada mod ligado (mod_<id>=1 no
// vitart_options.ini; menu de pausa: Options > Mods, aplicado ao reiniciar) troca arquivos inteiros do psarc:
// <mods>/<id>/entries.tsv lista "<psarc>\t<entrada da TOC>\t<arquivo no mod>". Vários mods juntos: vale a ordem de
// mods/modlist.tsv (o último ganha). Os arquivos entram crus (blocos sem compressão) no fim dos extras.
std::string mods_dir_for(const std::string &) {
    const char *md = getenv("VITART_MODS_DIR"); // testes: outra pasta de patches, sem mexer nos instalados
    return md && *md ? std::string(md) : data_path() + "/mods";
}
// Mods ligados, na ordem de modlist.tsv ("<jogo>\t<índice no menu>\t<id>\t<nome>").
std::vector<std::string> enabled_mods() {
    std::vector<std::string> on, out;
    if (FILE *f = fopen((data_path() + "/vitart_options.ini").c_str(), "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            char id[128];
            unsigned v = 0;
            if (sscanf(line, "mod_%127[A-Za-z0-9_]=%u", id, &v) == 2 && v) on.push_back(id);
        }
        fclose(f);
    }
    if (FILE *f = fopen((mods_dir_for("") + "/modlist.tsv").c_str(), "r")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char game[16], id[128];
            unsigned idx = 0;
            if (sscanf(line, "%15s %u %127s", game, &idx, id) == 3 && std::find(on.begin(), on.end(), id) != on.end())
                out.push_back(id);
        }
        fclose(f);
    }
    return out;
}
static uint64_t be(const uint8_t *p, int n) {
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = v << 8 | p[i];
    return v;
}
static void put_be(uint8_t *p, int n, uint64_t v) {
    for (int i = n - 1; i >= 0; i--, v >>= 8) p[i] = (uint8_t)v;
}
// Troca a entrada idx da TOC (já remendada) por data: entradas novas no fim da tabela de blocos (0 = bloco cheio cru)
// e os dados, completados até o bloco, no fim dos extras.
static void overlay_file(std::vector<uint8_t> &toc, std::vector<uint8_t> &extra, uint64_t orig, uint32_t idx,
                         const std::vector<uint8_t> &data) {
    const uint32_t ent = (uint32_t)be(&toc[16], 4), n = (uint32_t)be(&toc[20], 4), block = (uint32_t)be(&toc[24], 4);
    const int w = block <= 0x10000 ? 2 : block <= 0x1000000 ? 3 : 4;
    const size_t tab_at = 32 + (size_t)n * ent;
    if (idx == 0 || idx >= n) return;
    const uint64_t nb = (data.size() + block - 1) / block;
    const uint64_t first = (toc.size() - tab_at) / w;
    toc.resize(toc.size() + nb * w, 0);
    uint8_t *e = &toc[32 + (size_t)idx * ent];
    put_be(e + 16, 4, first);
    put_be(e + 20, 5, data.size());
    put_be(e + 25, 5, orig + extra.size());
    extra.insert(extra.end(), data.begin(), data.end());
    extra.resize(extra.size() + (nb * block - data.size()), 0);
}
// A TOC cresceu: o que o psarc tinha logo depois dela (o manifesto, entrada 0, e o que mais estiver no trecho
// coberto) ficaria escondido pela TOC nova; esses dados são copiados crus para os extras.
static void relocate_shadowed(std::vector<uint8_t> &toc, std::vector<uint8_t> &extra, uint64_t orig, FILE *ps,
                              uint64_t orig_toc_len) {
    const uint32_t ent = (uint32_t)be(&toc[16], 4), n = (uint32_t)be(&toc[20], 4), block = (uint32_t)be(&toc[24], 4);
    const int w = block <= 0x10000 ? 2 : block <= 0x1000000 ? 3 : 4;
    const size_t tab_at = 32 + (size_t)n * ent;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *e = &toc[32 + (size_t)i * ent];
        const uint64_t bi = be(e + 16, 4), size = be(e + 20, 5), off = be(e + 25, 5);
        if (off >= orig || off >= toc.size() || !size) continue;
        uint64_t raw = 0;
        for (uint64_t k = 0; k * block < size; k++) {
            const size_t at = tab_at + (size_t)(bi + k) * w;
            const uint64_t z = at + w <= toc.size() ? be(&toc[at], w) : 0;
            raw += z ? z : block;
        }
        if (off + raw <= orig_toc_len) continue;
        std::vector<uint8_t> buf(raw);
        _fseeki64(ps, (int64_t)off, SEEK_SET);
        if (fread(buf.data(), 1, raw, ps) != raw) continue;
        put_be(e + 25, 5, orig + extra.size());
        extra.insert(extra.end(), buf.begin(), buf.end());
    }
}
static std::shared_ptr<PsarcPatch> psarc_patch(const std::string &hp) {
    if (hp.size() < 6 || _stricmp(hp.c_str() + hp.size() - 6, ".psarc") != 0) return nullptr;
    static const bool off = getenv("VITART_NO_MODS") != nullptr;
    if (off) return nullptr;
    static std::mutex m;
    static std::unordered_map<std::string, std::shared_ptr<PsarcPatch>> cache; // carregado uma vez por psarc
    const std::lock_guard<std::mutex> lock(m);
    if (auto it = cache.find(hp); it != cache.end()) return it->second;
    auto &slot = cache[hp];
    const std::string name = fs::path(hp).filename().string();
    const std::string dir = mods_dir_for(name);
    const std::string pp = dir + "/" + name + ".vtpatch";
    auto pt = std::make_shared<PsarcPatch>();
    std::error_code ec;
    bool have = false;
    if (FILE *f = fopen(pp.c_str(), "rb")) {
        char magic[4] = {};
        uint32_t tl = 0, el = 0;
        bool ok = fread(magic, 1, 4, f) == 4 && !memcmp(magic, "VTPP", 4) && fread(&pt->orig, 8, 1, f) == 1 &&
                  fread(&tl, 4, 1, f) == 1 && fread(&el, 4, 1, f) == 1;
        if (ok) {
            pt->toc.resize(tl), pt->extra.resize(el);
            ok = fread(pt->toc.data(), 1, tl, f) == tl && fread(pt->extra.data(), 1, el, f) == el;
        }
        fclose(f);
        if (!ok || fs::file_size(hp, ec) != pt->orig) {
            logf("vitart: %s ignorado (não corresponde a este %s)", pp.c_str(), name.c_str());
            pt = std::make_shared<PsarcPatch>();
        } else {
            have = true;
            logf("vitart: mods em %s: TOC remendada, %u bytes de arquivos trocados", name.c_str(), el);
        }
    }
    // mods do carregador: arquivos inteiros por cima (docs/modding/README.md)
    FILE *ps = fopen(hp.c_str(), "rb");
    if (!ps) return have ? slot = pt : nullptr;
    uint8_t hdr[32] = {};
    const bool hdr_ok = fread(hdr, 1, 32, ps) == 32 && !memcmp(hdr, "PSAR", 4);
    const uint64_t orig_toc_len = hdr_ok ? be(hdr + 12, 4) : 0;
    if (hdr_ok && !have) {
        pt->orig = fs::file_size(hp, ec);
        pt->toc.resize(orig_toc_len);
        _fseeki64(ps, 0, SEEK_SET);
        if (fread(pt->toc.data(), 1, orig_toc_len, ps) != orig_toc_len) pt->toc.clear();
    }
    int nmods = 0, nfiles = 0;
    if (hdr_ok && !pt->toc.empty())
        for (const std::string &id : enabled_mods()) {
            FILE *t = fopen((dir + "/" + id + "/entries.tsv").c_str(), "r");
            if (!t) continue;
            char line[1024];
            int got = 0;
            while (fgets(line, sizeof line, t)) {
                char pn[256], rel[700];
                unsigned idx = 0;
                if (sscanf(line, "%255[^\t]\t%u\t%699[^\t\r\n]", pn, &idx, rel) != 3 || _stricmp(pn, name.c_str()) != 0)
                    continue;
                std::ifstream in(fs::u8path(dir + "/" + id + "/" + rel), std::ios::binary);
                std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (data.empty()) {
                    logf("vitart: mod %s: %s não encontrado", id.c_str(), rel);
                    continue;
                }
                overlay_file(pt->toc, pt->extra, pt->orig, idx, data);
                got++;
            }
            fclose(t);
            if (got) nmods++, nfiles += got, logf("vitart: mod %s: %d arquivos em %s", id.c_str(), got, name.c_str());
        }
    if (nfiles) {
        if (pt->toc.size() > orig_toc_len) relocate_shadowed(pt->toc, pt->extra, pt->orig, ps, orig_toc_len);
        put_be(&pt->toc[12], 4, pt->toc.size());
    }
    fclose(ps);
    if (!have && !nfiles) return nullptr;
    return slot = pt;
}

struct File {
    int fd = -1;
    std::shared_ptr<PsarcPatch> patch; // psarc com trocas (ver acima)
    int64_t pos = 0;                   // posição de leitura quando há trocas
    std::string path;
    bool is_dir = false;
    bool video = false;      // .bk2 (cena em vídeo)
    uint32_t skip_gen = 0;   // pedidos de pular já existentes quando foi aberto
    fs::directory_iterator it, end;
};
// Pular a cena em vídeo (X): acha na memória do jogo o HBINK do vídeo que está tocando ({Width, Height,
// Frames, FrameNum, LastFrameNum, ...}, o começo da estrutura do Bink SDK) e faz Frames = FrameNum + 1; o
// laço do jogo vê o último quadro e fecha o vídeo do jeito normal. (Fim de arquivo congelava o Bink.)
// O pedido fica pendente por alguns segundos e é tentado a cada leitura do controle: apertado logo no
// começo, o arquivo já está aberto mas o HBINK ainda não existe. Só procura com o vídeo tocando (lido nos
// últimos 0,5 s) e guarda onde achou, porque varrer a memória do jogo custa dezenas de ms.
static std::atomic<uint32_t> g_skip_gen{0};
// Cabeçalho do vídeo aberto (Bink 2: quadros em +8, largura em +20, altura em +24).
static std::atomic<uint32_t> g_video_open{0}, g_video_w{0}, g_video_h{0}, g_video_frames{0}, g_video_gen{0};
static std::atomic<int64_t> g_video_read_ms{0}, g_skip_until_ms{0};
static uint32_t g_hbink = 0, g_hbink_gen = 0; // onde o HBINK do vídeo atual foi achado
static int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static bool hbink_matches(const uint32_t *v, uint32_t w, uint32_t h, uint32_t frames) {
    return v[0] == w && v[1] == h && v[2] == frames && v[3] <= frames && v[4] <= frames;
}
static uint32_t hbink_find(uint32_t w, uint32_t h, uint32_t frames) {
    uint8_t *base = mem_base();
    for (uint8_t *p = base; p < base + 0x100000000ull;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(p, &mi, sizeof mi)) break;
        uint8_t *end = (uint8_t *)mi.BaseAddress + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint8_t *q = (uint8_t *)(((uintptr_t)p + 3) & ~(uintptr_t)3); q + 20 <= end; q += 4) {
                uint32_t v[5];
                std::memcpy(v, q, 20);
                if (hbink_matches(v, w, h, frames)) return (uint32_t)(q - base);
            }
        p = end;
    }
    return 0;
}
// Quadro atual do Bink do jogo (para a cena remasterizada, video_hd.cpp); -1 enquanto o HBINK não foi
// achado. A busca na memória do jogo roda numa thread à parte, uma vez por vídeo.
static std::atomic<bool> g_hbink_busy{false};
static std::atomic<uint32_t> g_hbink_async{0}, g_hbink_async_gen{0};
int64_t video_bink_frame() {
    if (!g_video_open) return -1;
    const uint32_t gen = g_video_gen, w = g_video_w, h = g_video_h, frames = g_video_frames;
    uint32_t at = g_hbink_async_gen == gen ? g_hbink_async.load() : 0;
    if (at) {
        uint32_t v[5];
        std::memcpy(v, ptr<uint8_t>(at), 20);
        if (hbink_matches(v, w, h, frames)) return v[3];
        g_hbink_async = 0;
    }
    if (now_ms() - g_video_read_ms > 500 || g_hbink_busy.exchange(true)) return -1;
    std::thread([gen, w, h, frames] {
        const auto t0 = std::chrono::steady_clock::now();
        const uint32_t a = hbink_find(w, h, frames);
        static int logged = 0;
        if (logged++ < 40)
            logf("vitart: busca do HBINK: %s em %.0f ms", a ? "achado" : "não achado",
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        if (a) g_hbink_async = a, g_hbink_async_gen = gen;
        std::this_thread::sleep_for(std::chrono::milliseconds(a ? 0 : 200));
        g_hbink_busy = false;
    }).detach();
    return -1;
}

void video_skip_request() {
    g_skip_gen++;
    if (g_video_open) g_skip_until_ms = now_ms() + 4000;
}
// Chamada a cada leitura do controle.
void video_skip_tick() {
    const int64_t until = g_skip_until_ms, now = now_ms();
    if (!until) return;
    if (now > until || !g_video_open) {
        g_skip_until_ms = 0;
        return;
    }
    if (now - g_video_read_ms > 500) return; // vídeo aberto mas parado: espera ele tocar
    static int64_t last_try = 0;
    if (now - last_try < 150) return;
    last_try = now;
    const uint32_t w = g_video_w, h = g_video_h, frames = g_video_frames;
    if (!w || !h || !frames) return;
    uint32_t at = 0;
    if (g_hbink && g_hbink_gen == g_video_gen) {
        uint32_t v[5];
        std::memcpy(v, ptr<uint8_t>(g_hbink), 20);
        if (hbink_matches(v, w, h, frames)) at = g_hbink;
    }
    if (!at) at = hbink_find(w, h, frames);
    if (!at) return; // ainda não criado: tenta de novo daqui a pouco
    g_hbink = at, g_hbink_gen = g_video_gen;
    uint32_t v[5];
    std::memcpy(v, ptr<uint8_t>(at), 20);
    const uint32_t cut = v[3] + 1 < frames ? v[3] + 1 : frames;
    std::memcpy(ptr<uint8_t>(at + 8), &cut, 4);
    g_skip_until_ms = 0;
    logf("vitart: cena pulada (HBINK em 0x%08x: quadro %u de %u)", at, v[3], frames);
}
static bool is_video(const std::string &p) {
    if (p.size() < 4) return false;
    std::string e = p.substr(p.size() - 4);
    for (char &c : e) c = (char)std::tolower((unsigned char)c);
    return e == ".bk2";
}
// Leitura de um vídeo que o jogador mandou pular: fim de arquivo.
static bool video_cut(uint32_t vfd);
static std::mutex g_files_mutex;
static std::unordered_map<uint32_t, File> g_files;
static uint32_t g_next_fd = 0x00020001;

static void fill_stat(uint32_t st, const fs::path &p) {
    std::error_code ec;
    std::memset(ptr<uint8_t>(st), 0, 0x58);
    const bool dir = fs::is_directory(p, ec);
    const uint32_t mode = (dir ? 0x1000u : 0x2000u) | 0x01FFu; // SCE_S_IFDIR/IFREG + rwx
    wr32(st + 0, mode);
    wr32(st + 4, dir ? 0x10u : 0x20u);
    if (!dir) {
        uint64_t size = (uint64_t)fs::file_size(p, ec);
        if (auto pt = psarc_patch(p.string())) size = pt->orig + pt->extra.size();
        wr64(st + 8, size);
    }
}

VT_HLE(sceIoOpen) {
    const std::string path = gstr(arg(ctx, 0));
    const uint32_t flags = arg(ctx, 1);
    if (skipped_video(path)) {
        logf("vitart: vídeo pulado: %s", path.c_str());
        return ret(ctx, SCE_ERROR_ERRNO_ENOENT);
    }
    const std::string hp = host_path(path);
    int of = _O_BINARY;
    const uint32_t acc = flags & 3;
    of |= acc == 1 ? _O_RDONLY : acc == 2 ? _O_WRONLY : _O_RDWR;
    if (flags & 0x0100) of |= _O_APPEND;
    if (flags & 0x0200) of |= _O_CREAT;
    if (flags & 0x0400) of |= _O_TRUNC;
    if (flags & 0x0800) of |= _O_EXCL;
    if (of & _O_CREAT) {
        std::error_code ec;
        fs::create_directories(fs::path(hp).parent_path(), ec);
    }
    const int fd = _open(hp.c_str(), of, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        logf("vitart: sceIoOpen(%s, 0x%x) falhou (%s)", path.c_str(), flags, hp.c_str());
        return ret(ctx, (flags & 0x0800) && errno == EEXIST ? SCE_ERROR_ERRNO_EEXIST : SCE_ERROR_ERRNO_ENOENT);
    }
    const std::lock_guard<std::mutex> lock(g_files_mutex);
    const uint32_t vfd = g_next_fd++;
    g_files[vfd].fd = fd;
    g_files[vfd].path = path;
    g_files[vfd].video = is_video(path);
    if (acc == 1) g_files[vfd].patch = psarc_patch(hp);
    if (g_files[vfd].video) {
        uint32_t hd[9] = {};
        _lseeki64(fd, 0, SEEK_SET);
        if (_read(fd, hd, sizeof hd) == (int)sizeof hd) g_video_frames = hd[2], g_video_w = hd[5], g_video_h = hd[6];
        _lseeki64(fd, 0, SEEK_SET);
        g_video_open++;
        g_video_gen++;
        video_hd_open(path, hd[2], hd[7], hd[8], 0, 0);
    }
    g_files[vfd].skip_gen = g_skip_gen.load();
    logf("vitart: sceIoOpen(%s, 0x%x) -> 0x%x", path.c_str(), flags, vfd);
    ret(ctx, vfd);
}

static int host_fd(uint32_t vfd) {
    if (vfd == 0x00010001u) return 1;
    if (vfd == 0x00010002u) return 2;
    if (vfd == 0x00010000u) return 0;
    const std::lock_guard<std::mutex> lock(g_files_mutex);
    auto it = g_files.find(vfd);
    return it == g_files.end() ? -1 : it->second.fd;
}

static bool video_cut(uint32_t vfd) {
    const std::lock_guard<std::mutex> lock(g_files_mutex);
    auto it = g_files.find(vfd);
    if (it != g_files.end() && it->second.video) g_video_read_ms = now_ms(); // vídeo tocando
    static const bool eof_cut = getenv("VITART_SKIP_EOF") != nullptr; // experimento: o Bink congela no fim de arquivo
    if (!eof_cut || it == g_files.end() || !it->second.video || g_skip_gen.load() == it->second.skip_gen) return false;
    static uint32_t logged = 0;
    if (logged != it->second.skip_gen + 1) {
        logged = it->second.skip_gen + 1;
        logf("vitart: cena pulada com START: %s", it->second.path.c_str());
    }
    return true;
}

VT_HLE(sceIoClose) {
    const std::lock_guard<std::mutex> lock(g_files_mutex);
    auto it = g_files.find(arg(ctx, 0));
    if (it == g_files.end()) return ret(ctx, SCE_ERROR_ERRNO_EBADF);
    _close(it->second.fd);
    if (it->second.video && g_video_open && !--g_video_open) video_hd_close();
    g_files.erase(it);
    ret(ctx, 0);
}
// Leitura/escrita posicional atômica (várias threads do jogo usam o mesmo descritor).
static int pio(int fd, uint32_t buf, uint32_t n, int64_t off, bool write) {
    commit_range(buf, n);
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD done = 0;
    const BOOL ok = write ? WriteFile(h, ptr<void>(buf), n, &done, &ov) : ReadFile(h, ptr<void>(buf), n, &done, &ov);
    if (!ok && GetLastError() != ERROR_HANDLE_EOF) {
        logf("vitart: E/S posicional falhou (erro %lu, buf 0x%08x, %u bytes, offset %lld)", GetLastError(), buf, n, (long long)off);
        return -1;
    }
    return (int)done;
}

// Leitura de um psarc com trocas: TOC remendada, psarc original e arquivos extras no fim.
static int patched_read(int fd, const PsarcPatch &pt, uint32_t buf, uint32_t n, int64_t off) {
    commit_range(buf, n);
    uint8_t *dst = ptr<uint8_t>(buf);
    const int64_t total = (int64_t)(pt.orig + pt.extra.size());
    if (off >= total) return 0;
    if (off + n > total) n = (uint32_t)(total - off);
    uint32_t done = 0;
    while (done < n) {
        const int64_t o = off + done;
        uint32_t chunk = n - done;
        if (o < (int64_t)pt.toc.size()) {
            chunk = (uint32_t)std::min<int64_t>(chunk, (int64_t)pt.toc.size() - o);
            memcpy(dst + done, pt.toc.data() + o, chunk);
        } else if (o >= (int64_t)pt.orig) {
            memcpy(dst + done, pt.extra.data() + (o - pt.orig), chunk);
        } else {
            chunk = (uint32_t)std::min<int64_t>(chunk, (int64_t)pt.orig - o);
            const int r = pio(fd, buf + done, chunk, o, false);
            if (r <= 0) break;
            chunk = (uint32_t)r;
        }
        done += chunk;
    }
    return (int)done;
}
static std::shared_ptr<PsarcPatch> file_patch(uint32_t vfd, int64_t **pos) {
    const std::lock_guard<std::mutex> lock(g_files_mutex);
    auto it = g_files.find(vfd);
    if (it == g_files.end() || !it->second.patch) return nullptr;
    *pos = &it->second.pos;
    return it->second.patch;
}

VT_HLE(sceIoRead) {
    const int fd = host_fd(arg(ctx, 0));
    if (fd < 0) return ret(ctx, SCE_ERROR_ERRNO_EBADF);
    if (video_cut(arg(ctx, 0))) return ret(ctx, 0);
    int64_t *pos = nullptr;
    if (auto pt = file_patch(arg(ctx, 0), &pos)) {
        const int n = patched_read(fd, *pt, arg(ctx, 1), arg(ctx, 2), *pos);
        if (n > 0) *pos += n;
        return ret(ctx, n < 0 ? SCE_ERROR_ERRNO_EBADF : (uint32_t)n);
    }
    commit_range(arg(ctx, 1), arg(ctx, 2));
    const int n = _read(fd, ptr<void>(arg(ctx, 1)), arg(ctx, 2));
    ret(ctx, n < 0 ? SCE_ERROR_ERRNO_EBADF : (uint32_t)n);
}
VT_HLE(sceIoWrite) {
    const int fd = host_fd(arg(ctx, 0));
    if (fd < 0) return ret(ctx, SCE_ERROR_ERRNO_EBADF);
    if (fd == 1 || fd == 2) {
        std::string s(ptr<char>(arg(ctx, 1)), arg(ctx, 2));
        while (!s.empty() && s.back() == '\n') s.pop_back();
        logf("[jogo] %s", s.c_str());
        return ret(ctx, arg(ctx, 2));
    }
    commit_range(arg(ctx, 1), arg(ctx, 2));
    const int n = _write(fd, ptr<void>(arg(ctx, 1)), arg(ctx, 2));
    ret(ctx, n < 0 ? SCE_ERROR_ERRNO_EBADF : (uint32_t)n);
}
VT_HLE(sceIoLseek) {
    // (fd, SceOff offset em r2:r3, whence na pilha) -> SceOff em r0:r1
    const int fd = host_fd(arg(ctx, 0));
    if (fd < 0) return ret64(ctx, (uint64_t)(int64_t)(int32_t)SCE_ERROR_ERRNO_EBADF);
    const int64_t off = (int64_t)((uint64_t)arg(ctx, 2) | ((uint64_t)arg(ctx, 3) << 32));
    const int whence = (int)arg(ctx, 4);
    int64_t *pos = nullptr;
    if (auto pt = file_patch(arg(ctx, 0), &pos)) {
        const int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? *pos : (int64_t)(pt->orig + pt->extra.size());
        *pos = base + off;
        return ret64(ctx, (uint64_t)*pos);
    }
    ret64(ctx, (uint64_t)_lseeki64(fd, off, whence));
}
VT_HLE(sceIoPread) {
    // (fd, buf, nbyte, SceOff offset na pilha [sp], alinhado a 8)
    const int fd = host_fd(arg(ctx, 0));
    if (fd < 0) return ret(ctx, SCE_ERROR_ERRNO_EBADF);
    const int64_t off = (int64_t)((uint64_t)arg(ctx, 4) | ((uint64_t)arg(ctx, 5) << 32));
    if (video_cut(arg(ctx, 0))) return ret(ctx, 0);
    int64_t *pos = nullptr;
    if (auto pt = file_patch(arg(ctx, 0), &pos)) {
        const int n = patched_read(fd, *pt, arg(ctx, 1), arg(ctx, 2), off);
        return ret(ctx, n < 0 ? SCE_ERROR_ERRNO_EBADF : (uint32_t)n);
    }
    const int n = pio(fd, arg(ctx, 1), arg(ctx, 2), off, false);
    ret(ctx, n < 0 ? SCE_ERROR_ERRNO_EBADF : (uint32_t)n);
}
VT_HLE(sceIoPwrite) {
    const int fd = host_fd(arg(ctx, 0));
    if (fd < 0) return ret(ctx, SCE_ERROR_ERRNO_EBADF);
    const int64_t off = (int64_t)((uint64_t)arg(ctx, 4) | ((uint64_t)arg(ctx, 5) << 32));
    const int n = pio(fd, arg(ctx, 1), arg(ctx, 2), off, true);
    ret(ctx, n < 0 ? SCE_ERROR_ERRNO_EBADF : (uint32_t)n);
}
VT_HLE(sceIoSync) { ret(ctx, 0); }
VT_HLE(sceIoSyncByFd) { ret(ctx, 0); }
VT_HLE(sceIoDevctl) { ret(ctx, 0); }
VT_HLE(sceIoIoctl) { ret(ctx, 0); }
VT_HLE(sceIoChstat) { ret(ctx, 0); }
VT_HLE(sceIoGetstat) {
    if (skipped_video(gstr(arg(ctx, 0)))) return ret(ctx, SCE_ERROR_ERRNO_ENOENT);
    const fs::path p = host_path(gstr(arg(ctx, 0)));
    std::error_code ec;
    if (!fs::exists(p, ec)) return ret(ctx, SCE_ERROR_ERRNO_ENOENT);
    fill_stat(arg(ctx, 1), p);
    ret(ctx, 0);
}
VT_HLE(sceIoGetstatByFd) {
    std::string path;
    {
        const std::lock_guard<std::mutex> lock(g_files_mutex);
        auto it = g_files.find(arg(ctx, 0));
        if (it == g_files.end()) return ret(ctx, SCE_ERROR_ERRNO_EBADF);
        path = it->second.path;
    }
    fill_stat(arg(ctx, 1), host_path(path));
    ret(ctx, 0);
}
VT_HLE(sceIoMkdir) {
    std::error_code ec;
    fs::create_directories(host_path(gstr(arg(ctx, 0))), ec);
    ret(ctx, 0);
}
VT_HLE(sceIoRmdir) {
    std::error_code ec;
    ret(ctx, fs::remove(host_path(gstr(arg(ctx, 0))), ec) ? 0 : SCE_ERROR_ERRNO_ENOENT);
}
VT_HLE(sceIoRemove) {
    std::error_code ec;
    ret(ctx, fs::remove(host_path(gstr(arg(ctx, 0))), ec) ? 0 : SCE_ERROR_ERRNO_ENOENT);
}
VT_HLE(sceIoRename) {
    std::error_code ec;
    fs::rename(host_path(gstr(arg(ctx, 0))), host_path(gstr(arg(ctx, 1))), ec);
    ret(ctx, ec ? SCE_ERROR_ERRNO_ENOENT : 0);
}
VT_HLE(sceIoDopen) {
    const std::string path = gstr(arg(ctx, 0));
    std::error_code ec;
    fs::directory_iterator it(host_path(path), ec);
    if (ec) return ret(ctx, SCE_ERROR_ERRNO_ENOENT);
    const std::lock_guard<std::mutex> lock(g_files_mutex);
    const uint32_t vfd = g_next_fd++;
    File &f = g_files[vfd];
    f.is_dir = true;
    f.path = path;
    f.it = std::move(it);
    ret(ctx, vfd);
}
VT_HLE(sceIoDread) {
    // (fd, SceIoDirent*): stat (0x58) + d_name[256] + d_private + dummy
    const std::lock_guard<std::mutex> lock(g_files_mutex);
    auto fit = g_files.find(arg(ctx, 0));
    if (fit == g_files.end() || !fit->second.is_dir) return ret(ctx, SCE_ERROR_ERRNO_EBADF);
    File &f = fit->second;
    if (f.it == f.end) return ret(ctx, 0);
    const uint32_t d = arg(ctx, 1);
    fill_stat(d, f.it->path());
    const std::string name = f.it->path().filename().string();
    std::memset(ptr<char>(d + 0x58), 0, 256);
    std::memcpy(ptr<char>(d + 0x58), name.c_str(), std::min<size_t>(name.size(), 255));
    std::error_code ec;
    f.it.increment(ec);
    ret(ctx, 1);
}
VT_HLE(sceIoDclose) {
    const std::lock_guard<std::mutex> lock(g_files_mutex);
    ret(ctx, g_files.erase(arg(ctx, 0)) ? 0 : SCE_ERROR_ERRNO_EBADF);
}

// =============================================================== sceClib*
VT_HLE(sceClibMemset) {
    std::memset(ptr<uint8_t>(arg(ctx, 0)), (int)arg(ctx, 1), arg(ctx, 2));
    ret(ctx, arg(ctx, 0));
}

// printf do guest: argumentos variádicos pela AAPCS (palavras r0-r3 e depois a pilha; valores de
// 64 bits começam em palavra par).
static std::string guest_format(arc_ctx *ctx, int first) {
    const char *f = gstr(arg(ctx, first));
    int w = first + 1;
    auto next32 = [&]() { return arg(ctx, w++); };
    auto next64 = [&]() {
        if (w & 1) w++;
        const uint64_t lo = arg(ctx, w), hi = arg(ctx, w + 1);
        w += 2;
        return lo | (hi << 32);
    };
    std::string out;
    char buf[512];
    while (*f) {
        if (*f != '%') {
            out += *f++;
            continue;
        }
        std::string spec = "%";
        f++;
        while (*f && std::strchr("-+ #0123456789.*", *f)) {
            if (*f == '*') spec += std::to_string((int)next32());
            else spec += *f;
            f++;
        }
        int longs = 0;
        bool sz = false;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'j' || *f == 't') {
            if (*f == 'l') longs++;
            if (*f == 'z' || *f == 'j' || *f == 't') sz = true;
            f++;
        }
        const char c = *f ? *f++ : 0;
        switch (c) {
        case 'd': case 'i':
            if (longs >= 2) snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)next64());
            else snprintf(buf, sizeof buf, (spec + "d").c_str(), (int)next32());
            out += buf;
            break;
        case 'u': case 'x': case 'X': case 'o':
            if (longs >= 2) snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), (unsigned long long)next64());
            else snprintf(buf, sizeof buf, (spec + c).c_str(), (unsigned)next32());
            out += buf;
            break;
        case 'p':
            snprintf(buf, sizeof buf, "0x%08x", next32());
            out += buf;
            break;
        case 'c':
            snprintf(buf, sizeof buf, (spec + "c").c_str(), (int)next32());
            out += buf;
            break;
        case 's': {
            const uint32_t a = next32();
            snprintf(buf, sizeof buf, (spec + "s").c_str(), a ? gstr(a) : "(null)");
            out += buf;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': {
            const uint64_t bits = next64();
            double d;
            std::memcpy(&d, &bits, 8);
            snprintf(buf, sizeof buf, (spec + c).c_str(), d);
            out += buf;
            break;
        }
        case '%':
            out += '%';
            break;
        default:
            out += spec;
            if (c) out += c;
        }
        (void)sz;
    }
    return out;
}

VT_HLE(sceClibPrintf) {
    std::string s = guest_format(ctx, 0);
    while (!s.empty() && s.back() == '\n') s.pop_back();
    logf("[jogo] %s", s.c_str());
    ret(ctx, (uint32_t)s.size());
}
VT_HLE(sceClibSnprintf) {
    const std::string s = guest_format(ctx, 2);
    const uint32_t dst = arg(ctx, 0), n = arg(ctx, 1);
    if (n) {
        const size_t k = std::min<size_t>(s.size(), n - 1);
        std::memcpy(ptr<char>(dst), s.data(), k);
        ptr<char>(dst)[k] = 0;
    }
    ret(ctx, (uint32_t)s.size());
}

} // namespace vt
