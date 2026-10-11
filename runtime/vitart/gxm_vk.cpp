// Renderizador Vulkan dos desenhos do GXM (ver gxm_vk.h).
//
// Os desenhos de um quadro vão para um único command buffer, com vértices, índices, uniforms e texturas
// num anel de upload mapeado e um pool de descritores por quadro. O render pass fica aberto enquanto o
// alvo não muda. Ler um alvo como textura no mesmo quadro grava uma cópia para uma imagem amostrável no
// próprio fluxo (sem laço de realimentação e sem esperar a GPU). A espera só acontece ao apresentar, ao
// ler um alvo ou quando o anel enche.
#include "gxm_vk.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "user32.lib")
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <mutex>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

namespace gvk {
namespace {

std::string g_err;
VkInstance g_inst = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkDevice g_dev = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_qfam = 0;
VkCommandPool g_pool = VK_NULL_HANDLE;
VkCommandBuffer g_cmd = VK_NULL_HANDLE;
VkCommandBuffer g_cur = VK_NULL_HANDLE;  // onde barrier() grava (g_cmd, ou g_pcmd durante a apresentação)
VkCommandBuffer g_pcmd = VK_NULL_HANDLE; // apresentação: buffer e fence próprios, sem esperar a GPU
VkFence g_pfence = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;
VkPhysicalDeviceMemoryProperties g_memprops{};
VkRenderPass g_rp = VK_NULL_HANDLE;
VkDescriptorSetLayout g_dsl[3] = {};
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkSampler g_sampler = VK_NULL_HANDLE, g_sampler_nearest = VK_NULL_HANDLE;
constexpr uint32_t kTexBindings = 16;
std::recursive_mutex g_lock; // desenhos vêm da thread de render do jogo, a apresentação da de vídeo

// Janela e swapchain (só com init(..., true)).
HWND g_hwnd = nullptr;
bool g_closed = false;
VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swap = VK_NULL_HANDLE;
std::vector<VkImage> g_swap_images;
VkExtent2D g_swap_extent{};
VkSemaphore g_acquired = VK_NULL_HANDLE, g_rendered = VK_NULL_HANDLE;
// Profundidade com stencil (o GXM usa DF32 + S8): a vista de amostragem só tem a profundidade, a de
// anexo tem os dois aspectos.
constexpr VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM, kDepth = VK_FORMAT_D32_SFLOAT_S8_UINT;
constexpr VkImageAspectFlags kDepthAspects = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;

Stats g_stats;
// Resolução interna: os alvos do jogo são renderizados em escala x (VITART_VK_SCALE, padrão 2).
// Viewport, scissor e o deslocamento de meio pixel escalam junto; texturas lidas de alvos usam
// coordenadas normalizadas e não mudam.
uint32_t render_scale() {
    static const uint32_t s = [] {
        const char *e = getenv("VITART_VK_SCALE");
        const int v = e ? std::atoi(e) : 2;
        return (uint32_t)(v < 1 ? 1 : v > 8 ? 8 : v);
    }();
    return s;
}
float g_aniso = 1.f;                         // anisotropia máxima dos samplers (1 = desligada)
VkPipelineCache g_pcache = VK_NULL_HANDLE;   // persistido em vk_pipeline_cache.bin
bool g_pcache_dirty = false;
VkQueryPool g_qpool = VK_NULL_HANDLE;
double g_ts_period = 1.0; // ns por tique de timestamp
uint64_t now_ns() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
struct Timer {
    uint64_t &acc, t0 = now_ns();
    explicit Timer(uint64_t &a) : acc(a) {}
    ~Timer() { acc += now_ns() - t0; }
};

constexpr const char *kPcacheFile = "vk_pipeline_cache.bin";
void save_pcache() {
    if (!g_pcache || !g_pcache_dirty) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(g_dev, g_pcache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<char> data(n);
    if (vkGetPipelineCacheData(g_dev, g_pcache, &n, data.data()) != VK_SUCCESS) return;
    if (FILE *f = std::fopen(kPcacheFile, "wb")) {
        std::fwrite(data.data(), 1, n, f);
        std::fclose(f);
        g_pcache_dirty = false;
    }
}

bool fail(const char *what, VkResult r) {
    char b[160];
    std::snprintf(b, sizeof b, "%s falhou (VkResult %d)", what, (int)r);
    g_err = b;
    return false;
}
#define VKC(call)                                   \
    do {                                            \
        VkResult r_ = (call);                       \
        if (r_ != VK_SUCCESS) return fail(#call, r_); \
    } while (0)

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

struct Buffer {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};

bool make_buffer(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage, const void *data) {
    b.size = size ? size : 16;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = b.size;
    ci.usage = usage;
    VKC(vkCreateBuffer(g_dev, &ci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKC(vkAllocateMemory(g_dev, &ai, nullptr, &b.mem));
    VKC(vkBindBufferMemory(g_dev, b.buf, b.mem, 0));
    void *p;
    VKC(vkMapMemory(g_dev, b.mem, 0, b.size, 0, &p));
    if (data && size)
        std::memcpy(p, data, size);
    else
        std::memset(p, 0, b.size);
    vkUnmapMemory(g_dev, b.mem);
    return true;
}

void destroy(Buffer &b) {
    if (b.buf) vkDestroyBuffer(g_dev, b.buf, nullptr);
    if (b.mem) vkFreeMemory(g_dev, b.mem, nullptr);
    b = {};
}

struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView att_view = VK_NULL_HANDLE; // profundidade: vista de anexo (profundidade + stencil)
    uint32_t w = 0, h = 0, mips = 1;
    VkFormat fmt = kColor;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

bool make_image(Image &im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, uint32_t mips = 1) {
    Timer tm(g_stats.alloc_ns);
    im.w = w, im.h = h, im.fmt = fmt, im.mips = mips;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = mips;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    VKC(vkCreateImage(g_dev, &ci, nullptr, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, im.img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(g_dev, &ai, nullptr, &im.mem));
    VKC(vkBindImageMemory(g_dev, im.img, im.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {(VkImageAspectFlags)(fmt == kDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, mips, 0, 1};
    VKC(vkCreateImageView(g_dev, &vi, nullptr, &im.view));
    if (fmt == kDepth) {
        vi.subresourceRange.aspectMask = kDepthAspects;
        VKC(vkCreateImageView(g_dev, &vi, nullptr, &im.att_view));
    }
    return true;
}

void destroy(Image &im) {
    if (im.view) vkDestroyImageView(g_dev, im.view, nullptr);
    if (im.att_view) vkDestroyImageView(g_dev, im.att_view, nullptr);
    if (im.img) vkDestroyImage(g_dev, im.img, nullptr);
    if (im.mem) vkFreeMemory(g_dev, im.mem, nullptr);
    im = {};
}

void barrier(Image &im, VkImageLayout to) {
    if (im.layout == to) return;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = im.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.img;
    b.subresourceRange = {(VkImageAspectFlags)(im.fmt == kDepth ? kDepthAspects : VK_IMAGE_ASPECT_COLOR_BIT), 0, im.mips, 0, 1};
    vkCmdPipelineBarrier(g_cur, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    im.layout = to;
}

// Dois quadros em voo: cada vaga tem command buffer, fence, pool de descritores, metade do anel de
// upload e um par de timestamps. A CPU grava uma vaga enquanto a GPU executa a outra.
constexpr uint32_t kSlots = 2;
VkCommandBuffer g_cmds[kSlots] = {};
VkFence g_fences[kSlots] = {};
VkDescriptorPool g_dpools[kSlots] = {};
bool g_pend[kSlots] = {}, g_tsopen[kSlots] = {};
uint32_t g_fi = 0; // vaga atual

bool begin_cmd() {
    VKC(vkResetCommandBuffer(g_cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(g_cmd, &bi));
    if (g_qpool) {
        vkCmdResetQueryPool(g_cmd, g_qpool, 2 * g_fi, 2);
        vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_qpool, 2 * g_fi);
    }
    return true;
}

bool submit_wait() {
    VKC(vkEndCommandBuffer(g_cmd));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    VKC(vkResetFences(g_dev, 1, &g_fence));
    VKC(vkQueueSubmit(g_queue, 1, &si, g_fence));
    VKC(vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX));
    return true;
}

struct Target {
    Image color, depth;
    VkFramebuffer fb = VK_NULL_HANDLE;
    uint64_t cver = 1, dver = 1; // sobem a cada escrita na cor / na profundidade
};
std::map<uint32_t, Target> g_targets;

// ------------------------------------------------------------- quadro em gravação
bool g_recording = false;
Target *g_open = nullptr; // alvo com render pass aberto
Buffer g_ring;
uint8_t *g_ring_ptr = nullptr;
VkDeviceSize g_ring_off = 0, g_ring_base = 0, g_ssbo_align = 256;
constexpr VkDeviceSize kRingSize = 256ull << 20, kRingHalf = kRingSize / kSlots;
VkDescriptorPool g_dpool = VK_NULL_HANDLE;
uint32_t g_draws_in_frame = 0;
constexpr uint32_t kMaxDrawsPerFrame = 2048;
std::vector<Image> g_transient;
std::unordered_map<uint64_t, Image> g_textures;
std::unordered_map<uint64_t, uint64_t> g_tex_used; // textura -> último quadro de gravação que a usou
uint64_t g_frame_no = 0;                            // quadros de gravação (frame_begin) desde o início
std::vector<float> g_debug_vs; // cópias de alvos lidas como textura: vivem até o fim do quadro

void end_pass() {
    if (g_open) {
        vkCmdEndRenderPass(g_cmd);
        g_open = nullptr;
    }
}

bool slot_wait(uint32_t i) {
    if (!g_pend[i]) return true;
    {
        Timer tm(g_stats.wait_ns);
        VKC(vkWaitForFences(g_dev, 1, &g_fences[i], VK_TRUE, UINT64_MAX));
    }
    if (g_tsopen[i]) {
        uint64_t ts[2] = {};
        if (vkGetQueryPoolResults(g_dev, g_qpool, 2 * i, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
            ts[1] > ts[0])
            g_stats.gpu_ns += (uint64_t)((ts[1] - ts[0]) * g_ts_period);
        g_tsopen[i] = false;
    }
    g_pend[i] = false;
    return true;
}

void select_slot(uint32_t i) {
    g_fi = i;
    g_cmd = g_cur = g_cmds[i];
    g_fence = g_fences[i];
    g_dpool = g_dpools[i];
    g_ring_base = (VkDeviceSize)i * kRingHalf;
}

// Espera as duas vagas (leitura de alvo, depuração, encerramento).
bool frame_wait() {
    for (uint32_t i = 0; i < kSlots; i++)
        if (!slot_wait(i)) return false;
    for (auto &im : g_transient) destroy(im);
    g_transient.clear();
    return true;
}

bool frame_begin() {
    if (g_recording) return true;
    // Próxima vaga: só espera o quadro que a usou por último (o de dois quadros atrás).
    const uint32_t next = (g_fi + 1) % kSlots;
    if (!slot_wait(next)) return false;
    select_slot(next);
    // A GPU terminou tudo: texturas sem uso há muitos quadros saem (o vídeo gera uma por quadro).
    if (++g_frame_no % 64 == 0)
        for (auto it = g_textures.begin(); it != g_textures.end();) {
            auto u = g_tex_used.find(it->first);
            if (u == g_tex_used.end() || g_frame_no - u->second > 128) {
                destroy(it->second);
                g_tex_used.erase(it->first);
                it = g_textures.erase(it);
            } else {
                ++it;
            }
        }
    g_ring_off = 0;
    g_draws_in_frame = 0;
    VKC(vkResetDescriptorPool(g_dev, g_dpool, 0));
    if (!begin_cmd()) return false;
    g_recording = true;
    return true;
}

bool frame_flush(bool wait) {
    if (g_recording) {
        end_pass();
        if (g_qpool) vkCmdWriteTimestamp(g_cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_qpool, 2 * g_fi + 1);
        g_tsopen[g_fi] = g_qpool != VK_NULL_HANDLE;
        VKC(vkEndCommandBuffer(g_cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &g_cmd;
        VKC(vkResetFences(g_dev, 1, &g_fence));
        VKC(vkQueueSubmit(g_queue, 1, &si, g_fence));
        g_stats.submits++;
        g_pend[g_fi] = true;
        g_recording = false;
    }
    return !wait || frame_wait();
}

// Garante espaço no anel e no pool para um desenho; se faltar, fecha o quadro e começa outro.
bool frame_reserve(VkDeviceSize bytes) {
    if (bytes > kRingHalf) {
        g_err = "upload maior que o anel";
        return false;
    }
    if (!frame_begin()) return false;
    if (g_ring_off + bytes <= kRingHalf && g_draws_in_frame < kMaxDrawsPerFrame) return true;
    return frame_flush(false) && frame_begin();
}

VkDeviceSize ring_put(const void *data, VkDeviceSize n, VkDeviceSize align) {
    Timer tm(g_stats.ring_ns);
    g_ring_off = (g_ring_off + align - 1) & ~(align - 1);
    const VkDeviceSize at = g_ring_base + g_ring_off, len = n ? n : 16;
    if (data && n)
        std::memcpy(g_ring_ptr + at, data, n);
    else
        std::memset(g_ring_ptr + at, 0, len);
    g_ring_off += len;
    return at;
}

bool get_target(uint32_t addr, uint32_t w, uint32_t h, Target *&out) {
    auto it = g_targets.find(addr);
    if (it != g_targets.end()) {
        out = &it->second;
        return true;
    }
    Target t;
    w *= render_scale(), h *= render_scale();
    if (!make_image(t.color, w, h, kColor,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    if (!make_image(t.depth, w, h, kDepth,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return false;
    VkImageView views[2] = {t.color.view, t.depth.att_view};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g_rp;
    fi.attachmentCount = 2;
    fi.pAttachments = views;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    VKC(vkCreateFramebuffer(g_dev, &fi, nullptr, &t.fb));
    // Estado inicial: cor 0 e profundidade 1 (o jogo limpa com desenhos próprios).
    if (!frame_begin()) return false;
    end_pass();
    barrier(t.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barrier(t.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue cc{};
    VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_cmd, t.color.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &cr);
    VkClearDepthStencilValue dc{1.0f, 0};
    VkImageSubresourceRange dr{kDepthAspects, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(g_cmd, t.depth.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dc, 1, &dr);
    barrier(t.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(t.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    out = &(g_targets[addr] = t);
    return true;
}

uint64_t fnv(const void *p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    for (size_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

std::map<uint32_t, uint32_t> g_depth_owner; // superfície de profundidade -> alvo de cor que a usa
// Cópia amostrável de um alvo (cor ou profundidade), refeita só quando o alvo mudou desde a última cópia:
// o passe de névoa é lido por quase todos os desenhos da cena sem mudar entre eles.
struct Snap {
    Image im;
    uint64_t ver = 0;
};
std::map<std::pair<uint32_t, bool>, Snap> g_snaps;
Image g_white;

// Samplers por combinação de filtro e endereçamento do jogo (são poucas).
std::map<uint32_t, VkSampler> g_samplers;
VkSamplerAddressMode addr_mode(uint32_t m) {
    switch (m) {
    case 0: case 4: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; // clamp, mirror clamp e as bordas
    }
}
VkSampler sampler_for(const Texture &t) {
    const bool mipped = t.mip_count != 0 && t.rgba;
    const uint32_t key = (t.mag & 1) | (t.min & 1) << 1 | (t.addr_u & 7) << 2 | (t.addr_v & 7) << 5 |
                         (mipped ? 1u : 0u) << 8 | (t.mip_filter & 1) << 9 | (uint32_t)(t.lod_bias & 63) << 10;
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second;
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = (t.mag & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = (t.min & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.addressModeU = addr_mode(t.addr_u);
    sci.addressModeV = addr_mode(t.addr_v);
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    sci.maxLod = mipped ? VK_LOD_CLAMP_NONE : 0.25f;
    sci.mipmapMode = (t.mip_filter & 1) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.mipLodBias = mipped ? ((float)(t.lod_bias & 63) - 31.f) / 8.f : 0.f;
    if (mipped && g_aniso > 1.f && (t.min & 1)) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = g_aniso;
    }
    VkSampler smp = VK_NULL_HANDLE;
    if (vkCreateSampler(g_dev, &sci, nullptr, &smp) != VK_SUCCESS) smp = g_sampler;
    return g_samplers[key] = smp;
}

// Grava o envio no quadro atual (o chamador já reservou os bytes no anel).
// Barreira de um único nível (geração de mipmaps).
void level_barrier(Image &im, uint32_t level, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im.img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

uint32_t mip_levels(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while ((w | h) >> n) n++;
    return n;
}

bool upload_texture(Image &im, uint32_t w, uint32_t h, const uint8_t *rgba, bool mipmapped = false) {
    const uint32_t mips = mipmapped ? mip_levels(w, h) : 1;
    if (!make_image(im, w, h, kColor,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mips))
        return false;
    g_stats.uploads++;
    const VkDeviceSize at = ring_put(rgba, (VkDeviceSize)w * h * 4, 16);
    end_pass();
    barrier(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = at;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(g_cmd, g_ring.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    // Níveis seguintes por redução linear do anterior; ao fim todos ficam TRANSFER_SRC.
    for (uint32_t l = 1; l < mips; l++) {
        level_barrier(im, l - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        bl.srcOffsets[1] = {(int32_t)std::max(1u, w >> (l - 1)), (int32_t)std::max(1u, h >> (l - 1)), 1};
        bl.dstOffsets[1] = {(int32_t)std::max(1u, w >> l), (int32_t)std::max(1u, h >> l), 1};
        vkCmdBlitImage(g_cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                       VK_FILTER_LINEAR);
    }
    if (mips > 1) {
        level_barrier(im, mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        im.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    barrier(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

std::unordered_map<uint64_t, VkShaderModule> g_modules;

// O chamador mantém o SPIR-V vivo e no mesmo endereço (cache por programa): o ponteiro basta como
// chave rápida; o hash do conteúdo (caro: KB por desenho) só na primeira vez.
std::unordered_map<const void *, VkShaderModule> g_modules_by_ptr;
VkShaderModule module_by_content(const uint32_t *code, uint32_t size);

VkShaderModule module(const uint32_t *code, uint32_t size) {
    auto pit = g_modules_by_ptr.find(code);
    if (pit != g_modules_by_ptr.end()) return pit->second;
    return g_modules_by_ptr[code] = module_by_content(code, size);
}

VkShaderModule module_by_content(const uint32_t *code, uint32_t size) {
    const uint64_t h = fnv(code, size);
    auto it = g_modules.find(h);
    if (it != g_modules.end()) return it->second;
    Timer tm(g_stats.pipe_ns);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_dev, &ci, nullptr, &m) != VK_SUCCESS) m = VK_NULL_HANDLE;
    return g_modules[h] = m;
}

VkBlendFactor factor(uint32_t f) {
    static const VkBlendFactor t[12] = {
        VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE};
    return f < 12 ? t[f] : VK_BLEND_FACTOR_ONE;
}

VkBlendOp blend_op(uint32_t f) {
    switch (f) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

std::map<std::tuple<VkShaderModule, VkShaderModule, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, VkPipeline> g_pipes;

// Função e operações de uma face em 12 bits (máscaras e referência são estado dinâmico).
uint32_t stencil_bits(const Stencil &st) { return (st.func & 7) | (st.fail & 7) << 3 | (st.zfail & 7) << 6 | (st.zpass & 7) << 9; }
bool stencil_used(const Draw &d) {
    auto idle = [](const Stencil &st) { return st.func == 7 && !st.fail && !st.zfail && !st.zpass; };
    return d.stencil && !(idle(d.front) && idle(d.back));
}
VkStencilOpState stencil_state(const Stencil &st) {
    VkStencilOpState o{};
    o.failOp = (VkStencilOp)(st.fail & 7);
    o.depthFailOp = (VkStencilOp)(st.zfail & 7);
    o.passOp = (VkStencilOp)(st.zpass & 7);
    o.compareOp = (VkCompareOp)(st.func & 7);
    return o;
}

VkFormat vertex_format(uint32_t fmt, uint32_t comps) {
    static const VkFormat t[10][4] = {
        {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED},
        {VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED},
        {VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED},
        {VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED},
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
        {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM},
        {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
        {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM},
        {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT},
        {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}};
    if (fmt > 9 || comps < 1 || comps > 4) return VK_FORMAT_UNDEFINED;
    return t[fmt][comps - 1];
}

VkPipeline pipeline(const Draw &d, VkShaderModule vs, VkShaderModule fs) {
    const uint32_t blend = d.has_blend ? d.blend : 0xFFFFFFFFu;
    const uint32_t depth = d.depth_func | (d.depth_write ? 8u : 0u);
    const uint32_t sten = stencil_used(d) ? (1u << 24 | stencil_bits(d.front) | stencil_bits(d.back) << 12) : 0;
    // Layout dos vértices crus entra na chave (strides, e por atributo location/stream/offset/formato).
    uint64_t layout = d.raw_attrs.empty() ? d.attr_count : 0x8000000000000000ull;
    for (const VertexStream &st : d.raw_streams) layout = (layout ^ st.stride) * 1099511628211ull;
    for (const VertexAttr &a : d.raw_attrs)
        layout = (layout ^ (a.location | a.stream << 5 | a.offset << 10 | a.fmt << 20 | a.comps << 25)) * 1099511628211ull;
    auto key = std::make_tuple(vs, fs, layout, blend, depth, d.cull, sten);
    auto it = g_pipes.find(key);
    if (it != g_pipes.end()) return it->second;
    Timer tm(g_stats.pipe_ns);
    g_stats.pipes++;

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    std::vector<VkVertexInputBindingDescription> binds;
    std::vector<VkVertexInputAttributeDescription> attrs;
    if (d.raw_attrs.empty()) {
        for (uint32_t k = 0; k < d.attr_count; k++) {
            binds.push_back({k, 16, VK_VERTEX_INPUT_RATE_VERTEX});
            attrs.push_back({k, k, VK_FORMAT_R32G32B32A32_SFLOAT, 0});
        }
    } else {
        for (uint32_t b = 0; b < d.raw_streams.size(); b++) binds.push_back({b, d.raw_streams[b].stride, VK_VERTEX_INPUT_RATE_VERTEX});
        for (const VertexAttr &a : d.raw_attrs) attrs.push_back({a.location, a.stream, vertex_format(a.fmt, a.comps), a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (uint32_t)binds.size();
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // o sentido da face com viewport invertido ainda não foi medido
    static const bool cw_front = getenv("VITART_VK_FRONT_CW") != nullptr; // depuração: inverte a face da frente
    rs.frontFace = cw_front ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // VITART_VK_NODEPTH=1 (depuração): sem teste de profundidade, para separar geometria de profundidade.
    static const bool nodepth = getenv("VITART_VK_NODEPTH") != nullptr;
    ds.depthTestEnable = nodepth ? VK_FALSE : VK_TRUE;
    ds.depthWriteEnable = d.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = (VkCompareOp)(d.depth_func & 7);
    if (sten) {
        ds.stencilTestEnable = VK_TRUE;
        ds.front = stencil_state(d.front);
        ds.back = stencil_state(d.back);
    }
    VkPipelineColorBlendAttachmentState ba{};
    // VITART_VK_NOBLEND=1 (depuração): sem blend, para separar triângulo ausente de alfa 0.
    static const bool noblend = getenv("VITART_VK_NOBLEND") != nullptr;
    if (d.has_blend && !noblend) {
        const uint32_t b = d.blend;
        const uint32_t mask = b & 0xFF, cfunc = (b >> 8) & 15, afunc = (b >> 12) & 15;
        ba.colorWriteMask = ((mask & 2) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((mask & 4) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                            ((mask & 8) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((mask & 1) ? VK_COLOR_COMPONENT_A_BIT : 0);
        ba.blendEnable = (cfunc || afunc) ? VK_TRUE : VK_FALSE;
        ba.colorBlendOp = blend_op(cfunc);
        ba.alphaBlendOp = blend_op(afunc);
        ba.srcColorBlendFactor = cfunc ? factor((b >> 16) & 15) : VK_BLEND_FACTOR_ONE;
        ba.dstColorBlendFactor = cfunc ? factor((b >> 20) & 15) : VK_BLEND_FACTOR_ZERO;
        ba.srcAlphaBlendFactor = afunc ? factor((b >> 24) & 15) : VK_BLEND_FACTOR_ONE;
        ba.dstAlphaBlendFactor = afunc ? factor((b >> 28) & 15) : VK_BLEND_FACTOR_ZERO;
    } else {
        ba.colorWriteMask = 0xF;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    VkDynamicState dyn[5] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 5;
    dy.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.stageCount = 2;
    pci.pStages = st;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dy;
    pci.layout = g_layout;
    pci.renderPass = g_rp;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g_dev, g_pcache, 1, &pci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    g_pcache_dirty = true;
    return g_pipes[key] = p;
}

} // namespace

const char *last_error() { return g_err.c_str(); }

bool vertex_format_ok(uint32_t fmt, uint32_t comps) {
    static int cache[10][5] = {};
    const VkFormat f = vertex_format(fmt, comps);
    if (f == VK_FORMAT_UNDEFINED || !g_phys) return false;
    int &c = cache[fmt][comps];
    if (!c) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, f, &fp);
        c = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) ? 1 : -1;
    }
    return c > 0;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        g_closed = true;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static bool create_window() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"vitart";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, 1440, 816}; // 2x o framebuffer de 720x408
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME, FALSE);
    g_hwnd = CreateWindowExW(0, L"vitart", L"God of War (vitart, Vulkan)", (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME) | WS_VISIBLE,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        g_err = "CreateWindowEx falhou";
        return false;
    }
    VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    si.hinstance = wc.hInstance;
    si.hwnd = g_hwnd;
    VKC(vkCreateWin32SurfaceKHR(g_inst, &si, nullptr, &g_surface));
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, g_qfam, g_surface, &ok);
    if (!ok) {
        g_err = "a fila não apresenta nesta superfície";
        return false;
    }
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps);
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &nf, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (auto &f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM) fmt = f;
    g_swap_extent = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent : VkExtent2D{1440, 816};
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount) ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = fmt.format;
    ci.imageColorSpace = fmt.colorSpace;
    ci.imageExtent = g_swap_extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    // VITART_NOVSYNC=1 (medição): apresenta sem esperar o monitor (mailbox, ou imediato).
    if (getenv("VITART_NOVSYNC")) {
        uint32_t nm = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, nullptr);
        std::vector<VkPresentModeKHR> modes(nm);
        vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, modes.data());
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR})
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                ci.presentMode = m;
                break;
            }
    }
    ci.clipped = VK_TRUE;
    VKC(vkCreateSwapchainKHR(g_dev, &ci, nullptr, &g_swap));
    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(g_dev, g_swap, &ni, nullptr);
    g_swap_images.resize(ni);
    vkGetSwapchainImagesKHR(g_dev, g_swap, &ni, g_swap_images.data());
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKC(vkCreateSemaphore(g_dev, &sci, nullptr, &g_acquired));
    VKC(vkCreateSemaphore(g_dev, &sci, nullptr, &g_rendered));
    return true;
}

bool window_closed() { return g_closed; }

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
    if (!g_dev || it == g_targets.end() || !frame_begin()) return;
    end_pass();
    Image &dep = it->second.depth;
    barrier(dep, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearDepthStencilValue dc{1.0f, stencil};
    VkImageSubresourceRange dr{kDepthAspects, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(g_cmd, dep.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dc, 1, &dr);
    barrier(dep, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    it->second.dver++;
}

bool present(uint32_t addr) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_dev) return false;
    // O quadro vai para a fila sem esperar; o blit vem depois na mesma fila, que garante a ordem.
    if (!frame_flush(false)) return false;
    if (!g_swap && !g_hwnd && !create_window()) return false;
    if (!g_swap) return false;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    auto it = g_targets.find(addr);
    if (it == g_targets.end()) return false;
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, UINT64_MAX, g_acquired, VK_NULL_HANDLE, &idx);
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return fail("vkAcquireNextImageKHR", r);
    VKC(vkWaitForFences(g_dev, 1, &g_pfence, VK_TRUE, UINT64_MAX)); // apresentação anterior concluída
    VKC(vkResetCommandBuffer(g_pcmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(g_pcmd, &bi));
    g_cur = g_pcmd;
    Image &src = it->second.color;
    Image dst;
    dst.img = g_swap_images[idx];
    dst.fmt = VK_FORMAT_B8G8R8A8_UNORM;
    barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {(int32_t)src.w, (int32_t)src.h, 1};
    b.dstOffsets[1] = {(int32_t)g_swap_extent.width, (int32_t)g_swap_extent.height, 1};
    vkCmdBlitImage(g_pcmd, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   VK_FILTER_LINEAR);
    barrier(src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    g_cur = g_cmd;
    VKC(vkEndCommandBuffer(g_pcmd));
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_acquired;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_pcmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_rendered;
    VKC(vkResetFences(g_dev, 1, &g_pfence));
    VKC(vkQueueSubmit(g_queue, 1, &si, g_pfence));
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swap;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR(g_queue, &pi);
    static uint32_t presents = 0;
    if (++presents % 300 == 0) save_pcache();
    return true;
}

bool init(bool validation, bool window) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "vitart";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (validation) {
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
    }
    const char *iext[2] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    if (window) {
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = iext;
    }
    VKC(vkCreateInstance(&ici, nullptr, &g_inst));
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(g_inst, &n, devs.data());
    if (!n) {
        g_err = "nenhuma GPU Vulkan";
        return false;
    }
    g_phys = devs[0];
    for (auto d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) g_phys = d;
    }
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memprops);
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(g_phys, &qn, qf.data());
    for (uint32_t i = 0; i < qn; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            g_qfam = i;
            break;
        }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkPhysicalDeviceFeatures feats{};
    feats.vertexPipelineStoresAndAtomics = VK_TRUE; // só usado pelo modo de depuração do vértice
    {
        // Filtro anisotrópico (VITART_VK_ANISO, padrão 16) nas texturas com mipmap e filtro linear.
        VkPhysicalDeviceFeatures have{};
        vkGetPhysicalDeviceFeatures(g_phys, &have);
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(g_phys, &pp);
        const char *e = getenv("VITART_VK_ANISO");
        const float want = e ? (float)std::atof(e) : 16.f;
        if (have.samplerAnisotropy && want > 1.f) {
            feats.samplerAnisotropy = VK_TRUE;
            g_aniso = std::min(want, pp.limits.maxSamplerAnisotropy);
        }
    }
    dci.pEnabledFeatures = &feats;
    const char *dext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (window) {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &dext;
    }
    VKC(vkCreateDevice(g_phys, &dci, nullptr, &g_dev));
    vkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_qfam;
    VKC(vkCreateCommandPool(g_dev, &pci, nullptr, &g_pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    for (auto &cb : g_cmds) VKC(vkAllocateCommandBuffers(g_dev, &cai, &cb));
    VKC(vkAllocateCommandBuffers(g_dev, &cai, &g_pcmd));
    g_cur = g_cmd;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (auto &f : g_fences) VKC(vkCreateFence(g_dev, &fci, nullptr, &f));
    VkFenceCreateInfo pfci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    pfci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VKC(vkCreateFence(g_dev, &pfci, nullptr, &g_pfence));
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(g_phys, &pp);
        g_ts_period = pp.limits.timestampPeriod;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = 2 * kSlots;
        if (pp.limits.timestampComputeAndGraphics && vkCreateQueryPool(g_dev, &qi, nullptr, &g_qpool) != VK_SUCCESS)
            g_qpool = VK_NULL_HANDLE;
    }

    // Passo de render único (cor + profundidade, carregando o conteúdo): serve a todos os alvos.
    VkAttachmentDescription at[2]{};
    at[0].format = kColor;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = at[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[1] = at[0];
    at[1].format = kDepth;
    at[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    at[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[1].initialLayout = at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cref;
    sp.pDepthStencilAttachment = &dref;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 2;
    rci.pAttachments = at;
    rci.subpassCount = 1;
    rci.pSubpasses = &sp;
    VKC(vkCreateRenderPass(g_dev, &rci, nullptr, &g_rp));

    // Layout: set 0 = uniforms do vértice (0) e memória do vldst (1); set 1 = texturas do fragmento;
    // set 2 = uniforms e memória do fragmento.
    VkDescriptorSetLayoutBinding b0[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutBinding b1[kTexBindings];
    for (uint32_t i = 0; i < kTexBindings; i++)
        b1[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutBinding b2[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const VkDescriptorSetLayoutBinding *bs[3] = {b0, b1, b2};
    const uint32_t bn[3] = {2, kTexBindings, 2};
    for (int i = 0; i < 3; i++) {
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = bn[i];
        li.pBindings = bs[i];
        VKC(vkCreateDescriptorSetLayout(g_dev, &li, nullptr, &g_dsl[i]));
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 3;
    pli.pSetLayouts = g_dsl;
    VKC(vkCreatePipelineLayout(g_dev, &pli, nullptr, &g_layout));
    {
        // Cache de pipelines em disco: sem ele cada pipeline compila de novo a cada sessão (engasgos).
        std::vector<char> data;
        if (FILE *f = std::fopen(kPcacheFile, "rb")) {
            std::fseek(f, 0, SEEK_END);
            data.resize((size_t)std::max(0L, std::ftell(f)));
            std::fseek(f, 0, SEEK_SET);
            data.resize(std::fread(data.data(), 1, data.size(), f));
            std::fclose(f);
        }
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        ci.initialDataSize = data.size();
        ci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pcache) != VK_SUCCESS) {
            ci.initialDataSize = 0, ci.pInitialData = nullptr; // arquivo de outro driver: começa vazio
            if (vkCreatePipelineCache(g_dev, &ci, nullptr, &g_pcache) != VK_SUCCESS) g_pcache = VK_NULL_HANDLE;
        }
    }
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.maxLod = 0.25f;
    VKC(vkCreateSampler(g_dev, &sci, nullptr, &g_sampler));
    sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VKC(vkCreateSampler(g_dev, &sci, nullptr, &g_sampler_nearest));
    const uint8_t white[4] = {255, 255, 255, 255};
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_phys, &props);
    g_ssbo_align = props.limits.minStorageBufferOffsetAlignment > 16 ? props.limits.minStorageBufferOffsetAlignment : 16;
    {
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size = kRingSize;
        ci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VKC(vkCreateBuffer(g_dev, &ci, nullptr, &g_ring.buf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_dev, g_ring.buf, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VKC(vkAllocateMemory(g_dev, &ai, nullptr, &g_ring.mem));
        VKC(vkBindBufferMemory(g_dev, g_ring.buf, g_ring.mem, 0));
        g_ring.size = kRingSize;
        void *p;
        VKC(vkMapMemory(g_dev, g_ring.mem, 0, kRingSize, 0, &p));
        g_ring_ptr = static_cast<uint8_t *>(p);
    }
    {
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * kMaxDrawsPerFrame},
                                      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexBindings * kMaxDrawsPerFrame}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 3 * kMaxDrawsPerFrame;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = ps;
        for (auto &dp : g_dpools) VKC(vkCreateDescriptorPool(g_dev, &dpi, nullptr, &dp));
        select_slot(0);
    }
    // A janela nasce na primeira apresentação: as mensagens só chegam à thread que a criou, e quem
    // apresenta é a thread de vídeo, não a que inicializa.
    if (!frame_reserve(64) || !upload_texture(g_white, 1, 1, white)) return false;
    return frame_flush(true);
}

bool draw(const Draw &d) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!d.vs || !d.fs || !d.index_count || !d.vertex_count || !d.target_w || !d.target_h) return true;
    VkShaderModule vs = module(d.vs, d.vs_size), fs = module(d.fs, d.fs_size);
    if (!vs || !fs) {
        g_err = "módulo de shader recusado";
        return false;
    }
    VkPipeline pipe = pipeline(d, vs, fs);
    if (!pipe) {
        g_err = "pipeline recusado";
        return false;
    }
    const bool raw = !d.raw_attrs.empty();
    const uint32_t na = raw ? (uint32_t)std::min<size_t>(d.raw_streams.size(), 16) : (d.attr_count < 16 ? d.attr_count : 16);
    VkDeviceSize vbytes = 0;
    if (raw)
        for (const VertexStream &st : d.raw_streams) vbytes += st.size + 16;
    else
        vbytes = (VkDeviceSize)na * d.vertex_count * 16;
    VkDeviceSize need = vbytes + (VkDeviceSize)d.index_count * 4 + d.vert_uniforms_size +
                        d.frag_uniforms_size + 64 * 1024;
    std::vector<uint64_t> keys(d.textures.size());
    for (size_t i = 0; i < d.textures.size(); i++) {
        const Texture &t = d.textures[i];
        if (t.target_addr || !t.rgba) continue;
        keys[i] = t.key ? t.key : fnv(t.rgba, (size_t)t.width * t.height * 4, fnv(&t.width, 8));
        if (!g_textures.count(keys[i])) need += (VkDeviceSize)t.width * t.height * 4 + 16;
    }
    if (!frame_reserve(need)) return false;
    Target *tgt;
    if (!get_target(d.target_addr, d.target_w, d.target_h, tgt)) return false;
    if (d.depth_addr) g_depth_owner[d.depth_addr] = d.target_addr;

    // Texturas: imagens enviadas (cache por conteúdo) ou cópia de um alvo deste quadro.
    VkDescriptorImageInfo img_info[kTexBindings];
    for (auto &ii : img_info) ii = {g_sampler, g_white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (size_t ti = 0; ti < d.textures.size(); ti++) {
        const Texture &t = d.textures[ti];
        if (t.binding >= kTexBindings) continue;
        // Profundidade sempre por ponto (D32 não garante filtro linear).
        static const char *smp_dbg = getenv("VITART_VK_SAMPLER"); // depuração: "depthlinear" ou "nearest"
        img_info[t.binding].sampler = t.depth ? g_sampler_nearest : sampler_for(t);
        if (smp_dbg && !strcmp(smp_dbg, "depthlinear")) img_info[t.binding].sampler = sampler_for(t);
        if (smp_dbg && !strcmp(smp_dbg, "nearest")) img_info[t.binding].sampler = g_sampler_nearest;
        if (t.target_addr) {
            uint32_t owner = t.target_addr;
            if (t.depth) {
                auto o = g_depth_owner.find(t.target_addr);
                if (o == g_depth_owner.end()) continue;
                owner = o->second;
            }
            auto it = g_targets.find(owner);
            if (it == g_targets.end() || (!t.depth && it->first == d.target_addr)) continue;
            Image &src = t.depth ? it->second.depth : it->second.color;
            const VkImageLayout back = t.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                               : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            const VkImageAspectFlags aspect = t.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            const uint64_t ver = t.depth ? it->second.dver : it->second.cver;
            Snap &sn = g_snaps[{owner, t.depth}];
            Image &snap = sn.im;
            if (snap.img && sn.ver == ver) {
                img_info[t.binding].imageView = snap.view;
                continue;
            }
            if (!snap.img &&
                !make_image(snap, src.w, src.h, src.fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
                return false;
            sn.ver = ver;
            end_pass();
            barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            barrier(snap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {aspect, 0, 0, 1};
            c.extent = {src.w, src.h, 1};
            vkCmdCopyImage(g_cmd, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(src, back);
            barrier(snap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            g_stats.snapshots++;
            static int snaplog = getenv("VITART_VK_SNAPLOG") ? atoi(getenv("VITART_VK_SNAPLOG")) : 0;
            if (snaplog > 0) {
                snaplog--;
                if (FILE *f = fopen("vk_snap.log", "a")) {
                    fprintf(f, "desenho %u: binding %u lê %s de 0x%08x (dono 0x%08x %ux%u), desenha em 0x%08x prof 0x%08x\n",
                            g_draws_in_frame, t.binding, t.depth ? "prof" : "cor", t.target_addr, owner, src.w, src.h,
                            d.target_addr, d.depth_addr);
                    fclose(f);
                }
            }
            img_info[t.binding].imageView = snap.view;
            continue;
        }
        if (!t.rgba) continue;
        const uint64_t h = keys[ti];
        auto it = g_textures.find(h);
        if (it == g_textures.end()) {
            Image im;
            if (!upload_texture(im, t.width, t.height, t.rgba, t.mip_count != 0 && t.width > 1 && t.height > 1)) return false;
            it = g_textures.emplace(h, im).first;
        }
        img_info[t.binding].imageView = it->second.view;
        g_tex_used[h] = g_frame_no;
    }

    tgt->cver++;
    if (d.depth_write) tgt->dver++;

    VkDeviceSize voff[16];
    for (uint32_t k = 0; k < na; k++)
        voff[k] = raw ? ring_put(d.raw_streams[k].data, d.raw_streams[k].size, 16)
                      : ring_put(d.attrs + (size_t)k * d.vertex_count * 4, (VkDeviceSize)d.vertex_count * 16, 16);
    const VkDeviceSize ioff = ring_put(d.indices, (VkDeviceSize)d.index_count * 4, 4);
    const VkDeviceSize vuoff = ring_put(d.vert_uniforms, d.vert_uniforms_size, g_ssbo_align);
    const VkDeviceSize dbgoff = d.debug_vs ? ring_put(nullptr, (VkDeviceSize)d.vertex_count * 16, 4) : 0;
    const VkDeviceSize fuoff = ring_put(d.frag_uniforms, d.frag_uniforms_size, g_ssbo_align);
    const VkDeviceSize vurange = d.debug_vs ? (dbgoff + (VkDeviceSize)d.vertex_count * 16 - vuoff)
                                            : (d.vert_uniforms_size ? d.vert_uniforms_size : 16);
    const VkDeviceSize furange = d.frag_uniforms_size ? d.frag_uniforms_size : 16;
    g_draws_in_frame++;
    g_stats.draws++;
    Timer tdesc(g_stats.desc_ns);

    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = g_dpool;
    dai.descriptorSetCount = 3;
    dai.pSetLayouts = g_dsl;
    VkDescriptorSet sets[3];
    VKC(vkAllocateDescriptorSets(g_dev, &dai, sets));
    VkDescriptorBufferInfo bi[4] = {{g_ring.buf, vuoff, vurange}, {g_ring.buf, vuoff, vurange},
                                    {g_ring.buf, fuoff, furange}, {g_ring.buf, fuoff, furange}};
    VkWriteDescriptorSet w[5]{};
    for (int i = 0; i < 4; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = sets[i < 2 ? 0 : 2];
        w[i].dstBinding = i & 1;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w[4].dstSet = sets[1];
    w[4].descriptorCount = kTexBindings;
    w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[4].pImageInfo = img_info;
    vkUpdateDescriptorSets(g_dev, 5, w, 0, nullptr);

    if (g_open != tgt) {
        end_pass();
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = g_rp;
        rbi.framebuffer = tgt->fb;
        rbi.renderArea = {{0, 0}, {tgt->color.w, tgt->color.h}};
        vkCmdBeginRenderPass(g_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        g_open = tgt;
    }
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 3, sets, 0, nullptr);
    // Viewport do GXM: tela = deslocamento + escala * ndc. No Vulkan, x = desl - escala, largura = 2 * escala
    // (altura negativa inverte y, como a escala negativa do GXM).
    VkViewport vp{d.vps[0] - d.vps[1], d.vps[2] - d.vps[3], 2 * d.vps[1], 2 * d.vps[3], d.vps[4], d.vps[4] + d.vps[5]};
    // O jogo conta com o centro do pixel em coordenada inteira (como o D3D9); no Vulkan ele fica em +0,5.
    // Sem compensar, toda passada de tela cheia lê entre texels: o quadro borra e a máscara de névoa
    // vaza 1 px além dos personagens (o contorno escuro). Medido no replay: +0,5 deixa nítido e sem halo.
    static const float halfpix = getenv("VITART_VK_HALFPIX") ? (float)std::atof(getenv("VITART_VK_HALFPIX")) : 0.5f;
    const float rs = (float)render_scale();
    vp.x = (vp.x + halfpix) * rs, vp.y = (vp.y + halfpix) * rs, vp.width *= rs, vp.height *= rs;
    vp.minDepth = vp.minDepth < 0 ? 0 : (vp.minDepth > 1 ? 1 : vp.minDepth);
    vp.maxDepth = vp.maxDepth < 0 ? 0 : (vp.maxDepth > 1 ? 1 : vp.maxDepth);
    vkCmdSetViewport(g_cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {tgt->color.w, tgt->color.h}};
    vkCmdSetScissor(g_cmd, 0, 1, &sc);
    vkCmdSetStencilCompareMask(g_cmd, VK_STENCIL_FACE_FRONT_BIT, d.front.cmask);
    vkCmdSetStencilCompareMask(g_cmd, VK_STENCIL_FACE_BACK_BIT, d.back.cmask);
    vkCmdSetStencilWriteMask(g_cmd, VK_STENCIL_FACE_FRONT_BIT, d.front.wmask);
    vkCmdSetStencilWriteMask(g_cmd, VK_STENCIL_FACE_BACK_BIT, d.back.wmask);
    vkCmdSetStencilReference(g_cmd, VK_STENCIL_FACE_FRONT_BIT, d.front.ref);
    vkCmdSetStencilReference(g_cmd, VK_STENCIL_FACE_BACK_BIT, d.back.ref);
    VkBuffer bufs[16];
    for (uint32_t k = 0; k < na; k++) bufs[k] = g_ring.buf;
    if (na) vkCmdBindVertexBuffers(g_cmd, 0, na, bufs, voff);
    vkCmdBindIndexBuffer(g_cmd, g_ring.buf, ioff, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(g_cmd, d.index_count, 1, 0, 0, 0);
    if (d.debug_vs) {
        if (!frame_flush(true)) return false;
        g_debug_vs.assign(reinterpret_cast<const float *>(g_ring_ptr + dbgoff),
                          reinterpret_cast<const float *>(g_ring_ptr + dbgoff) + (size_t)d.vertex_count * 4);
    }
    return true;
}

const std::vector<float> &debug_vs_output() { return g_debug_vs; }

Stats take_stats() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Stats s = g_stats;
    g_stats = {};
    return s;
}

void end_frame() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_dev) frame_flush(true);
}

std::vector<uint32_t> targets() {
    std::vector<uint32_t> v;
    for (auto &kv : g_targets) v.push_back(kv.first);
    return v;
}

bool read_target(uint32_t addr, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!frame_flush(true)) return false;
    auto it = g_targets.find(addr);
    if (it == g_targets.end()) return false;
    Image &im = it->second.color;
    w = im.w, h = im.h;
    Buffer b;
    if (!make_buffer(b, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr)) return false;
    if (!begin_cmd()) return false;
    barrier(im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(g_cmd, im.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b.buf, 1, &c);
    barrier(im, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (!submit_wait()) return false;
    rgba.resize((size_t)w * h * 4);
    void *p;
    VKC(vkMapMemory(g_dev, b.mem, 0, b.size, 0, &p));
    std::memcpy(rgba.data(), p, rgba.size());
    vkUnmapMemory(g_dev, b.mem);
    destroy(b);
    return true;
}

void shutdown() {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_dev) return;
    frame_flush(true);
    vkDeviceWaitIdle(g_dev);
    save_pcache();
    if (g_pcache) vkDestroyPipelineCache(g_dev, g_pcache, nullptr);
    for (auto dp : g_dpools) vkDestroyDescriptorPool(g_dev, dp, nullptr);
    vkUnmapMemory(g_dev, g_ring.mem);
    destroy(g_ring);
    if (g_swap) vkDestroySwapchainKHR(g_dev, g_swap, nullptr);
    if (g_acquired) vkDestroySemaphore(g_dev, g_acquired, nullptr);
    if (g_rendered) vkDestroySemaphore(g_dev, g_rendered, nullptr);
    if (g_surface) vkDestroySurfaceKHR(g_inst, g_surface, nullptr);
    for (auto &kv : g_pipes) if (kv.second) vkDestroyPipeline(g_dev, kv.second, nullptr);
    for (auto &kv : g_modules) if (kv.second) vkDestroyShaderModule(g_dev, kv.second, nullptr);
    for (auto &kv : g_textures) destroy(kv.second);
    for (auto &kv : g_snaps) destroy(kv.second.im);
    for (auto &kv : g_targets) {
        vkDestroyFramebuffer(g_dev, kv.second.fb, nullptr);
        destroy(kv.second.color), destroy(kv.second.depth);
    }
    destroy(g_white);
    for (auto &kv : g_samplers)
        if (kv.second != g_sampler) vkDestroySampler(g_dev, kv.second, nullptr);
    vkDestroySampler(g_dev, g_sampler, nullptr);
    vkDestroySampler(g_dev, g_sampler_nearest, nullptr);
    vkDestroyPipelineLayout(g_dev, g_layout, nullptr);
    for (auto l : g_dsl) vkDestroyDescriptorSetLayout(g_dev, l, nullptr);
    vkDestroyRenderPass(g_dev, g_rp, nullptr);
    for (auto f : g_fences) vkDestroyFence(g_dev, f, nullptr);
    vkDestroyFence(g_dev, g_pfence, nullptr);
    if (g_qpool) vkDestroyQueryPool(g_dev, g_qpool, nullptr);
    vkDestroyCommandPool(g_dev, g_pool, nullptr);
    vkDestroyDevice(g_dev, nullptr);
    vkDestroyInstance(g_inst, nullptr);
    g_dev = VK_NULL_HANDLE;
}

} // namespace gvk
