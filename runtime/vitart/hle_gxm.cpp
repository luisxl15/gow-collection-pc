// HLE do sceGxm para o vitart: camada de ESTADO. Objetos (contexto, shader patcher, programas,
// render targets, sync objects) existem como blocos na memória do guest (para o jogo guardar
// ponteiros válidos) com o estado no host. Ainda não desenha: grava o que cada cena pede e
// conclui as notificações na hora, para o jogo seguir o laço. O renderizador Vulkan entra aqui.
#include "vitart.h"

#include <chrono>
#include <deque>
#include <map>
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
VT_HLE(sceGxmDepthStencilSurfaceSetForceLoadMode) { ret(ctx, SCE_OK); }
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
    wr32(arg(ctx, 1), reserve_uniforms(c->vertex_ring, c->vertex_ring_size, c->vertex_ring_pos, c->vprog));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmReserveFragmentDefaultUniformBuffer) {
    auto c = context_of(arg(ctx, 0));
    if (!c || !arg(ctx, 1)) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    wr32(arg(ctx, 1), reserve_uniforms(c->fragment_ring, c->fragment_ring_size, c->fragment_ring_pos, c->fprog));
    ret(ctx, SCE_OK);
}
VT_HLE(sceGxmSetUniformDataF) {
    // (uniformBuffer, parameter, componentOffset, componentCount, const float *sourceData)
    const uint32_t buf = arg(ctx, 0), param = arg(ctx, 1), off = arg(ctx, 2), n = arg(ctx, 3), src = arg(ctx, 4);
    if (!buf || !param || !src) return ret(ctx, GXM_ERROR_INVALID_POINTER);
    const uint32_t base = buf + 4 * (rd32(param + 12) + off);
    std::memcpy(ptr<uint8_t>(base), ptr<uint8_t>(src), 4 * n);
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
    g_draws++;
    ret(ctx, SCE_OK);
}

// Estado fixo: só registrado quando o renderizador existir; por ora aceito.
VT_HLE(sceGxmSetViewport) {}
VT_HLE(sceGxmSetCullMode) {}
VT_HLE(sceGxmSetTwoSidedEnable) {}
VT_HLE(sceGxmSetFrontDepthFunc) {}
VT_HLE(sceGxmSetBackDepthFunc) {}
VT_HLE(sceGxmSetFrontDepthWriteEnable) {}
VT_HLE(sceGxmSetBackDepthWriteEnable) {}
VT_HLE(sceGxmSetFrontStencilFunc) {}
VT_HLE(sceGxmSetBackStencilFunc) {}
VT_HLE(sceGxmSetFrontStencilRef) {}
VT_HLE(sceGxmSetBackStencilRef) {}
VT_HLE(sceGxmSetFrontPolygonMode) {}
VT_HLE(sceGxmSetUserMarker) { ret(ctx, SCE_OK); }

} // namespace vt
