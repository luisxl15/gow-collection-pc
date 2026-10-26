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

// barrinha de vida dos bichos, igual a do GoW3
bool enemy_bar(uint32_t im_w, uint32_t im_h, const EnemyBar &bar, std::vector<uint32_t> &px, uint32_t &bw, uint32_t &bh,
               int32_t &x0, int32_t &y0) {
    bh = std::max(6u, (uint32_t)(im_h * 0.016f + 0.5f)), bw = std::max(36u, (uint32_t)(im_h * 0.11f + 0.5f));
    const uint32_t edge = std::max(1u, bh / 4);
    x0 = (int32_t)(bar.x * im_w) - (int32_t)bw / 2, y0 = (int32_t)(bar.y * im_h) - (int32_t)bh;
    if (x0 < 0 || y0 < 0 || x0 + (int32_t)bw > (int32_t)im_w || y0 + (int32_t)bh > (int32_t)im_h) return false;
    const float frac = bar.frac < 0.f ? 0.f : bar.frac > 1.f ? 1.f : bar.frac;
    const uint32_t fill = edge + (uint32_t)((bw - 2 * edge) * frac + 0.5f);
    px.resize((size_t)bw * bh);
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t r, g, b;
            if (x < edge || y < edge || x >= bw - edge || y >= bh - edge) {
                r = 26, g = 16, b = 10;
            } else if (x < fill) {
                const float t = (float)(y - edge) / (float)std::max(1u, bh - 2 * edge - 1); // 0 em cima, 1 embaixo
                r = (uint32_t)(235 - 110 * t), g = (uint32_t)(40 - 25 * t), b = (uint32_t)(28 - 18 * t);
                if (y == edge) r = 255, g = 120, b = 90; // um brilhinho
            } else {
                r = 58, g = 10, b = 8; // parte que já apanhou
            }
            px[y * bw + x] = 0xFF000000u | b << 16 | g << 8 | r; // RGBA na memória
        }
    return true;
}

} // namespace gvk::ovl
