// Renderizador Direct3D 11 dos desenhos do GXM (ver gxm_vk.h), para PCs sem Vulkan: mesma entrada do renderizador
// Vulkan (gxm_vk.cpp) e mesmo resultado, com os shaders em DXBC shader model 5.0 (tools/usse_d3d.py, a partir do
// mesmo tradutor USSE). Sem GPU com Direct3D 11 (nível 11_0), usa o WARP, o rasterizador por software do Windows.
//
// Como no Vulkan, o alvo de cada desenho é o endereço da superfície de cor do guest; ler um alvo como textura
// usa uma cópia feita só quando ele mudou. Vértices e índices vão num anel de upload (buffer dinâmico); os
// uniforms de todos os desenhos ficam num buffer só, lido pelos shaders como ByteAddressBuffer, e o cbuffer b0 de
// cada desenho diz onde começam os dele (contrato em vitarecomp/usse/translate.py, D3D_*).
// Fica de fora o pós-processamento de tela (shaders de computação do Vulkan, desligado por padrão).
#include "gxm_vk.h"
#include "game.h"
#include "d3d11_blit.h"
#include "gxm_overlay.h"
#include "gxm_window.h"
#include "pbr.h"
#include "trophy_banner.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

namespace vt {
void logf(const char *fmt, ...); // core.cpp
}

namespace gvk::dx11 {
namespace {

using Microsoft::WRL::ComPtr;

std::string g_err;
ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11RenderTargetView> g_bb_rtv;
uint32_t g_bb_w = 0, g_bb_h = 0;
bool g_flip_model = false;
std::recursive_mutex g_lock; // desenhos vêm da thread de render do jogo, a apresentação da de vídeo
Stats g_stats;
constexpr uint32_t kTexSlots = 16;

bool fail(const char *what, HRESULT hr) {
    char b[160];
    std::snprintf(b, sizeof b, "%s falhou (HRESULT %08lx)", what, (unsigned long)hr);
    g_err = b;
    return false;
}
#define DXC(call)                                    \
    do {                                             \
        HRESULT hr_ = (call);                        \
        if (FAILED(hr_)) return fail(#call, hr_);    \
    } while (0)

uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Timer {
    uint64_t &acc, t0 = now_ns();
    explicit Timer(uint64_t &a) : acc(a) {}
    ~Timer() { acc += now_ns() - t0; }
};

uint64_t fnv(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

// Resolução interna: a mesma regra do Vulkan (altura nativa 408 vezes a escala cobre a tela, entre 2 e 4).
uint32_t g_scale_override = 0;
uint32_t scale_auto() {
    static const uint32_t s = [] {
        const char *e = getenv("VITART_VK_SCALE");
        int v = e ? std::atoi(e) : 0;
        if (!v) {
            const int mh = GetSystemMetrics(SM_CYSCREEN);
            v = (mh + 407) / 408;
            v = v < 2 ? 2 : v > 4 ? 4 : v;
        }
        return (uint32_t)(v < 1 ? 1 : v > 8 ? 8 : v);
    }();
    return s;
}
uint32_t scale_now() { return g_scale_override ? g_scale_override : scale_auto(); }

// ------------------------------------------------------------------ imagens
struct Tex {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    uint32_t w = 0, h = 0;
};

bool make_tex(Tex &t, uint32_t w, uint32_t h, DXGI_FORMAT fmt, UINT bind, uint32_t mips = 1, const void *rgba = nullptr) {
    Timer tm(g_stats.alloc_ns);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w, td.Height = h, td.MipLevels = mips, td.ArraySize = 1, td.Format = fmt;
    td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = bind;
    // mips 0 = a cadeia inteira, gerada na GPU a partir do nível 0
    if (mips != 1) td.BindFlags |= D3D11_BIND_RENDER_TARGET, td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    D3D11_SUBRESOURCE_DATA sd{rgba, w * 4, 0};
    DXC(g_dev->CreateTexture2D(&td, rgba && mips == 1 ? &sd : nullptr, &t.tex));
    if (bind & D3D11_BIND_SHADER_RESOURCE) DXC(g_dev->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv));
    if (rgba && mips != 1) {
        g_ctx->UpdateSubresource(t.tex.Get(), 0, nullptr, rgba, w * 4, 0);
        g_ctx->GenerateMips(t.srv.Get());
    }
    t.w = w, t.h = h;
    return true;
}

// Alvo do jogo: cor RGBA8 e profundidade D32 + S8 (a vista de amostragem só tem a profundidade).
struct Target {
    Tex color;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11Texture2D> depth;
    ComPtr<ID3D11DepthStencilView> dsv;
    uint64_t cver = 1, dver = 1; // sobem a cada escrita na cor / na profundidade
};
std::map<uint32_t, Target> g_targets;
std::map<uint32_t, uint32_t> g_depth_owner; // superfície de profundidade -> alvo de cor que a usa
Target *g_open = nullptr;                   // alvo ligado ao output merger

bool make_target(Target &t, uint32_t w, uint32_t h) {
    if (!make_tex(t.color, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE)) return false;
    DXC(g_dev->CreateRenderTargetView(t.color.tex.Get(), nullptr, &t.rtv));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w, td.Height = h, td.MipLevels = 1, td.ArraySize = 1, td.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
    td.SampleDesc.Count = 1, td.Usage = D3D11_USAGE_DEFAULT, td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    DXC(g_dev->CreateTexture2D(&td, nullptr, &t.depth));
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT, dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    DXC(g_dev->CreateDepthStencilView(t.depth.Get(), &dd, &t.dsv));
    const float black[4] = {0, 0, 0, 0};
    g_ctx->ClearRenderTargetView(t.rtv.Get(), black);
    g_ctx->ClearDepthStencilView(t.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    return true;
}

bool get_target(uint32_t addr, uint32_t w, uint32_t h, Target *&out) {
    auto it = g_targets.find(addr);
    if (it != g_targets.end()) return out = &it->second, true;
    Target t;
    if (!make_target(t, w * scale_now(), h * scale_now())) return false;
    out = &(g_targets[addr] = std::move(t));
    return true;
}

// Cópia amostrável de um alvo (cor ou profundidade), refeita só quando o alvo mudou desde a última cópia.
struct Snap {
    Tex im;
    uint64_t ver = 0;
};
std::map<std::pair<uint32_t, bool>, Snap> g_snaps;

ID3D11ShaderResourceView *snapshot(Target &t, uint32_t owner, bool depth) {
    Snap &sn = g_snaps[{owner, depth}];
    const uint64_t ver = depth ? t.dver : t.cver;
    if (sn.im.tex && sn.ver == ver) return sn.im.srv.Get();
    if (!sn.im.tex) {
        D3D11_TEXTURE2D_DESC td{};
        (depth ? t.depth : t.color.tex)->GetDesc(&td);
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE, td.MiscFlags = 0;
        if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &sn.im.tex))) return nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = depth ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, sd.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateShaderResourceView(sn.im.tex.Get(), &sd, &sn.im.srv))) return nullptr;
        sn.im.w = td.Width, sn.im.h = td.Height;
    }
    g_ctx->CopyResource(sn.im.tex.Get(), depth ? t.depth.Get() : t.color.tex.Get());
    sn.ver = ver;
    g_stats.snapshots++;
    return sn.im.srv.Get();
}

std::unordered_map<uint64_t, Tex> g_textures;
std::unordered_map<uint64_t, uint64_t> g_tex_used; // textura -> último quadro que a usou
uint64_t g_frame_no = 0;
Tex g_white, g_pbr_flat, g_pbr_none;

// ------------------------------------------------------------------ amostragem e estado fixo
std::map<uint32_t, ComPtr<ID3D11SamplerState>> g_samplers;
ComPtr<ID3D11SamplerState> g_sampler_linear, g_sampler_nearest;
float g_aniso = 16.f;
D3D11_TEXTURE_ADDRESS_MODE addr_mode(uint32_t m) {
    switch (m) {
    case 0: case 4: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 5: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_CLAMP; // clamp, mirror clamp e as bordas
    }
}
ID3D11SamplerState *sampler_for(const Texture &t) {
    const bool mipped = t.mip_count != 0 && t.rgba;
    const uint32_t key = (t.mag & 1) | (t.min & 1) << 1 | (t.addr_u & 7) << 2 | (t.addr_v & 7) << 5 | (mipped ? 1u : 0u) << 8 |
                         (t.mip_filter & 1) << 9 | (uint32_t)(t.lod_bias & 63) << 10;
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second.Get();
    D3D11_SAMPLER_DESC sd{};
    const bool lmin = t.min & 1, lmag = t.mag & 1, lmip = mipped && (t.mip_filter & 1);
    sd.Filter = (D3D11_FILTER)((lmip ? 0x1 : 0) | (lmag ? 0x4 : 0) | (lmin ? 0x10 : 0));
    if (mipped && g_aniso > 1.f && lmin) sd.Filter = D3D11_FILTER_ANISOTROPIC, sd.MaxAnisotropy = (UINT)g_aniso;
    sd.AddressU = addr_mode(t.addr_u), sd.AddressV = addr_mode(t.addr_v), sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MipLODBias = mipped ? ((float)(t.lod_bias & 63) - 31.f) / 8.f : 0.f;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = mipped ? D3D11_FLOAT32_MAX : 0.25f;
    ComPtr<ID3D11SamplerState> s;
    if (FAILED(g_dev->CreateSamplerState(&sd, &s))) s = g_sampler_linear;
    return (g_samplers[key] = s).Get();
}

D3D11_BLEND factor(uint32_t f) {
    static const D3D11_BLEND t[12] = {D3D11_BLEND_ZERO,         D3D11_BLEND_ONE,           D3D11_BLEND_SRC_COLOR,
                                      D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_SRC_ALPHA,     D3D11_BLEND_INV_SRC_ALPHA,
                                      D3D11_BLEND_DEST_COLOR,    D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_DEST_ALPHA,
                                      D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_SRC_ALPHA_SAT, D3D11_BLEND_ONE};
    return f < 12 ? t[f] : D3D11_BLEND_ONE;
}
// Fator de alfa: as variantes "COLOR" não valem no canal alfa do D3D11 (viram a de alfa correspondente).
D3D11_BLEND alpha_factor(uint32_t f) {
    switch (factor(f)) {
    case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default: return factor(f);
    }
}
D3D11_BLEND_OP blend_op(uint32_t f) {
    switch (f) {
    case 2: return D3D11_BLEND_OP_SUBTRACT;
    case 3: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D11_BLEND_OP_MIN;
    case 5: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

std::unordered_map<uint32_t, ComPtr<ID3D11BlendState>> g_blends;
ID3D11BlendState *blend_state(const Draw &d) {
    static const bool noblend = getenv("VITART_VK_NOBLEND") != nullptr; // depuração, como no Vulkan
    const uint32_t key = d.has_blend && !noblend ? d.blend : 0xFFFFFFFFu;
    auto it = g_blends.find(key);
    if (it != g_blends.end()) return it->second.Get();
    D3D11_BLEND_DESC bd{};
    D3D11_RENDER_TARGET_BLEND_DESC &rt = bd.RenderTarget[0];
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    if (key != 0xFFFFFFFFu) {
        const uint32_t b = key, mask = b & 0xFF, cfunc = (b >> 8) & 15, afunc = (b >> 12) & 15;
        rt.RenderTargetWriteMask = (UINT8)(((mask & 2) ? D3D11_COLOR_WRITE_ENABLE_RED : 0) | ((mask & 4) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0) |
                                           ((mask & 8) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0) | ((mask & 1) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
        rt.BlendEnable = (cfunc || afunc) ? TRUE : FALSE;
        rt.BlendOp = blend_op(cfunc), rt.BlendOpAlpha = blend_op(afunc);
        if (cfunc) rt.SrcBlend = factor((b >> 16) & 15), rt.DestBlend = factor((b >> 20) & 15);
        if (afunc) rt.SrcBlendAlpha = alpha_factor((b >> 24) & 15), rt.DestBlendAlpha = alpha_factor((b >> 28) & 15);
    }
    ComPtr<ID3D11BlendState> s;
    g_dev->CreateBlendState(&bd, &s);
    return (g_blends[key] = s).Get();
}

// Função e operações de stencil de uma face (mesma ordem do VkCompareOp/VkStencilOp; o D3D11 começa em 1).
uint32_t stencil_bits(const Stencil &st) { return (st.func & 7) | (st.fail & 7) << 3 | (st.zfail & 7) << 6 | (st.zpass & 7) << 9; }
bool stencil_used(const Draw &d) {
    auto idle = [](const Stencil &st) { return st.func == 7 && !st.fail && !st.zfail && !st.zpass; };
    return d.stencil && !(idle(d.front) && idle(d.back));
}
D3D11_DEPTH_STENCILOP_DESC stencil_face(const Stencil &st) {
    D3D11_DEPTH_STENCILOP_DESC o{};
    o.StencilFailOp = (D3D11_STENCIL_OP)((st.fail & 7) + 1);
    o.StencilDepthFailOp = (D3D11_STENCIL_OP)((st.zfail & 7) + 1);
    o.StencilPassOp = (D3D11_STENCIL_OP)((st.zpass & 7) + 1);
    o.StencilFunc = (D3D11_COMPARISON_FUNC)((st.func & 7) + 1);
    return o;
}
std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> g_depths;
ID3D11DepthStencilState *depth_state(const Draw &d) {
    static const bool nodepth = getenv("VITART_VK_NODEPTH") != nullptr; // depuração, como no Vulkan
    const bool sten = stencil_used(d);
    const uint64_t key = (uint64_t)(d.depth_func & 7) | (d.depth_write ? 8u : 0u) | (nodepth ? 16u : 0u) |
                         (sten ? (1ull << 5 | (uint64_t)stencil_bits(d.front) << 6 | (uint64_t)stencil_bits(d.back) << 18 |
                                  (uint64_t)d.front.cmask << 30 | (uint64_t)d.front.wmask << 38)
                              : 0);
    auto it = g_depths.find(key);
    if (it != g_depths.end()) return it->second.Get();
    D3D11_DEPTH_STENCIL_DESC dd{};
    dd.DepthEnable = nodepth ? FALSE : TRUE;
    dd.DepthWriteMask = d.depth_write ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dd.DepthFunc = (D3D11_COMPARISON_FUNC)((d.depth_func & 7) + 1);
    if (sten) {
        // o D3D11 tem uma máscara só para as duas faces: a da frente (as duas são iguais no GoW)
        dd.StencilEnable = TRUE;
        dd.StencilReadMask = d.front.cmask, dd.StencilWriteMask = d.front.wmask;
        dd.FrontFace = stencil_face(d.front), dd.BackFace = stencil_face(d.back);
    }
    ComPtr<ID3D11DepthStencilState> s;
    g_dev->CreateDepthStencilState(&dd, &s);
    return (g_depths[key] = s).Get();
}
ComPtr<ID3D11RasterizerState> g_raster;

// ------------------------------------------------------------------ shaders e entrada de vértices
std::unordered_map<const void *, ComPtr<ID3D11VertexShader>> g_vs;
std::unordered_map<const void *, ComPtr<ID3D11PixelShader>> g_ps;
std::unordered_map<uint64_t, ComPtr<ID3D11InputLayout>> g_layouts;
ComPtr<ID3D11VertexShader> g_blit_vs;
ComPtr<ID3D11PixelShader> g_blit_ps;

// O chamador mantém o DXBC vivo e no mesmo endereço (cache por programa): o ponteiro basta como chave.
ID3D11VertexShader *vertex_shader(const uint32_t *code, uint32_t size) {
    auto it = g_vs.find(code);
    if (it != g_vs.end()) return it->second.Get();
    Timer tm(g_stats.pipe_ns);
    ComPtr<ID3D11VertexShader> s;
    if (FAILED(g_dev->CreateVertexShader(code, size, nullptr, &s))) s = nullptr;
    g_stats.pipes++;
    return (g_vs[code] = s).Get();
}
ID3D11PixelShader *pixel_shader(const uint32_t *code, uint32_t size) {
    auto it = g_ps.find(code);
    if (it != g_ps.end()) return it->second.Get();
    Timer tm(g_stats.pipe_ns);
    ComPtr<ID3D11PixelShader> s;
    if (FAILED(g_dev->CreatePixelShader(code, size, nullptr, &s))) s = nullptr;
    g_stats.pipes++;
    return (g_ps[code] = s).Get();
}

// Formatos de vértice que o D3D11 lê como float: os normalizados e os de ponto flutuante. Os inteiros sem
// normalização do GXM (U8, S8, U16, S16) e os de 3 componentes de 8/16 bits não existem: vão convertidos na CPU.
DXGI_FORMAT vertex_format(uint32_t fmt, uint32_t comps) {
    static const DXGI_FORMAT t[10][4] = {
        {}, {}, {}, {},
        {DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_UNORM},
        {DXGI_FORMAT_R8_SNORM, DXGI_FORMAT_R8G8_SNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R8G8B8A8_SNORM},
        {DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_UNORM},
        {DXGI_FORMAT_R16_SNORM, DXGI_FORMAT_R16G16_SNORM, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_SNORM},
        {DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R16G16B16A16_FLOAT},
        {DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT}};
    if (fmt > 9 || comps < 1 || comps > 4) return DXGI_FORMAT_UNKNOWN;
    return t[fmt][comps - 1];
}

// ------------------------------------------------------------------ anéis de upload
// Um buffer dinâmico para vértices e índices e outro para os uniforms (lido como ByteAddressBuffer). Os dados
// de cada desenho vão no fim do que já foi escrito no quadro (NO_OVERWRITE); quando o anel enche, DISCARD dá
// um buffer novo ao driver e recomeça do zero.
struct Ring {
    ComPtr<ID3D11Buffer> buf;
    ComPtr<ID3D11ShaderResourceView> srv;
    UINT size = 0, off = 0;
    bool no_overwrite = true;
};
Ring g_vring, g_uring;
ComPtr<ID3D11Buffer> g_cb; // b0: bases dos uniforms deste desenho e o sinal de y

bool make_ring(Ring &r, UINT size, UINT bind) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = size, bd.Usage = D3D11_USAGE_DYNAMIC, bd.BindFlags = bind, bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (bind & D3D11_BIND_SHADER_RESOURCE) bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
    DXC(g_dev->CreateBuffer(&bd, nullptr, &r.buf));
    if (bind & D3D11_BIND_SHADER_RESOURCE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS, sd.ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX;
        sd.BufferEx.NumElements = size / 4, sd.BufferEx.Flags = D3D11_BUFFEREX_SRV_FLAG_RAW;
        DXC(g_dev->CreateShaderResourceView(r.buf.Get(), &sd, &r.srv));
    }
    r.size = size, r.off = size; // o primeiro put começa com DISCARD
    return true;
}

// Escreve no anel os pedaços dados, alinhados; devolve os deslocamentos em bytes. ok=false se não couber.
struct Piece {
    const void *data;
    UINT size, align;
};
bool ring_put(Ring &r, const Piece *pieces, size_t n, UINT *offs) {
    Timer tm(g_stats.ring_ns);
    UINT need = 0;
    for (size_t i = 0; i < n; i++) need += pieces[i].size + pieces[i].align + 16;
    if (need > r.size) return g_err = "upload maior que o anel", false;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (!r.no_overwrite || r.off + need > r.size) mode = D3D11_MAP_WRITE_DISCARD, r.off = 0;
    D3D11_MAPPED_SUBRESOURCE m;
    DXC(g_ctx->Map(r.buf.Get(), 0, mode, 0, &m));
    uint8_t *p = static_cast<uint8_t *>(m.pData);
    for (size_t i = 0; i < n; i++) {
        r.off = (r.off + pieces[i].align - 1) / pieces[i].align * pieces[i].align;
        offs[i] = r.off;
        if (pieces[i].data && pieces[i].size) std::memcpy(p + r.off, pieces[i].data, pieces[i].size);
        else std::memset(p + r.off, 0, 16);
        r.off += pieces[i].size ? pieces[i].size : 16;
    }
    g_ctx->Unmap(r.buf.Get(), 0);
    return true;
}

bool upload_texture(Tex &t, const Texture &src, bool mipped) {
    g_stats.uploads++;
    return make_tex(t, src.width, src.height, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE,
                    mipped ? 0 : 1, src.rgba);
}

// Alvo e viewport ligados ao output merger.
void bind_target(Target *t) {
    if (g_open == t) return;
    ID3D11RenderTargetView *rtv = t ? t->rtv.Get() : nullptr;
    g_ctx->OMSetRenderTargets(1, &rtv, t ? t->dsv.Get() : nullptr);
    g_open = t;
}

// Cópia com escala de uma imagem para um alvo de render (apresentação, troca de resolução).
void blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst, float x, float y, float w, float h) {
    g_open = nullptr;
    g_ctx->OMSetRenderTargets(1, &dst, nullptr);
    const D3D11_VIEWPORT vp{x, y, w, h, 0.f, 1.f};
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_blit_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_blit_ps.Get(), nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, &src);
    ID3D11SamplerState *s = g_sampler_linear.Get();
    g_ctx->PSSetSamplers(0, 1, &s);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(nullptr, 0);
    g_ctx->RSSetState(g_raster.Get());
    g_ctx->Draw(3, 0);
    ID3D11ShaderResourceView *none = nullptr;
    g_ctx->PSSetShaderResources(0, 1, &none);
}

// Leque -> lista (o D3D11 não tem leque): (0, i, i+1) para cada i.
std::vector<uint32_t> fan_to_list(const Draw &d) {
    std::vector<uint32_t> out;
    auto at = [&](uint32_t i) -> uint32_t {
        if (!d.index_data) return d.indices[i];
        return d.index_size == 2 ? static_cast<const uint16_t *>(d.index_data)[i] : static_cast<const uint32_t *>(d.index_data)[i];
    };
    for (uint32_t i = 1; i + 1 < d.index_count; i++) out.push_back(at(0)), out.push_back(at(i)), out.push_back(at(i + 1));
    return out;
}

// Vértices crus -> vec4 float por location (quando o D3D11 não tem o formato ou recusa o layout).
float attr_value(const uint8_t *p, uint32_t fmt, uint32_t i) {
    switch (fmt) {
    case 0: return p[i];
    case 1: return (int8_t)p[i];
    case 2: return reinterpret_cast<const uint16_t *>(p)[i];
    case 3: return reinterpret_cast<const int16_t *>(p)[i];
    case 4: return p[i] / 255.0f;
    case 5: return std::max((int8_t)p[i] / 127.0f, -1.f);
    case 6: return reinterpret_cast<const uint16_t *>(p)[i] / 65535.0f;
    case 7: return std::max(reinterpret_cast<const int16_t *>(p)[i] / 32767.0f, -1.f);
    case 8: {
        const uint16_t hb = reinterpret_cast<const uint16_t *>(p)[i];
        const uint32_t sgn = hb >> 15, ex = (hb >> 10) & 31, mt = hb & 1023;
        const float v = ex == 0 ? std::ldexp((float)mt, -24) : ex == 31 ? INFINITY : std::ldexp((float)(mt | 1024), (int)ex - 25);
        return sgn ? -v : v;
    }
    default: return reinterpret_cast<const float *>(p)[i];
    }
}
std::vector<float> expand_raw(const Draw &d, uint32_t &nloc) {
    nloc = 0;
    for (const VertexAttr &a : d.raw_attrs) nloc = std::max(nloc, a.location + 1);
    std::vector<float> out((size_t)nloc * d.vertex_count * 4, 0.f);
    for (const VertexAttr &a : d.raw_attrs) {
        const VertexStream &st = d.raw_streams[a.stream];
        for (uint32_t v = 0; v < d.vertex_count; v++) {
            const uint8_t *p = st.data + (size_t)v * st.stride + a.offset;
            float *o = &out[((size_t)a.location * d.vertex_count + v) * 4];
            o[3] = 1.f;
            for (uint32_t c = 0; c < a.comps; c++) o[c] = attr_value(p, a.fmt, c);
        }
    }
    return out;
}

std::atomic<uint32_t> g_dw_frame{0}, g_dw_last{0};
std::atomic<bool> g_vsync{true}, g_fifo{false};

// Swapchain na janela comum: troca por "flip" (Windows 10) ou a antiga por cópia (Windows 7/8).
bool make_backbuffer() {
    g_bb_rtv.Reset();
    ComPtr<ID3D11Texture2D> bb;
    DXC(g_swap->GetBuffer(0, IID_PPV_ARGS(&bb)));
    DXC(g_dev->CreateRenderTargetView(bb.Get(), nullptr, &g_bb_rtv));
    D3D11_TEXTURE2D_DESC td;
    bb->GetDesc(&td);
    g_bb_w = td.Width, g_bb_h = td.Height;
    return true;
}
bool create_swapchain() {
    if (!win::create((std::wstring(vt::game_window_title()) + L" (vitart, Direct3D 11)").c_str())) return g_err = "CreateWindowEx falhou", false;
    ComPtr<IDXGIDevice> xdev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory> factory;
    DXC(g_dev.As(&xdev));
    DXC(xdev->GetAdapter(&adapter));
    DXC(adapter->GetParent(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = static_cast<HWND>(win::hwnd());
    sd.Windowed = TRUE;
    sd.BufferCount = 2, sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    g_flip_model = SUCCEEDED(factory->CreateSwapChain(g_dev.Get(), &sd, &g_swap));
    if (!g_flip_model) {
        sd.BufferCount = 1, sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        DXC(factory->CreateSwapChain(g_dev.Get(), &sd, &g_swap));
    }
    factory->MakeWindowAssociation(sd.OutputWindow, DXGI_MWA_NO_ALT_ENTER); // Alt+Enter é da janela (sem bordas)
    return make_backbuffer();
}

// Cena remasterizada (video_hd.cpp): imagem do próximo present no lugar do alvo do jogo.
const uint8_t *g_vid_px = nullptr;
uint32_t g_vid_w = 0, g_vid_h = 0;
Tex g_vid;

// Copia uma imagem BGRA (aviso de troféu, FPS) para o backbuffer, cortada na borda; linhas [from, to) por linha.
void put_bgra(ID3D11Resource *dst, int32_t x0, int32_t y0, const uint32_t *px, uint32_t w, uint32_t h, uint32_t y, uint32_t rows,
              uint32_t from, uint32_t to) {
    const int32_t vis = std::min<int32_t>((int32_t)to, (int32_t)g_bb_w - x0);
    if (vis <= (int32_t)from || x0 + (int32_t)from < 0 || y0 + (int32_t)(y + rows) > (int32_t)g_bb_h || y0 < 0) return;
    const D3D11_BOX box{(UINT)(x0 + from), (UINT)(y0 + y), 0, (UINT)(x0 + vis), (UINT)(y0 + y + rows), 1};
    g_ctx->UpdateSubresource(dst, 0, &box, px + (size_t)y * w + from, w * 4, 0);
}

} // namespace

const char *last_error() { return g_err.c_str(); }

bool vertex_format_ok(uint32_t fmt, uint32_t comps) { return vertex_format(fmt, comps) != DXGI_FORMAT_UNKNOWN; }

bool init(bool validation, bool window) {
    (void)window; // a janela nasce na primeira apresentação (a thread de vídeo é a que recebe as mensagens)
    const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (validation ? D3D11_CREATE_DEVICE_DEBUG : 0);
    static const bool force_warp = getenv("VITART_D3D11_WARP") != nullptr;
    HRESULT hr = E_FAIL;
    for (int warp = force_warp ? 1 : 0; warp < 2 && FAILED(hr); warp++) {
        const D3D_DRIVER_TYPE type = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        hr = D3D11CreateDevice(nullptr, type, nullptr, flags, want, 2, D3D11_SDK_VERSION, &g_dev, &got, &g_ctx);
        if (FAILED(hr) && flags & D3D11_CREATE_DEVICE_DEBUG) // sem as camadas de depuração instaladas
            hr = D3D11CreateDevice(nullptr, type, nullptr, flags & ~D3D11_CREATE_DEVICE_DEBUG, want, 2, D3D11_SDK_VERSION, &g_dev,
                                   &got, &g_ctx);
        if (SUCCEEDED(hr)) {
            ComPtr<IDXGIDevice> xdev;
            ComPtr<IDXGIAdapter> ad;
            DXGI_ADAPTER_DESC desc{};
            if (SUCCEEDED(g_dev.As(&xdev)) && SUCCEEDED(xdev->GetAdapter(&ad))) ad->GetDesc(&desc);
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, nullptr, nullptr);
            vt::logf("vitart: Direct3D 11 em %s (nível %x.%x%s)", name, got >> 12, (got >> 8) & 15, warp ? ", WARP: por software" : "");
        }
    }
    if (FAILED(hr)) return fail("D3D11CreateDevice (nível 11_0, nem o WARP)", hr);
    {
        // NO_OVERWRITE em buffer dinâmico lido pelo shader é do D3D11.1; sem ele os uniforms vão com DISCARD.
        D3D11_FEATURE_DATA_D3D11_OPTIONS o{};
        if (FAILED(g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof o)) || !o.MapNoOverwriteOnDynamicBufferSRV)
            g_uring.no_overwrite = false;
    }
    if (const char *e = getenv("VITART_VK_ANISO")) g_aniso = (float)std::atof(e);
    g_aniso = std::min(g_aniso, 16.f);
    if (!make_ring(g_vring, 64u << 20, D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER) ||
        !make_ring(g_uring, (g_uring.no_overwrite ? 32u : 1u) << 20, D3D11_BIND_SHADER_RESOURCE))
        return false;
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = 16, cb.Usage = D3D11_USAGE_DYNAMIC, cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER, cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    DXC(g_dev->CreateBuffer(&cb, nullptr, &g_cb));
    D3D11_RASTERIZER_DESC rd{};
    // Frente = horária na tela, como no Vulkan (medido nas sombras por volume do GoW); o culling é desligado.
    rd.FillMode = D3D11_FILL_SOLID, rd.CullMode = D3D11_CULL_NONE, rd.FrontCounterClockwise = getenv("VITART_VK_FRONT_CCW") ? TRUE : FALSE;
    rd.DepthClipEnable = TRUE;
    DXC(g_dev->CreateRasterizerState(&rd, &g_raster));
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = 0.25f;
    DXC(g_dev->CreateSamplerState(&sd, &g_sampler_linear));
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    DXC(g_dev->CreateSamplerState(&sd, &g_sampler_nearest));
    DXC(g_dev->CreateVertexShader(kBlitVS, sizeof kBlitVS, nullptr, &g_blit_vs));
    DXC(g_dev->CreatePixelShader(kBlitPS, sizeof kBlitPS, nullptr, &g_blit_ps));
    const uint8_t white[4] = {255, 255, 255, 255}, flat[4] = {128, 128, 255, 255}, none[4] = {255, 204, 0, 0};
    return make_tex(g_white, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, 1, white) &&
           make_tex(g_pbr_flat, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, 1, flat) &&
           make_tex(g_pbr_none, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE, 1, none);
}

bool window_closed() { return win::closed(); }
bool window_rect(int &x, int &y, int &w, int &h) { return win::rect(x, y, w, h); }
bool pointer(float &nx, float &ny) { return win::pointer(nx, ny); }
bool has_focus() { return win::focused(); }
void notify(const std::string &text) { win::notify(text); }
void set_borderless(bool on) { win::request_borderless(on); }
bool is_borderless() { return win::borderless(); }
void notify_trophy(const TrophyBanner &t) { ovl::trophy_push(t); }
void set_fps_counter(bool on) { ovl::set_fps_counter(on); }
uint32_t measured_fps() { return ovl::measured_fps(); }
uint32_t last_frame_depth_writes() { return g_dw_last; }
void set_vsync(bool on, bool fifo) { g_vsync = on, g_fifo = fifo; }
// O pós-processamento de tela é do Vulkan (shaders de computação): aqui fica sempre desligado.
void set_post_bypass(bool) {}
void post_stats(uint32_t out[6]) { std::memset(out, 0, 6 * sizeof(uint32_t)); }
void post_focus_values(float out[4]) { std::memset(out, 0, 4 * sizeof(float)); }
void set_video_frame(const uint8_t *rgba, uint32_t w, uint32_t h) { g_vid_px = rgba, g_vid_w = w, g_vid_h = h; }

bool is_target(uint32_t addr) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_targets.count(addr) != 0;
}
bool is_depth(uint32_t addr) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_depth_owner.count(addr) != 0;
}

void begin_scene(uint32_t color_addr, uint8_t stencil) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_targets.find(color_addr);
    if (!g_dev || it == g_targets.end()) return;
    g_ctx->ClearDepthStencilView(it->second.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, stencil);
    it->second.dver++;
}

void draw_enemy_bars(uint32_t color_addr, const EnemyBar *bars, uint32_t n) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_targets.find(color_addr);
    if (!g_dev || !n || it == g_targets.end()) return;
    Tex &im = it->second.color;
    std::vector<uint32_t> px;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t bw, bh;
        int32_t x0, y0;
        if (!ovl::enemy_bar(im.w, im.h, bars[i], px, bw, bh, x0, y0)) continue;
        const D3D11_BOX box{(UINT)x0, (UINT)y0, 0, (UINT)x0 + bw, (UINT)y0 + bh, 1};
        g_ctx->UpdateSubresource(im.tex.Get(), 0, &box, px.data(), bw * 4, 0);
    }
    it->second.cver++;
}

bool draw(const Draw &d) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!d.vs || !d.fs || !d.index_count || !d.vertex_count || !d.target_w || !d.target_h) return true;
    ID3D11VertexShader *vs = vertex_shader(d.vs, d.vs_size);
    ID3D11PixelShader *ps = pixel_shader(d.fs, d.fs_size);
    static ComPtr<ID3D11PixelShader> magenta; // depuração: VITART_D3D11_MAGENTA
    if (getenv("VITART_D3D11_MAGENTA") && d.depth_write && d.depth_func != 7 && (magenta || SUCCEEDED(g_dev->CreatePixelShader(kMagentaPS, sizeof kMagentaPS, nullptr, &magenta))))
        ps = magenta.Get();
    if (!vs || !ps) return g_err = "shader DXBC recusado", false;
    Target *tgt;
    if (!get_target(d.target_addr, d.target_w, d.target_h, tgt)) return false;
    if (d.depth_addr) g_depth_owner[d.depth_addr] = d.target_addr;
    if (d.depth_write) g_dw_frame++;

    // Texturas: imagens enviadas (cache por conteúdo) ou cópia de um alvo.
    ID3D11ShaderResourceView *srv[kTexSlots];
    ID3D11SamplerState *smp[kTexSlots];
    for (uint32_t i = 0; i < kTexSlots; i++) srv[i] = g_white.srv.Get(), smp[i] = g_sampler_linear.Get();
    static const bool skip_depth_tex = getenv("VITART_D3D11_NODEPTHTEX") != nullptr; // depuração
    for (const Texture &t : d.textures) {
        if (t.binding >= kTexSlots) continue;
        if (skip_depth_tex && t.depth) return true;
        smp[t.binding] = t.depth ? g_sampler_nearest.Get() : sampler_for(t); // profundidade sempre por ponto
        if (t.target_addr) {
            uint32_t owner = t.target_addr;
            if (t.depth) {
                auto o = g_depth_owner.find(t.target_addr);
                if (o == g_depth_owner.end()) continue;
                owner = o->second;
            }
            auto it = g_targets.find(owner);
            if (it == g_targets.end() || (!t.depth && it->first == d.target_addr)) continue;
            if (ID3D11ShaderResourceView *s = snapshot(it->second, owner, t.depth)) srv[t.binding] = s;
            continue;
        }
        static const bool white_tex = getenv("VITART_D3D11_WHITETEX") != nullptr; // depuração
        if (!t.rgba || white_tex) continue;
        const uint64_t h = t.key ? t.key : fnv(t.rgba, (size_t)t.width * t.height * 4, fnv(&t.width, 8));
        auto it = g_textures.find(h);
        if (it == g_textures.end()) {
            Tex im;
            if (!upload_texture(im, t, t.mip_count != 0 && t.width > 1 && t.height > 1)) return false;
            it = g_textures.emplace(h, std::move(im)).first;
        }
        srv[t.binding] = it->second.srv.Get();
        g_tex_used[h] = g_frame_no;
    }
    // PBR: normal map e ORM nos registradores 14 e 15 (o jogo só usa as unidades 0 e 1).
    if (d.pbr) {
        const Tex *neutral[2] = {&g_pbr_flat, &g_pbr_none};
        for (int k = 0; k < 2; k++) {
            const Texture &t = d.pbr_maps[k];
            srv[14 + k] = neutral[k]->srv.Get(), smp[14 + k] = g_sampler_linear.Get();
            if (!t.rgba || !t.key) continue;
            auto it = g_textures.find(t.key);
            if (it == g_textures.end()) {
                Tex im;
                if (!upload_texture(im, t, t.width > 1 && t.height > 1)) return false;
                it = g_textures.emplace(t.key, std::move(im)).first;
            }
            srv[14 + k] = it->second.srv.Get(), smp[14 + k] = sampler_for(t);
            g_tex_used[t.key] = g_frame_no;
        }
    }
    tgt->cver++;
    if (d.depth_write) tgt->dver++;

    // Viewport do GXM: tela = deslocamento + escala * ndc. O D3D11 não aceita altura negativa: com a escala de y
    // positiva o shader inverte o y (cbuffer b0.w) e a altura fica positiva.
    static const float halfpix = getenv("VITART_VK_HALFPIX") ? (float)std::atof(getenv("VITART_VK_HALFPIX")) : 0.5f;
    const float rs = (float)scale_now();
    const float flip = d.vps[3] > 0 ? -1.f : 1.f;
    D3D11_VIEWPORT vp{};
    vp.TopLeftX = (d.vps[0] - d.vps[1] + halfpix) * rs;
    vp.Width = 2 * d.vps[1] * rs;
    vp.TopLeftY = (d.vps[2] - std::fabs(d.vps[3]) + halfpix) * rs;
    vp.Height = 2 * std::fabs(d.vps[3]) * rs;
    vp.MinDepth = std::clamp(d.vps[4], 0.f, 1.f);
    vp.MaxDepth = std::clamp(d.vps[4] + d.vps[5], 0.f, 1.f);
    if (vp.Width < 0) vp.TopLeftX += vp.Width, vp.Width = -vp.Width;

    // Uniforms e parâmetros do PBR no buffer comum; as bases (em palavras) vão no cbuffer.
    float prm[kPbrParamFloats];
    if (d.pbr) {
        // Pixel -> câmera: a inversa de g_Projection vezes a inversa do viewport usado na GPU (convenção do
        // Vulkan, com a altura negativa quando o GXM inverte o y).
        std::memcpy(prm, d.pbr_params, sizeof prm);
        const float vy = (d.vps[2] - d.vps[3] + halfpix) * rs, vh = 2 * d.vps[3] * rs;
        vt::pbr::screen_to_view(d.pbr_params, vp.TopLeftX, vy, vp.Width, vh, vp.MinDepth, vp.MaxDepth, prm);
    }
    const Piece up[3] = {{d.vert_uniforms, d.vert_uniforms_size, 16}, {d.frag_uniforms, d.frag_uniforms_size, 16},
                         {d.pbr ? prm : nullptr, d.pbr ? (UINT)sizeof prm : 0u, 16}};
    UINT uoff[3];
    if (!ring_put(g_uring, up, 3, uoff)) return false;
    {
        D3D11_MAPPED_SUBRESOURCE m;
        DXC(g_ctx->Map(g_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
        uint32_t *b = static_cast<uint32_t *>(m.pData);
        b[0] = uoff[0] / 4, b[1] = uoff[1] / 4, b[2] = uoff[2] / 4;
        std::memcpy(&b[3], &flip, 4);
        g_ctx->Unmap(g_cb.Get(), 0);
    }

    // Vértices: crus (formatos nativos) ou vec4 por location; índices do jogo (leque vira lista).
    std::vector<float> expanded;
    uint32_t nloc = d.attr_count;
    const float *attrs = d.attrs;
    bool raw = !d.raw_attrs.empty();
    D3D11_INPUT_ELEMENT_DESC el[16];
    uint32_t nel = 0;
    uint64_t lkey = raw ? 0x8000000000000000ull : d.attr_count;
    if (raw) {
        for (const VertexStream &st : d.raw_streams) lkey = (lkey ^ st.stride) * 1099511628211ull;
        for (const VertexAttr &a : d.raw_attrs) {
            lkey = (lkey ^ (a.location | a.stream << 5 | a.offset << 10 | a.fmt << 20 | a.comps << 25)) * 1099511628211ull;
            if (nel < 16) el[nel++] = {"TEXCOORD", a.location, vertex_format(a.fmt, a.comps), a.stream, a.offset, D3D11_INPUT_PER_VERTEX_DATA, 0};
        }
    }
    const uint64_t key = fnv(&d.vs, sizeof d.vs, lkey);
    auto lit = g_layouts.find(key);
    if (lit == g_layouts.end()) {
        ComPtr<ID3D11InputLayout> il;
        if (raw && FAILED(g_dev->CreateInputLayout(el, nel, d.vs, d.vs_size, &il))) il = nullptr;
        if (!raw) {
            for (uint32_t k = 0; k < nloc && k < 16; k++)
                el[k] = {"TEXCOORD", k, DXGI_FORMAT_R32G32B32A32_FLOAT, k, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
            if (FAILED(g_dev->CreateInputLayout(el, std::min(nloc, 16u), d.vs, d.vs_size, &il))) il = nullptr;
        }
        lit = g_layouts.emplace(key, il).first;
    }
    if (raw && !lit->second) {
        // o D3D11 recusou o layout cru (alinhamento): converte na CPU como o caminho sem formatos nativos
        expanded = expand_raw(d, nloc);
        attrs = expanded.data(), raw = false;
        const uint64_t k2 = fnv(&d.vs, sizeof d.vs, nloc);
        lit = g_layouts.find(k2);
        if (lit == g_layouts.end()) {
            ComPtr<ID3D11InputLayout> il;
            for (uint32_t k = 0; k < nloc && k < 16; k++)
                el[k] = {"TEXCOORD", k, DXGI_FORMAT_R32G32B32A32_FLOAT, k, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
            if (FAILED(g_dev->CreateInputLayout(el, std::min(nloc, 16u), d.vs, d.vs_size, &il))) il = nullptr;
            lit = g_layouts.emplace(k2, il).first;
        }
    }
    if (!lit->second) return g_err = "layout de vértices recusado", false;
    std::vector<uint32_t> fan;
    if (d.topology == 2) fan = fan_to_list(d);
    std::vector<Piece> vp_pieces;
    const uint32_t nstreams = raw ? (uint32_t)std::min<size_t>(d.raw_streams.size(), 16) : std::min(nloc, 16u);
    for (uint32_t k = 0; k < nstreams; k++)
        vp_pieces.push_back(raw ? Piece{d.raw_streams[k].data, d.raw_streams[k].size, 16}
                                : Piece{attrs + (size_t)k * d.vertex_count * 4, d.vertex_count * 16u, 16});
    const bool idx16 = d.index_data && d.index_size == 2 && fan.empty();
    if (!fan.empty()) vp_pieces.push_back({fan.data(), (UINT)fan.size() * 4, 4});
    else if (d.index_data) vp_pieces.push_back({d.index_data, d.index_count * d.index_size, 4});
    else vp_pieces.push_back({d.indices, d.index_count * 4, 4});
    std::vector<UINT> voff(vp_pieces.size());
    if (!ring_put(g_vring, vp_pieces.data(), vp_pieces.size(), voff.data())) return false;
    g_stats.draws++;

    Timer tdesc(g_stats.desc_ns);
    bind_target(tgt);
    g_ctx->RSSetViewports(1, &vp);
    g_ctx->RSSetState(g_raster.Get());
    g_ctx->IASetInputLayout(lit->second.Get());
    g_ctx->IASetPrimitiveTopology(d.topology == 1 ? D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer *bufs[16];
    UINT strides[16];
    for (uint32_t k = 0; k < nstreams; k++) bufs[k] = g_vring.buf.Get(), strides[k] = raw ? d.raw_streams[k].stride : 16;
    if (nstreams) g_ctx->IASetVertexBuffers(0, nstreams, bufs, strides, voff.data());
    g_ctx->IASetIndexBuffer(g_vring.buf.Get(), idx16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, voff.back());
    g_ctx->VSSetShader(vs, nullptr, 0);
    g_ctx->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer *cb = g_cb.Get();
    g_ctx->VSSetConstantBuffers(0, 1, &cb);
    g_ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView *ub[3] = {g_uring.srv.Get(), g_uring.srv.Get(), g_uring.srv.Get()};
    g_ctx->VSSetShaderResources(20, 2, ub);
    g_ctx->PSSetShaderResources(20, 3, ub);
    g_ctx->PSSetShaderResources(0, kTexSlots, srv);
    g_ctx->PSSetSamplers(0, kTexSlots, smp);
    g_ctx->OMSetBlendState(blend_state(d), nullptr, 0xFFFFFFFF);
    g_ctx->OMSetDepthStencilState(depth_state(d), d.front.ref);
    static const uint64_t log_frame = getenv("VITART_D3D11_LOG") ? std::strtoull(getenv("VITART_D3D11_LOG"), nullptr, 10) : ~0ull;
    if (g_frame_no >= log_frame && (g_frame_no - log_frame) % 301 == 0) { // depuração: todos os desenhos, a cada 301 quadros (paridade alterna)
        float p0[4] = {0, 0, 0, 1};
        for (const VertexAttr &a : d.raw_attrs)
            if (a.location == 0 && a.stream < d.raw_streams.size()) {
                const VertexStream &st = d.raw_streams[a.stream];
                const uint32_t first = d.index_data ? (d.index_size == 2 ? static_cast<const uint16_t *>(d.index_data)[0]
                                                                           : static_cast<const uint32_t *>(d.index_data)[0]) + d.vertex_offset
                                                    : d.indices[0];
                for (uint32_t c = 0; c < a.comps; c++) p0[c] = attr_value(st.data + (size_t)first * st.stride + a.offset, a.fmt, c);
            }
        if (!raw && attrs && d.index_data) {
            const int32_t first = (int32_t)(d.index_size == 2 ? static_cast<const uint16_t *>(d.index_data)[0]
                                                              : static_cast<const uint32_t *>(d.index_data)[0]) + d.vertex_offset;
            if (first >= 0 && (uint32_t)first < d.vertex_count) std::memcpy(p0, attrs + (size_t)first * 4, 16);
        }
        static int so_n = 0;
        if (d.depth_write && so_n < 8) { // posições que o vértice gera (stream output) dos primeiros desenhos 3D
            so_n++;
            const D3D11_SO_DECLARATION_ENTRY se{0, "SV_Position", 0, 0, 4, 0};
            const UINT stride = 16;
            ComPtr<ID3D11GeometryShader> gs;
            ComPtr<ID3D11Buffer> sob, stg;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 16 * 3 * 64, bd.Usage = D3D11_USAGE_DEFAULT, bd.BindFlags = D3D11_BIND_STREAM_OUTPUT;
            D3D11_BUFFER_DESC sd = bd;
            sd.Usage = D3D11_USAGE_STAGING, sd.BindFlags = 0, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            const HRESULT h1 = g_dev->CreateGeometryShaderWithStreamOutput(d.vs, d.vs_size, &se, 1, &stride, 1,
                                                                           D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &gs);
            if (SUCCEEDED(h1) && SUCCEEDED(g_dev->CreateBuffer(&bd, nullptr, &sob)) && SUCCEEDED(g_dev->CreateBuffer(&sd, nullptr, &stg))) {
                // o mesmo estado do desenho de verdade, ligado logo abaixo; aqui só os primeiros triângulos
                bind_target(tgt);
                g_ctx->RSSetViewports(1, &vp);
                g_ctx->IASetInputLayout(lit->second.Get());
                g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D11Buffer *bufs2[16];
                UINT strides2[16];
                for (uint32_t k = 0; k < nstreams; k++) bufs2[k] = g_vring.buf.Get(), strides2[k] = raw ? d.raw_streams[k].stride : 16;
                if (nstreams) g_ctx->IASetVertexBuffers(0, nstreams, bufs2, strides2, voff.data());
                g_ctx->IASetIndexBuffer(g_vring.buf.Get(), idx16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT, voff.back());
                g_ctx->VSSetShader(vs, nullptr, 0);
                ID3D11Buffer *cb0 = g_cb.Get();
                g_ctx->VSSetConstantBuffers(0, 1, &cb0);
                ID3D11ShaderResourceView *ub2[2] = {g_uring.srv.Get(), g_uring.srv.Get()};
                g_ctx->VSSetShaderResources(20, 2, ub2);
                g_ctx->GSSetShader(gs.Get(), nullptr, 0);
                const UINT zero = 0;
                ID3D11Buffer *so = sob.Get();
                g_ctx->SOSetTargets(1, &so, &zero);
                g_ctx->DrawIndexed(std::min(d.index_count, 3u * 64u) / 3 * 3, 0, d.index_data ? d.vertex_offset : 0);
                ID3D11Buffer *none = nullptr;
                g_ctx->SOSetTargets(1, &none, &zero);
                g_ctx->GSSetShader(nullptr, nullptr, 0);
                g_ctx->CopyResource(stg.Get(), sob.Get());
                D3D11_MAPPED_SUBRESOURCE mm;
                if (SUCCEEDED(g_ctx->Map(stg.Get(), 0, D3D11_MAP_READ, 0, &mm))) {
                    const float *f = static_cast<const float *>(mm.pData);
                    vt::logf("d3d11 dbg: so %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f", f[0], f[1], f[2], f[3], f[4],
                             f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
                    g_ctx->Unmap(stg.Get(), 0);
                }
                // uniforms deste desenho, lidos de volta do anel (16 primeiras palavras)
                const float *vu = reinterpret_cast<const float *>(d.vert_uniforms);
                if (vu && d.vert_uniforms_size >= 64)
                    vt::logf("d3d11 dbg: vu %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f", vu[0], vu[1], vu[2],
                             vu[3], vu[4], vu[5], vu[6], vu[7], vu[8], vu[9], vu[10], vu[11], vu[12], vu[13], vu[14], vu[15]);
            } else {
                vt::logf("d3d11 dbg: stream output recusado %08x", (unsigned)h1);
            }
        }
        std::string fm;
        for (const VertexAttr &a : d.raw_attrs) fm += std::to_string(a.location) + ":" + std::to_string(a.fmt) + "x" + std::to_string(a.comps) + " ";
        vt::logf("d3d11 dbg: q%llu alvo %08x %ux%u vp %.1f %.1f %.1f %.1f z %.2f %.2f flip %.0f dw %d df %u st %d top %u raw %d nel %u "
                 "vc %u ic %u vo %d u %u/%u/%u vsz %u fsz %u tex %zu p0 %.3f %.3f %.3f %.3f [%s] bl %d %08x fs %p",
                 (unsigned long long)g_frame_no, d.target_addr, d.target_w, d.target_h, vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height, vp.MinDepth, vp.MaxDepth, flip,
                 d.depth_write, d.depth_func, stencil_used(d), d.topology, raw, nel, d.vertex_count, d.index_count, d.vertex_offset,
                 uoff[0], uoff[1], uoff[2], d.vert_uniforms_size, d.frag_uniforms_size, d.textures.size(), p0[0], p0[1], p0[2], p0[3], fm.c_str(), d.has_blend, d.blend, (const void *)d.fs);
    }
    const uint32_t count = fan.empty() ? d.index_count : (uint32_t)fan.size();
    g_ctx->DrawIndexed(count, 0, d.index_data ? d.vertex_offset : 0);
    return true;
}

bool present(uint32_t addr) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_dev) return false;
    g_dw_last = g_dw_frame.exchange(0);
    if (!g_swap && !create_swapchain()) return false;
    win::pump();
    auto it = g_targets.find(addr);
    if (getenv("VITART_D3D11_LOG") && g_frame_no == std::strtoull(getenv("VITART_D3D11_LOG"), nullptr, 10))
        vt::logf("d3d11 dbg: apresenta %08x (%s)", addr, it == g_targets.end() ? "sem alvo" : "ok");
    if (it == g_targets.end()) return false;
    if (win::take_resized()) {
        RECT r;
        GetClientRect(static_cast<HWND>(win::hwnd()), &r);
        if (r.right > 0 && r.bottom > 0) {
            g_bb_rtv.Reset();
            g_ctx->OMSetRenderTargets(0, nullptr, nullptr);
            g_open = nullptr;
            DXC(g_swap->ResizeBuffers(0, (UINT)r.right, (UINT)r.bottom, DXGI_FORMAT_UNKNOWN, 0));
            if (!make_backbuffer()) return false;
        }
    }
    if (!g_bb_rtv || IsIconic(static_cast<HWND>(win::hwnd()))) return true; // minimizada
    ID3D11ShaderResourceView *src = it->second.color.srv.Get();
    uint32_t sw_ = it->second.color.w, sh_ = it->second.color.h;
    if (g_vid_px) {
        if (g_vid.w != g_vid_w || g_vid.h != g_vid_h) {
            g_vid = {};
            make_tex(g_vid, g_vid_w, g_vid_h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
        }
        if (g_vid.tex) {
            g_ctx->UpdateSubresource(g_vid.tex.Get(), 0, nullptr, g_vid_px, g_vid_w * 4, 0);
            src = g_vid.srv.Get(), sw_ = g_vid_w, sh_ = g_vid_h;
        }
        g_vid_px = nullptr;
    }
    // Proporção do jogo preservada: a imagem ocupa o maior retângulo centrado; o resto fica preto.
    const float black[4] = {0, 0, 0, 1};
    g_ctx->ClearRenderTargetView(g_bb_rtv.Get(), black);
    const double bw = g_bb_w, bh = g_bb_h, sa = (double)sw_ / sh_;
    int32_t dw = (int32_t)bw, dh = (int32_t)(bw / sa + 0.5);
    if (dh > (int32_t)bh) dh = (int32_t)bh, dw = (int32_t)(bh * sa + 0.5);
    const int32_t dx = ((int32_t)bw - dw) / 2, dy = ((int32_t)bh - dh) / 2;
    blit(src, g_bb_rtv.Get(), (float)dx, (float)dy, (float)dw, (float)dh);
    ComPtr<ID3D11Resource> bb;
    g_bb_rtv->GetResource(&bb);
    {
        static std::vector<uint32_t> text;
        uint32_t w, h;
        const int32_t margin = (int32_t)(g_bb_h / 60);
        if (ovl::fps_frame(g_bb_h, text, w, h)) put_bgra(bb.Get(), dx + margin, dy + margin, text.data(), w, h, 0, h, 0, w);
    }
    {
        const BannerImage *bi = nullptr;
        int32_t x0 = 0, y0 = 0;
        if (ovl::trophy_frame(dx, dy, dw, dh, bi, x0, y0)) {
            const uint32_t r = std::min(bi->radius, bi->h / 2);
            for (uint32_t y = 0; y < r; y++) put_bgra(bb.Get(), x0, y0, bi->px.data(), bi->w, bi->h, y, 1, bi->row_from[y], bi->row_to[y]);
            put_bgra(bb.Get(), x0, y0, bi->px.data(), bi->w, bi->h, r, bi->h - 2 * r, 0, bi->w);
            for (uint32_t y = bi->h - r; y < bi->h; y++)
                put_bgra(bb.Get(), x0, y0, bi->px.data(), bi->w, bi->h, y, 1, bi->row_from[y], bi->row_to[y]);
        }
    }
    // VSync desligado ou o padrão (mailbox no Vulkan): não espera o vblank; com "FPS máximo" (fifo), espera.
    const UINT interval = g_vsync && g_fifo && !getenv("VITART_NOVSYNC") ? 1 : 0;
    const HRESULT hr = g_swap->Present(interval, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) return fail("Present (GPU perdida)", hr);
    win::presented();
    g_open = nullptr; // o blit trocou o alvo
    // Texturas sem uso há muitos quadros saem (o vídeo gera uma por quadro).
    if (++g_frame_no % 64 == 0)
        for (auto t = g_textures.begin(); t != g_textures.end();) {
            auto u = g_tex_used.find(t->first);
            if (u == g_tex_used.end() || g_frame_no - u->second > 128) {
                g_tex_used.erase(t->first);
                t = g_textures.erase(t);
            } else {
                ++t;
            }
        }
    return true;
}

void end_frame() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ctx) g_ctx->Flush();
}

std::vector<uint32_t> targets() {
    std::vector<uint32_t> v;
    for (auto &kv : g_targets) v.push_back(kv.first);
    return v;
}

bool read_target(uint32_t addr, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_targets.find(addr);
    if (it == g_targets.end()) return false;
    D3D11_TEXTURE2D_DESC td{};
    it->second.color.tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING, td.BindFlags = 0, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ, td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    DXC(g_dev->CreateTexture2D(&td, nullptr, &st));
    g_ctx->CopyResource(st.Get(), it->second.color.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    DXC(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m));
    w = td.Width, h = td.Height;
    rgba.resize((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&rgba[(size_t)y * w * 4], static_cast<uint8_t *>(m.pData) + (size_t)y * m.RowPitch, w * 4);
    g_ctx->Unmap(st.Get(), 0);
    return true;
}

uint32_t render_scale() { return scale_now(); }
uint32_t auto_render_scale() { return scale_auto(); }
// Troca a resolução interna com o jogo rodando: cada alvo é recriado na escala nova com a imagem atual
// reduzida/ampliada com filtro; a profundidade volta limpa e as cópias amostráveis são refeitas sob demanda.
bool set_render_scale(uint32_t s) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    s = s < 1 ? 1 : s > 8 ? 8 : s;
    const uint32_t cur = scale_now();
    if (s == cur) return true;
    if (!g_dev || g_targets.empty()) return g_scale_override = s, true;
    for (auto &kv : g_targets) {
        Target &t = kv.second;
        Target n;
        if (!make_target(n, t.color.w / cur * s, t.color.h / cur * s)) return false;
        blit(t.color.srv.Get(), n.rtv.Get(), 0, 0, (float)n.color.w, (float)n.color.h);
        n.cver = t.cver + 1, n.dver = t.dver + 1;
        t = std::move(n);
    }
    g_snaps.clear();
    g_open = nullptr;
    g_scale_override = s;
    return true;
}

const std::vector<float> &debug_vs_output() {
    static const std::vector<float> none;
    return none;
}

Stats peek_stats() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_stats;
}
Stats take_stats() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Stats s = g_stats;
    g_stats = {};
    return s;
}

void shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_dev) return;
    g_ctx->ClearState();
    g_ctx->Flush();
    g_targets.clear(), g_snaps.clear(), g_textures.clear(), g_samplers.clear(), g_blends.clear(), g_depths.clear();
    g_vs.clear(), g_ps.clear(), g_layouts.clear();
    g_vring = {}, g_uring = {}, g_cb.Reset(), g_raster.Reset();
    g_white = {}, g_pbr_flat = {}, g_pbr_none = {}, g_vid = {};
    g_sampler_linear.Reset(), g_sampler_nearest.Reset(), g_blit_vs.Reset(), g_blit_ps.Reset();
    g_bb_rtv.Reset(), g_swap.Reset(), g_ctx.Reset(), g_dev.Reset();
}

} // namespace gvk::dx11
