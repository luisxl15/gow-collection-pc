// aqui é só o porteiro: manda tudo pro Vulkan ou pro DX11 (detalhes em docs/renderizadores.md)
#include "gxm_vk.h"
#include "trophy_banner.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstring>
#include <string>

namespace vt {
void logf(const char *fmt, ...); // core.cpp
}

namespace gvk {
namespace vkb {
#include "gxm_api.inc"
}
namespace dx11 {
#include "gxm_api.inc"
}

namespace {
Backend g_backend = Backend::vulkan;
std::string g_want = "auto";
} // namespace

void select_backend(const char *name) { g_want = name && *name ? name : "auto"; }
Backend backend() { return g_backend; }
const char *backend_name() { return g_backend == Backend::d3d11 ? "Direct3D 11" : "Vulkan"; }

// tenta o Vulkan primeiro; se o PC torcer o nariz, vai de DX11 mesmo
bool init(bool validation, bool window) {
    if (g_want != "d3d11") {
        if (LoadLibraryA("vulkan-1.dll")) {
            g_backend = Backend::vulkan;
            if (vkb::init(validation, window)) return true;
            if (g_want == "vulkan") return false;
            vt::logf("vitart: Vulkan não iniciou (%s); usando o Direct3D 11", vkb::last_error());
        } else {
            if (g_want == "vulkan") return false;
            vt::logf("vitart: sem driver Vulkan (vulkan-1.dll); usando o Direct3D 11");
        }
    }
    g_backend = Backend::d3d11;
    return dx11::init(validation, window);
}

// daqui pra baixo é tudo repasse, nada de emocionante
#define TO(f, ...) (g_backend == Backend::d3d11 ? dx11::f(__VA_ARGS__) : vkb::f(__VA_ARGS__))

bool vertex_format_ok(uint32_t fmt, uint32_t comps) { return TO(vertex_format_ok, fmt, comps); }
bool present(uint32_t addr) { return TO(present, addr); }
bool window_closed() { return TO(window_closed); }
bool window_rect(int &x, int &y, int &w, int &h) { return TO(window_rect, x, y, w, h); }
bool pointer(float &nx, float &ny) { return TO(pointer, nx, ny); }
bool has_focus() { return TO(has_focus); }
void notify(const std::string &text) { TO(notify, text); }
bool is_target(uint32_t addr) { return TO(is_target, addr); }
bool is_depth(uint32_t addr) { return TO(is_depth, addr); }
void begin_scene(uint32_t color_addr, uint8_t stencil) { TO(begin_scene, color_addr, stencil); }
void shutdown() { TO(shutdown); }
uint32_t render_scale() { return TO(render_scale); }
uint32_t auto_render_scale() { return TO(auto_render_scale); }
bool set_render_scale(uint32_t s) { return TO(set_render_scale, s); }
void set_fps_counter(bool on) { TO(set_fps_counter, on); }
void draw_enemy_bars(uint32_t color_addr, const EnemyBar *bars, uint32_t n) { TO(draw_enemy_bars, color_addr, bars, n); }
uint32_t measured_fps() { return TO(measured_fps); }
void set_borderless(bool on) { TO(set_borderless, on); }
void set_vsync(bool on, bool fifo) { TO(set_vsync, on, fifo); }
uint32_t last_frame_depth_writes() { return TO(last_frame_depth_writes); }
void set_post_bypass(bool on) { TO(set_post_bypass, on); }
void post_stats(uint32_t out[6]) { TO(post_stats, out); }
void post_focus_values(float out[4]) { TO(post_focus_values, out); }
void set_video_frame(const uint8_t *rgba, uint32_t w, uint32_t h) { TO(set_video_frame, rgba, w, h); }
void notify_trophy(const TrophyBanner &t) { TO(notify_trophy, t); }
bool is_borderless() { return TO(is_borderless); }
bool draw(const Draw &d) { return TO(draw, d); }
void end_frame() { TO(end_frame); }
bool read_target(uint32_t addr, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) { return TO(read_target, addr, rgba, w, h); }
std::vector<uint32_t> targets() { return TO(targets); }
const char *last_error() { return TO(last_error); }
const std::vector<float> &debug_vs_output() { return TO(debug_vs_output); }
Stats take_stats() { return TO(take_stats); }
Stats peek_stats() { return TO(peek_stats); }

} // namespace gvk
