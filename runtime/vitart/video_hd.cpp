// Cenas em vídeo remasterizadas: enquanto o jogo toca um .bk2 que tem versão em alta resolução em
// <dados>/videos/<jogo>/index.tsv (nome do .bk2<TAB>arquivo[<TAB>atraso em s]), a imagem que vai para a
// tela é o quadro do arquivo novo no mesmo instante do vídeo do jogo. O jogo continua decodificando o
// Bink dele (som, fim do vídeo e o pular com X ficam iguais); só a imagem é trocada.
//
// O arquivo é decodificado por um ffmpeg.exe (ao lado do vitart.exe, ou no PATH) num processo filho, com
// decodificação na GPU, em RGBA no tamanho de saída pelo pipe. O instante vem do contador de quadros do
// Bink do jogo (HBINK, hle_io.cpp) mais o relógio desde a última mudança dele, então o vídeo novo (60
// quadros/s) anda liso mesmo com o Bink a 29,97.
#include "vitart.h"
#include "video_hd.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace vt {
namespace {

struct Entry {
    std::string file;
    double offset = 0; // segundos somados ao instante do jogo
};
std::map<std::string, Entry> g_index; // nome do .bk2 em minúsculas, sem extensão
bool g_index_loaded = false;

std::string lower(std::string s) {
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void load_index() {
    if (g_index_loaded) return;
    g_index_loaded = true;
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::path(data_path()) / "videos";
    for (auto &dir : std::filesystem::directory_iterator(root, ec)) {
        std::ifstream f(dir.path() / "index.tsv");
        std::string line;
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t a = line.find('\t');
            if (a == std::string::npos) continue;
            const size_t b = line.find('\t', a + 1);
            Entry e;
            e.file = (dir.path() / line.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1)).string();
            if (b != std::string::npos) e.offset = std::atof(line.c_str() + b + 1);
            if (std::filesystem::exists(e.file, ec)) g_index[lower(line.substr(0, a))] = e;
        }
    }
    if (!g_index.empty()) logf("vitart: cenas remasterizadas: %zu vídeos", g_index.size());
}

std::string ffmpeg_path() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    const auto here = std::filesystem::path(exe).parent_path() / "ffmpeg.exe";
    std::error_code ec;
    if (std::filesystem::exists(here, ec)) return here.string();
    return "ffmpeg.exe";
}

struct Frame {
    int64_t index = 0; // quadro do arquivo (60 por segundo no remaster)
    std::vector<uint8_t> rgba;
};

// Um vídeo tocando: o processo ffmpeg e a fila curta de quadros decodificados.
struct Player {
    Entry entry;
    double file_fps = 60.0;
    uint32_t w = 0, h = 0;
    HANDLE proc = nullptr, out = nullptr;
    std::thread reader;
    std::mutex m;
    std::condition_variable cv;
    std::deque<Frame> queue;
    std::atomic<bool> stop{false}, eof{false};
    int64_t start_index = 0; // primeiro quadro que o ffmpeg entrega (depois de um salto)

    bool start(int64_t from_index) {
        start_index = from_index;
        const double ss = from_index / file_fps;
        char cmd[2048];
        std::snprintf(cmd, sizeof cmd,
                      "\"%s\" -v error -nostdin -hwaccel d3d11va -ss %.3f -i \"%s\" -an -vf scale=%u:%u:flags=bicubic "
                      "-f rawvideo -pix_fmt rgba -",
                      ffmpeg_path().c_str(), ss, entry.file.c_str(), w, h);
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, (DWORD)(w * h * 4 * 2))) return false;
        SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOA si{sizeof si};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = wr;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = nullptr;
        PROCESS_INFORMATION pi{};
        const BOOL ok = CreateProcessA(nullptr, cmd, nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                       nullptr, nullptr, &si, &pi);
        CloseHandle(wr);
        if (!ok) {
            CloseHandle(rd);
            logf("vitart: cenas remasterizadas: não abriu o ffmpeg (%s)", ffmpeg_path().c_str());
            return false;
        }
        CloseHandle(pi.hThread);
        proc = pi.hProcess, out = rd;
        stop = false, eof = false;
        reader = std::thread([this] { read_loop(); });
        return true;
    }

    void read_loop() {
        const size_t bytes = (size_t)w * h * 4;
        int64_t idx = start_index;
        while (!stop) {
            Frame f;
            f.index = idx++;
            f.rgba.resize(bytes);
            size_t got = 0;
            while (got < bytes) {
                DWORD n = 0;
                if (!ReadFile(out, f.rgba.data() + got, (DWORD)(bytes - got), &n, nullptr) || !n) break;
                got += n;
            }
            if (got < bytes) break;
            std::unique_lock<std::mutex> l(m);
            cv.wait(l, [this] { return stop || queue.size() < 4; });
            if (stop) break;
            queue.push_back(std::move(f));
        }
        eof = true;
    }

    void halt() {
        stop = true;
        cv.notify_all();
        if (proc) TerminateProcess(proc, 0);
        if (reader.joinable()) reader.join();
        if (out) CloseHandle(out);
        if (proc) CloseHandle(proc);
        proc = out = nullptr;
        queue.clear();
    }
    ~Player() { halt(); }

    // Quadro do arquivo para o índice pedido (o mais recente que não passa dele). Recomeça o ffmpeg num
    // salto grande (para frente ou para trás).
    const Frame *get(int64_t want, Frame &hold) {
        std::unique_lock<std::mutex> l(m);
        if (!queue.empty() && (want < queue.front().index - 2 || want > queue.back().index + 90)) {
            l.unlock();
            halt();
            start(want < 0 ? 0 : want);
            return hold.rgba.empty() ? nullptr : &hold;
        }
        while (queue.size() > 1 && queue[1].index <= want) queue.pop_front();
        if (!queue.empty() && queue.front().index <= want + 1) {
            hold = std::move(queue.front());
            queue.pop_front();
            cv.notify_all();
        }
        return hold.rgba.empty() ? nullptr : &hold;
    }
};

std::mutex g_mutex;
std::unique_ptr<Player> g_player;
Frame g_hold;
double g_game_fps = 29.97;
uint32_t g_game_frames = 0;
int64_t g_last_num = -1;
std::chrono::steady_clock::time_point g_num_at;
std::chrono::steady_clock::time_point g_open_at;

} // namespace

void video_hd_open(const std::string &path, uint32_t frames, uint32_t fps_num, uint32_t fps_den, uint32_t out_w, uint32_t out_h) {
    load_index();
    std::string name = std::filesystem::path(path).stem().string();
    auto it = g_index.find(lower(name));
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_player.reset();
    g_hold = {};
    if (it == g_index.end()) return;
    auto p = std::make_unique<Player>();
    p->entry = it->second;
    // saída: até 1920x1080 na proporção 16:9 do remaster, sem passar da janela
    uint32_t h = std::min<uint32_t>(1080, out_h ? out_h : 1080);
    h &= ~1u;
    p->h = h, p->w = (h * 16 / 9) & ~1u;
    g_game_fps = fps_den ? (double)fps_num / fps_den : 29.97;
    g_game_frames = frames;
    g_last_num = -1;
    g_open_at = std::chrono::steady_clock::now();
    if (!p->start((int64_t)(p->entry.offset * p->file_fps))) return;
    logf("vitart: cena remasterizada: %s -> %s (%ux%u)", name.c_str(), p->entry.file.c_str(), p->w, p->h);
    g_player = std::move(p);
}

void video_hd_close() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    g_player.reset();
    g_hold = {};
}

bool video_hd_frame(int64_t bink_frame, const uint8_t *&rgba, uint32_t &w, uint32_t &h) {
    const std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_player) return false;
    const auto now = std::chrono::steady_clock::now();
    double t;
    if (bink_frame >= 0) {
        if (bink_frame != g_last_num) g_last_num = bink_frame, g_num_at = now;
        const double since = std::chrono::duration<double>(now - g_num_at).count();
        t = (bink_frame + std::min(since * g_game_fps, 1.0)) / g_game_fps;
    } else {
        // o Bink ainda não começou: o jogo abre o vídeo e carrega a fase antes de tocá-lo (até ~13 s na
        // abertura); mostrar o quadro 0 do remaster nesse tempo parecia o vídeo travado. Fica a imagem do jogo.
        return false;
    }
    static int64_t logged_at = -1;
    if (bink_frame >= 0 && (logged_at < 0 || bink_frame - logged_at >= 300 || bink_frame < logged_at)) {
        logged_at = bink_frame;
        logf("vitart: cena remasterizada: quadro do Bink %lld -> %.2f s (%.1f s desde a abertura)", (long long)bink_frame, t,
             std::chrono::duration<double>(now - g_open_at).count());
    }
    const int64_t want = (int64_t)((t + g_player->entry.offset) * g_player->file_fps);
    const Frame *f = g_player->get(want, g_hold);
    if (!f) return false;
    // remaster mais curto que o vídeo do jogo: no fim do arquivo volta para a imagem do jogo
    if (g_player->eof && want > f->index + 6) return false;
    rgba = f->rgba.data(), w = g_player->w, h = g_player->h;
    return true;
}

bool video_hd_active() {
    const std::lock_guard<std::mutex> lock(g_mutex);
    return g_player != nullptr;
}

} // namespace vt
