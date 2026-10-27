// coisinhas que a gente pinta por cima do jogo
#include "gxm_overlay.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace vt {
void logf(const char *fmt, ...); // core.cpp
}

namespace gvk::ovl {
namespace {

std::recursive_mutex g_lock; // duas threads mexendo aqui, então tranca
TrophyBanner g_trophy;
BannerImage g_trophy_img;
uint32_t g_trophy_for = 0;
bool g_trophy_on = false;
std::chrono::steady_clock::time_point g_trophy_t0;
std::vector<TrophyBanner> g_trophy_queue; // fila, um troféu de cada vez

void trophy_start_next() {
    if (g_trophy_queue.empty()) return;
    g_trophy = g_trophy_queue.front();
    g_trophy_queue.erase(g_trophy_queue.begin());
    vt::logf("vitart: aviso de troféu na tela (%zu na fila)", g_trophy_queue.size());
    g_trophy_for = 0, g_trophy_on = true;
    g_trophy_t0 = std::chrono::steady_clock::now();
    if (!g_trophy.sound.empty()) PlaySoundA(g_trophy.sound.c_str(), nullptr, SND_ASYNC | SND_FILENAME | SND_NODEFAULT);
    else MessageBeep(MB_ICONASTERISK);
}

std::atomic<bool> g_fps_counter{false};
std::atomic<uint32_t> g_fps_measured{0};

const uint8_t *glyph5x7(char c) {
    static const uint8_t digits[10][7] = {
        {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14},    {14, 17, 1, 2, 4, 8, 31},
        {31, 2, 4, 2, 1, 17, 14},     {2, 6, 10, 18, 31, 2, 2},   {31, 16, 30, 1, 1, 17, 14},
        {6, 8, 16, 30, 17, 17, 14},   {31, 1, 2, 4, 8, 8, 8},     {14, 17, 17, 14, 17, 17, 14},
        {14, 17, 17, 15, 1, 2, 12}};
    static const uint8_t F[7] = {31, 16, 16, 30, 16, 16, 16}, P[7] = {30, 17, 17, 30, 16, 16, 16},
                         S[7] = {15, 16, 16, 14, 1, 1, 30}, sp[7] = {};
    if (c >= '0' && c <= '9') return digits[c - '0'];
    return c == 'F' ? F : c == 'P' ? P : c == 'S' ? S : sp;
}

} // namespace

void trophy_push(const TrophyBanner &t) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_trophy_queue.push_back(t);
    if (!g_trophy_on) trophy_start_next();
}

bool trophy_frame(int32_t area_x, int32_t area_y, int32_t area_w, int32_t area_h, const BannerImage *&img, int32_t &x0,
                  int32_t &y0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_trophy_on) return false;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_trophy_t0).count();
    constexpr double kSlide = 0.25;
    const double kStay = g_trophy_queue.empty() ? 4.2 : 2.0; // se tem fila, anda logo
    if (t > kSlide * 2 + kStay) {
        g_trophy_on = false;
        trophy_start_next();
        return false;
    }
    if (g_trophy_for != (uint32_t)area_h) {
        if (!render_trophy_banner(g_trophy, (uint32_t)area_h, g_trophy_img)) return g_trophy_on = false;
        g_trophy_for = (uint32_t)area_h;
    }
    const BannerImage &bi = g_trophy_img;
    const double k = area_h / 544.0;
    const int32_t margin = (int32_t)(12 * k), x_in = area_x + area_w - (int32_t)bi.w - margin, x_out = area_x + area_w + margin;
    const double f = t < kSlide ? 1 - t / kSlide : t > kSlide + kStay ? (t - kSlide - kStay) / kSlide : 0; // 0 = no lugar
    x0 = x_in + (int32_t)((x_out - x_in) * f), y0 = area_y + (int32_t)(20 * k);
    img = &bi;
    return true;
}

void set_fps_counter(bool on) { g_fps_counter = on; }
uint32_t measured_fps() { return g_fps_measured; }

bool fps_frame(uint32_t screen_h, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h) {
    using clk = std::chrono::steady_clock;
    static clk::time_point t0 = clk::now();
    static uint32_t frames = 0;
    static double fps = 0;
    frames++;
    const double el = std::chrono::duration<double>(clk::now() - t0).count();
    if (el >= 0.5) fps = frames / el, frames = 0, t0 = clk::now(), g_fps_measured = (uint32_t)(fps + 0.5);
    if (!g_fps_counter) return false;
    char text[16];
    std::snprintf(text, sizeof text, "%d FPS", (int)(fps + 0.5));
    const uint32_t len = (uint32_t)std::strlen(text);
    uint32_t k = (screen_h + 135) / 270;
    k = k < 2 ? 2 : k > 6 ? 6 : k;
    w = (len * 6 + 3) * k, h = 11 * k;
    const uint32_t bg = 0xFF101010u, fg = 0xFF40FF40u; // verdinho estilo contador antigo
    px.assign((size_t)w * h, bg);
    for (uint32_t n = 0; n < len; n++) {
        const uint8_t *g = glyph5x7(text[n]);
        for (uint32_t r = 0; r < 7; r++)
            for (uint32_t c = 0; c < 5; c++)
                if (g[r] & (16 >> c))
                    for (uint32_t yy = 0; yy < k; yy++)
                        for (uint32_t xx = 0; xx < k; xx++) px[((2 + r) * k + yy) * w + (2 + n * 6 + c) * k + xx] = fg;
    }
    return true;
}

// barra de vida dos bichos: número, < filete dourado >, no jeito dos GoW novos
bool enemy_bar(uint32_t im_w, uint32_t im_h, const EnemyBar &bar, std::vector<uint32_t> &px, uint32_t &bw, uint32_t &bh,
               int32_t &x0, int32_t &y0, std::vector<Span> &spans) {
    const uint32_t th = std::max(3u, (uint32_t)(im_h * 0.0062f + 0.5f));  // espessura do filete
    const uint32_t len = th * 22;                                         // comprimento do filete
    const uint32_t ch = th * 3 | 1, cw = ch / 2 + 1;                      // seta: altura ímpar, meia largura
    const uint32_t st = std::max(1u, th * 2 / 3);                         // traço da seta
    const uint32_t k = std::max(1u, (ch + 3) / 7);                        // escala do número 5x7
    const uint32_t gap = th;
    char num[8] = {};
    if (bar.num >= 0) std::snprintf(num, sizeof num, "%d", bar.num > 9999 ? 9999 : bar.num);
    const uint32_t nd = (uint32_t)std::strlen(num);
    const uint32_t numw = nd ? nd * 6 * k - k : 0;
    const uint32_t pad = 1; // contorno escuro
    bw = pad * 2 + (nd ? numw + gap : 0) + cw + gap + len + gap + cw;
    bh = pad * 2 + std::max(ch, 7 * k);
    x0 = (int32_t)(bar.x * im_w) - (int32_t)bw / 2, y0 = (int32_t)(bar.y * im_h) - (int32_t)bh;
    if (x0 < 0 || y0 < 0 || x0 + (int32_t)bw > (int32_t)im_w || y0 + (int32_t)bh > (int32_t)im_h) return false;
    px.assign((size_t)bw * bh, 0);
    auto put = [&](uint32_t x, uint32_t y, uint32_t r, uint32_t g, uint32_t b) {
        if (x < bw && y < bh) px[(size_t)y * bw + x] = 0xFF000000u | b << 16 | g << 8 | r; // RGBA na memória
    };
    const uint32_t mid = bh / 2;
    uint32_t x = pad;
    // número
    for (uint32_t n = 0; n < nd; n++) {
        const uint8_t *gl = glyph5x7(num[n]);
        const uint32_t top = mid - 7 * k / 2;
        for (uint32_t r = 0; r < 7; r++)
            for (uint32_t c = 0; c < 5; c++)
                if (gl[r] & (16 >> c))
                    for (uint32_t yy = 0; yy < k; yy++)
                        for (uint32_t xx = 0; xx < k; xx++) put(x + n * 6 * k + c * k + xx, top + r * k + yy, 238, 234, 222);
    }
    if (nd) x += numw + gap;
    // setas: < na esquerda, > na direita
    auto chevron = [&](uint32_t left, bool open_right) {
        const uint32_t top = mid - ch / 2, half = ch / 2;
        for (uint32_t r = 0; r < ch; r++) {
            const uint32_t dy = r > half ? r - half : half - r;
            const uint32_t off = dy * (cw - st) / std::max(1u, half);
            for (uint32_t t = 0; t < st; t++) put(open_right ? left + off + t : left + cw - 1 - off - t, top + r, 236, 232, 220);
        }
    };
    chevron(x, true);
    x += cw + gap;
    // filete: ouro claro em cima, mais escuro embaixo; a parte perdida fica escura
    const float frac = bar.frac < 0.f ? 0.f : bar.frac > 1.f ? 1.f : bar.frac;
    const uint32_t fill = (uint32_t)(len * frac + 0.5f), top = mid - th / 2;
    for (uint32_t r = 0; r < th; r++) {
        const float t = th > 1 ? (float)r / (float)(th - 1) : 0.f;
        for (uint32_t c = 0; c < len; c++) {
            if (c < fill) put(x + c, top + r, (uint32_t)(246 - 70 * t), (uint32_t)(226 - 86 * t), (uint32_t)(160 - 100 * t));
            else put(x + c, top + r, 46, 38, 28);
        }
    }
    x += len + gap;
    chevron(x, false);
    // contorno de 1 px em volta do que foi desenhado, pra ler em cima de qualquer fundo
    std::vector<uint32_t> out = px;
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t xx = 0; xx < bw; xx++) {
            if (px[(size_t)y * bw + xx]) continue;
            bool touch = false;
            for (int dy = -1; dy <= 1 && !touch; dy++)
                for (int dx = -1; dx <= 1 && !touch; dx++) {
                    const int yy = (int)y + dy, xq = (int)xx + dx;
                    touch = yy >= 0 && xq >= 0 && yy < (int)bh && xq < (int)bw && px[(size_t)yy * bw + xq];
                }
            if (touch) out[(size_t)y * bw + xx] = 0xFF060a10u;
        }
    px.swap(out);
    spans.clear();
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t xx = 0; xx < bw;) {
            if (!px[(size_t)y * bw + xx]) {
                xx++;
                continue;
            }
            uint32_t e = xx;
            while (e < bw && px[(size_t)y * bw + e]) e++;
            spans.push_back({xx, y, e - xx});
            xx = e;
        }
    return true;
}

} // namespace gvk::ovl
