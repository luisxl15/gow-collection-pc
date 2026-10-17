// Renderizador Vulkan dos desenhos do GXM (ver gxm_vk.h).
//
// Os desenhos de um quadro vão para um único command buffer, com vértices, índices, uniforms e texturas
// num anel de upload mapeado e um pool de descritores por quadro. O render pass fica aberto enquanto o
// alvo não muda. Ler um alvo como textura no mesmo quadro grava uma cópia para uma imagem amostrável no
// próprio fluxo (sem laço de realimentação e sem esperar a GPU). A espera só acontece ao apresentar, ao
// ler um alvo ou quando o anel enche.
#include "gxm_vk.h"
#include "pbr.h"
#include "trophy_banner.h"
#include "post_shaders.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "user32.lib")
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
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
VkDescriptorSetLayout g_dsl[5] = {}; // 3: mapas do PBR, 4: parâmetros do PBR
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
// Sem VITART_VK_SCALE, a escala acompanha o monitor: a altura nativa do GoW (408) vezes a escala cobre a
// altura da tela (1080p -> 3, 1440p -> 4), e a apresentação reduz com filtro (supersampling).
uint32_t g_scale_override = 0; // escolhida no menu do jogo (set_render_scale)
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
// Descritores: uniforms (sets 0 e 2) num conjunto por vaga com STORAGE_BUFFER_DYNAMIC (cada desenho só
// passa os deslocamentos); texturas (set 1) num cache por combinação de imagens e samplers, num pool
// persistente que é esvaziado quando enche ou quando imagens são destruídas.
constexpr VkDeviceSize kUBWindow = 1u << 20; // janela de cada descritor dinâmico (uniforms + depuração)
VkDescriptorSet g_ubsets[kSlots][2] = {};
VkDescriptorSet g_pbrsets[kSlots] = {}; // set 4 (parâmetros do PBR por desenho, deslocamento dinâmico)
VkDescriptorPool g_tex_pool = VK_NULL_HANDLE;
constexpr uint32_t kTexSets = 4096;
std::unordered_map<uint64_t, VkDescriptorSet> g_tex_sets;
bool g_tex_sets_stale = false;

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
                g_tex_sets_stale = true;
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
    {
        // Conjuntos de uniforms da vaga: o anel inteiro, deslocamento dinâmico por desenho.
        VkDescriptorSetLayout ls[3] = {g_dsl[0], g_dsl[2], g_dsl[4]};
        VkDescriptorSet got[3];
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = g_dpool;
        dai.descriptorSetCount = 3;
        dai.pSetLayouts = ls;
        VKC(vkAllocateDescriptorSets(g_dev, &dai, got));
        g_ubsets[g_fi][0] = got[0], g_ubsets[g_fi][1] = got[1], g_pbrsets[g_fi] = got[2];
        VkDescriptorBufferInfo bi{g_ring.buf, 0, kUBWindow};
        VkWriteDescriptorSet w[5]{};
        for (int i = 0; i < 5; i++) {
            w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[i].dstSet = i < 4 ? g_ubsets[g_fi][i / 2] : g_pbrsets[g_fi];
            w[i].dstBinding = i < 4 ? i & 1 : 0;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
            w[i].pBufferInfo = &bi;
        }
        vkUpdateDescriptorSets(g_dev, 5, w, 0, nullptr);
    }
    // Cache de conjuntos de textura: esvaziado com a outra vaga parada (ela pode usar um deles).
    if (g_tex_sets_stale || g_tex_sets.size() >= kTexSets) {
        for (uint32_t i = 0; i < kSlots; i++)
            if (!slot_wait(i)) return false;
        VKC(vkResetDescriptorPool(g_dev, g_tex_pool, 0));
        g_tex_sets.clear();
        g_tex_sets_stale = false;
    }
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
    w *= scale_now(), h *= scale_now();
    if (!make_image(t.color, w, h, kColor,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_STORAGE_BIT))
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
// Mapas neutros do PBR (mapa ausente no desenho): normal reta e ORM com peso 0 (fica a cor original).
Image g_pbr_flat, g_pbr_none;

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
    uint64_t layout = (d.raw_attrs.empty() ? d.attr_count : 0x8000000000000000ull) ^ ((uint64_t)(d.topology & 3) << 58);
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
    ia.topology = d.topology == 1 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
                  : d.topology == 2 ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // o sentido da face com viewport invertido ainda não foi medido
    // Frente = horária na tela (com o viewport invertido). Medido nas sombras por volume do GoW (stencil
    // em 128, frente decrementa e verso incrementa na falha de profundidade, escurece onde < 128): com a
    // anti-horária a sombra do Kratos no convés sumia (83 px escurecidos contra 2.710).
    static const bool ccw_front = getenv("VITART_VK_FRONT_CCW") != nullptr; // depuração
    rs.frontFace = ccw_front ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
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

static bool g_resized = false, g_fullscreen = false;
static WINDOWPLACEMENT g_place{sizeof(WINDOWPLACEMENT)};
// F11 ou Alt+Enter: tela cheia sem bordas no monitor da janela (e volta ao lugar anterior).
static void toggle_fullscreen(HWND h) {
    const LONG style = GetWindowLongW(h, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi{sizeof mi};
        if (!GetWindowPlacement(h, &g_place) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(h, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(h, &g_place);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
    g_resized = true;
}

// Opção "Display Mode" do menu de pausa: o pedido vem de outra thread e é aplicado em present(), na thread
// da janela (-1 = nenhum pedido).
static std::atomic<int> g_want_borderless{-1};
void set_borderless(bool on) { g_want_borderless = on ? 1 : 0; }
bool is_borderless() { return g_fullscreen; }

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CLOSE:
        g_closed = true;
        return 0;
    case WM_SIZE:
        g_resized = true;
        break;
    case WM_KEYDOWN:
        if (w == VK_F11) return toggle_fullscreen(h), 0;
        break;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && (l & (1 << 29))) return toggle_fullscreen(h), 0; // Alt+Enter
        break;
    case WM_SYSCHAR:
        if (w == VK_RETURN) return 0; // sem o bipe do Alt+Enter
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

// Swapchain do tamanho atual da janela (recriada ao redimensionar, entrar/sair da tela cheia, restaurar).
static bool create_swapchain() {
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
    if (!g_swap_extent.width || !g_swap_extent.height) return true; // minimizada: recria quando voltar
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
    // Mailbox (sem rasgar a imagem e sem bloquear): quem dita o ritmo é o vblank emulado do vitart (30/60 do
    // jogo). Com FIFO a apresentação esperava o vblank real do monitor (74 Hz aqui) e os dois relógios se
    // desencontravam: em tela cheia sem bordas (sem o compositor no meio) caía para ~50 FPS com engasgos.
    // FIFO fica de reserva. VITART_NOVSYNC=1 (medição) aceita também o imediato.
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    {
        uint32_t nm = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, nullptr);
        std::vector<VkPresentModeKHR> modes(nm);
        vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &nm, modes.data());
        std::vector<VkPresentModeKHR> pref = {VK_PRESENT_MODE_MAILBOX_KHR};
        if (getenv("VITART_NOVSYNC")) pref.push_back(VK_PRESENT_MODE_IMMEDIATE_KHR);
        for (VkPresentModeKHR m : pref)
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                ci.presentMode = m;
                break;
            }
    }
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = g_swap;
    VkSwapchainKHR sw = VK_NULL_HANDLE;
    VKC(vkCreateSwapchainKHR(g_dev, &ci, nullptr, &sw));
    if (g_swap) vkDestroySwapchainKHR(g_dev, g_swap, nullptr);
    g_swap = sw;
    uint32_t ni = 0;
    vkGetSwapchainImagesKHR(g_dev, g_swap, &ni, nullptr);
    g_swap_images.resize(ni);
    vkGetSwapchainImagesKHR(g_dev, g_swap, &ni, g_swap_images.data());
    return true;
}

static bool g_show_pending = false; // janela criada invisível (troca de módulo), mostrar após o 1º quadro
static uint32_t g_presented = 0;
static void handoff_show() {
    if (!g_show_pending || ++g_presented < 2) return; // 2º quadro: o 1º já está na tela
    g_show_pending = false;
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    if (const char *ev = getenv("VITART_HANDOFF"))
        if (HANDLE e = OpenEventA(EVENT_MODIFY_STATE, FALSE, ev)) SetEvent(e), CloseHandle(e);
}

static bool create_window() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"vitart";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    RECT r{0, 0, 1440, 816}; // 2x o framebuffer de 720x408
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    int x = CW_USEDEFAULT, y = CW_USEDEFAULT, w = r.right - r.left, h = r.bottom - r.top;
    // Troca de módulo (menu -> jogo): a janela nasce invisível no lugar da anterior e só aparece depois
    // do primeiro quadro; aí o processo anterior é avisado e fecha (sem janela nova piscando).
    const char *wr = getenv("VITART_WINRECT");
    const bool handoff = wr && std::sscanf(wr, "%d,%d,%d,%d", &x, &y, &w, &h) == 4;
    g_show_pending = handoff;
    g_hwnd = CreateWindowExW(0, L"vitart", L"God of War (vitart, Vulkan)",
                             WS_OVERLAPPEDWINDOW | (handoff ? 0 : WS_VISIBLE), x, y, w, h, nullptr,
                             nullptr, wc.hInstance, nullptr);
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
    if (!create_swapchain()) return false;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VKC(vkCreateSemaphore(g_dev, &sci, nullptr, &g_acquired));
    VKC(vkCreateSemaphore(g_dev, &sci, nullptr, &g_rendered));
    return true;
}

bool window_closed() { return g_closed; }

bool window_rect(int &x, int &y, int &w, int &h) {
    RECT r;
    if (!g_hwnd || !GetWindowRect(g_hwnd, &r)) return false;
    x = r.left, y = r.top, w = r.right - r.left, h = r.bottom - r.top;
    return true;
}

static std::wstring g_notice;
static std::chrono::steady_clock::time_point g_notice_end;
static bool g_notice_shown = false;
static void notice_tick() {
    // Chamado a cada apresentação: põe o aviso no título e devolve o título normal ao fim do prazo.
    if (!g_hwnd) return;
    if (!g_notice.empty() && !g_notice_shown) {
        SetWindowTextW(g_hwnd, g_notice.c_str());
        g_notice_shown = true;
    } else if (g_notice_shown && std::chrono::steady_clock::now() > g_notice_end) {
        SetWindowTextW(g_hwnd, L"God of War (vitart, Vulkan)");
        g_notice.clear();
        g_notice_shown = false;
    }
}
// Aviso de troféu na própria imagem, igual ao do PS Vita (trophy_banner.cpp): entra deslizando pela
// direita, fica ~4 s e sai. Em tela cheia ou sem bordas não há título de janela para mostrar nada.
static TrophyBanner g_trophy;
static BannerImage g_trophy_img;
static uint32_t g_trophy_for = 0; // altura de tela para a qual a imagem foi montada
static bool g_trophy_on = false;
static std::chrono::steady_clock::time_point g_trophy_t0;
static Buffer g_banner_buf;
static std::vector<TrophyBanner> g_trophy_queue; // um aviso por vez (o troféu e a platina logo depois)
static void trophy_start_next() {
    if (g_trophy_queue.empty()) return;
    g_trophy = g_trophy_queue.front();
    g_trophy_queue.erase(g_trophy_queue.begin());
    g_trophy_for = 0, g_trophy_on = true;
    g_trophy_t0 = std::chrono::steady_clock::now();
    if (!g_trophy.sound.empty()) PlaySoundA(g_trophy.sound.c_str(), nullptr, SND_ASYNC | SND_FILENAME | SND_NODEFAULT);
    else MessageBeep(MB_ICONASTERISK);
}
void notify_trophy(const TrophyBanner &t) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_trophy_queue.push_back(t);
    if (!g_trophy_on) trophy_start_next();
}
static void overlay_banner(VkImage img, int32_t area_x, int32_t area_y, int32_t area_w, int32_t area_h) {
    if (!g_trophy_on) return;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_trophy_t0).count();
    constexpr double kSlide = 0.25, kStay = 4.2;
    if (t > kSlide * 2 + kStay) {
        g_trophy_on = false;
        trophy_start_next();
        return;
    }
    if (g_trophy_for != (uint32_t)area_h) {
        if (!render_trophy_banner(g_trophy, (uint32_t)area_h, g_trophy_img)) return g_trophy_on = false, void();
        g_trophy_for = (uint32_t)area_h;
    }
    const BannerImage &bi = g_trophy_img;
    const double k = area_h / 544.0;
    const int32_t margin = (int32_t)(12 * k), x_in = area_x + area_w - (int32_t)bi.w - margin, x_out = area_x + area_w + margin;
    const double f = t < kSlide ? 1 - t / kSlide : t > kSlide + kStay ? (t - kSlide - kStay) / kSlide : 0; // 0 = no lugar
    const int32_t x0 = x_in + (int32_t)((x_out - x_in) * f), y0 = area_y + (int32_t)(20 * k);
    const int32_t sw = (int32_t)g_swap_extent.width;
    if (x0 >= sw || y0 + (int32_t)bi.h > (int32_t)g_swap_extent.height || x0 < 0) return;
    const VkDeviceSize bytes = (VkDeviceSize)bi.w * bi.h * 4;
    if (g_banner_buf.size < bytes) {
        destroy(g_banner_buf);
        if (!make_buffer(g_banner_buf, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr)) return;
    }
    void *map = nullptr;
    if (vkMapMemory(g_dev, g_banner_buf.mem, 0, bytes, 0, &map) != VK_SUCCESS) return;
    std::memcpy(map, bi.px.data(), bytes);
    vkUnmapMemory(g_dev, g_banner_buf.mem);
    // uma região por linha dos cantos (só o trecho dentro da faixa) e uma para o meio; cortado na borda da tela
    std::vector<VkBufferImageCopy> regs;
    auto add = [&](uint32_t y, uint32_t rows, uint32_t from, uint32_t to) {
        const int32_t vis = std::min<int32_t>((int32_t)to, sw - x0);
        if (vis <= (int32_t)from) return;
        VkBufferImageCopy c{};
        c.bufferOffset = ((VkDeviceSize)y * bi.w + from) * 4;
        c.bufferRowLength = bi.w, c.bufferImageHeight = bi.h;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {x0 + (int32_t)from, y0 + (int32_t)y, 0};
        c.imageExtent = {(uint32_t)vis - from, rows, 1};
        regs.push_back(c);
    };
    const uint32_t r = std::min(bi.radius, bi.h / 2);
    for (uint32_t y = 0; y < r; y++) add(y, 1, bi.row_from[y], bi.row_to[y]);
    add(r, bi.h - 2 * r, 0, bi.w);
    for (uint32_t y = bi.h - r; y < bi.h; y++) add(y, 1, bi.row_from[y], bi.row_to[y]);
    if (regs.empty()) return;
    VkImageMemoryBarrier mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.oldLayout = mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_pcmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &mb);
    vkCmdCopyBufferToImage(g_pcmd, g_banner_buf.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)regs.size(), regs.data());
}

void notify(const std::string &text) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), n);
    for (wchar_t &c : w)
        if (c == L'\n') c = L' '; // o título tem uma linha só
    g_notice = w;
    g_notice_end = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    g_notice_shown = false;
}

bool pointer(float &nx, float &ny) {
    if (!g_hwnd || GetForegroundWindow() != g_hwnd || !(GetAsyncKeyState(VK_LBUTTON) & 0x8000)) return false;
    POINT p;
    RECT r;
    if (!GetCursorPos(&p) || !ScreenToClient(g_hwnd, &p) || !GetClientRect(g_hwnd, &r)) return false;
    if (p.x < 0 || p.y < 0 || p.x >= r.right || p.y >= r.bottom || r.right <= 0 || r.bottom <= 0) return false;
    nx = (p.x + 0.5f) / (float)r.right, ny = (p.y + 0.5f) / (float)r.bottom;
    return true;
}

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

// Contador de FPS (opção "FPS Counter" do menu de pausa): quadros apresentados por segundo, medidos a cada
// meio segundo, escritos no canto da imagem do jogo depois do blit para a swapchain. O texto é um bitmap
// 5x7 ampliado (sem pipeline gráfico: cópia de buffer para a imagem, que já é destino de transferência).
static std::atomic<bool> g_fps_counter{false};
static std::atomic<uint32_t> g_fps_measured{0};
void set_fps_counter(bool on) { g_fps_counter = on; }
uint32_t measured_fps() { return g_fps_measured; }
static Buffer g_ovl;
static const uint8_t *glyph5x7(char c) {
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
static void overlay_fps(VkImage img, int32_t x0, int32_t y0) {
    using clk = std::chrono::steady_clock;
    static clk::time_point t0 = clk::now();
    static uint32_t frames = 0;
    static double fps = 0;
    frames++;
    const double el = std::chrono::duration<double>(clk::now() - t0).count();
    if (el >= 0.5) fps = frames / el, frames = 0, t0 = clk::now(), g_fps_measured = (uint32_t)(fps + 0.5);
    if (!g_fps_counter) return;
    char text[16];
    std::snprintf(text, sizeof text, "%d FPS", (int)(fps + 0.5));
    const uint32_t len = (uint32_t)std::strlen(text);
    uint32_t k = (g_swap_extent.height + 135) / 270; // 1080p -> 4
    k = k < 2 ? 2 : k > 6 ? 6 : k;
    const uint32_t w = (len * 6 + 3) * k, h = 11 * k;
    if (x0 + (int32_t)w > (int32_t)g_swap_extent.width || y0 + (int32_t)h > (int32_t)g_swap_extent.height) return;
    if (!g_ovl.buf && !make_buffer(g_ovl, 512 * 1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr)) return;
    if ((VkDeviceSize)w * h * 4 > g_ovl.size) return;
    void *map = nullptr;
    if (vkMapMemory(g_dev, g_ovl.mem, 0, g_ovl.size, 0, &map) != VK_SUCCESS) return;
    uint32_t *px = static_cast<uint32_t *>(map);
    const uint32_t bg = 0xFF101010u, fg = 0xFF40FF40u; // BGRA: fundo quase preto, texto verde
    for (uint32_t i = 0; i < w * h; i++) px[i] = bg;
    for (uint32_t n = 0; n < len; n++) {
        const uint8_t *g = glyph5x7(text[n]);
        for (uint32_t r = 0; r < 7; r++)
            for (uint32_t c = 0; c < 5; c++)
                if (g[r] & (16 >> c))
                    for (uint32_t yy = 0; yy < k; yy++)
                        for (uint32_t xx = 0; xx < k; xx++)
                            px[((2 + r) * k + yy) * w + (2 + n * 6 + c) * k + xx] = fg;
    }
    vkUnmapMemory(g_dev, g_ovl.mem);
    // o blit escreveu na mesma imagem: ordena as duas escritas de transferência
    VkImageMemoryBarrier mb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.oldLayout = mb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_pcmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &mb);
    VkBufferImageCopy c{};
    c.bufferRowLength = w, c.bufferImageHeight = h;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = {x0, y0, 0};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(g_pcmd, g_ovl.buf, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

void post_at_present(uint32_t addr); // iluminação de tela (mais abaixo)
// Cena remasterizada (video_hd.cpp): imagem do próximo present no lugar do alvo do jogo.
static const uint8_t *g_vid_px = nullptr;
static uint32_t g_vid_w = 0, g_vid_h = 0;
static Image g_vid_img;
static Buffer g_vid_buf;
void set_video_frame(const uint8_t *rgba, uint32_t w, uint32_t h) { g_vid_px = rgba, g_vid_w = w, g_vid_h = h; }
void set_post_bypass(bool on);

bool present(uint32_t addr) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_dev) return false;
    post_at_present(addr);
    // O quadro vai para a fila sem esperar; o blit vem depois na mesma fila, que garante a ordem.
    if (!frame_flush(false)) return false;
    if (!g_swap && !g_hwnd && !create_window()) return false;
    if (!g_swap) return false;
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    notice_tick();
    if (const int want = g_want_borderless.exchange(-1); want >= 0 && g_hwnd && (want == 1) != g_fullscreen)
        toggle_fullscreen(g_hwnd);
    auto it = g_targets.find(addr);
    if (it == g_targets.end()) return false;
    if (g_resized) {
        g_resized = false;
        vkDeviceWaitIdle(g_dev);
        if (!create_swapchain()) return false;
    }
    if (!g_swap_extent.width || !g_swap_extent.height) return true; // minimizada
    uint32_t idx = 0;
    VkResult r = vkAcquireNextImageKHR(g_dev, g_swap, UINT64_MAX, g_acquired, VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        g_resized = true;
        return true;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) return fail("vkAcquireNextImageKHR", r);
    VKC(vkWaitForFences(g_dev, 1, &g_pfence, VK_TRUE, UINT64_MAX)); // apresentação anterior concluída
    VKC(vkResetCommandBuffer(g_pcmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(g_pcmd, &bi));
    g_cur = g_pcmd;
    Image *srcp = &it->second.color;
    if (g_vid_px) {
        const VkDeviceSize bytes = (VkDeviceSize)g_vid_w * g_vid_h * 4;
        bool ok = true;
        if (g_vid_img.w != g_vid_w || g_vid_img.h != g_vid_h) {
            destroy(g_vid_img);
            ok = make_image(g_vid_img, g_vid_w, g_vid_h, kColor, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        }
        if (ok && g_vid_buf.size < bytes) {
            destroy(g_vid_buf);
            ok = make_buffer(g_vid_buf, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr);
        }
        void *map = nullptr;
        if (ok && vkMapMemory(g_dev, g_vid_buf.mem, 0, bytes, 0, &map) == VK_SUCCESS) {
            std::memcpy(map, g_vid_px, bytes);
            vkUnmapMemory(g_dev, g_vid_buf.mem);
            barrier(g_vid_img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkBufferImageCopy c{};
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            c.imageExtent = {g_vid_w, g_vid_h, 1};
            vkCmdCopyBufferToImage(g_pcmd, g_vid_buf.buf, g_vid_img.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            srcp = &g_vid_img;
        }
        g_vid_px = nullptr;
    }
    Image &src = *srcp;
    Image dst;
    dst.img = g_swap_images[idx];
    dst.fmt = VK_FORMAT_B8G8R8A8_UNORM;
    barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    // Proporção do jogo preservada: a imagem ocupa o maior retângulo centrado; o resto fica preto.
    const VkClearColorValue black{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_pcmd, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    const double sw = g_swap_extent.width, sh = g_swap_extent.height, sa = (double)src.w / src.h;
    int32_t dw = (int32_t)sw, dh = (int32_t)(sw / sa + 0.5);
    if (dh > (int32_t)sh) dh = (int32_t)sh, dw = (int32_t)(sh * sa + 0.5);
    const int32_t dx = ((int32_t)sw - dw) / 2, dy = ((int32_t)sh - dh) / 2;
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {(int32_t)src.w, (int32_t)src.h, 1};
    b.dstOffsets[0] = {dx, dy, 0};
    b.dstOffsets[1] = {dx + dw, dy + dh, 1};
    vkCmdBlitImage(g_pcmd, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   VK_FILTER_LINEAR);
    const int32_t margin = (int32_t)(g_swap_extent.height / 60);
    overlay_fps(dst.img, dx + margin, dy + margin);
    overlay_banner(dst.img, dx, dy, dw, dh);
    if (srcp == &it->second.color) barrier(src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
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
    const VkResult pr = vkQueuePresentKHR(g_queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) g_resized = true;
    handoff_show();
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
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutBinding b1[kTexBindings];
    for (uint32_t i = 0; i < kTexBindings; i++)
        b1[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutBinding b2[2] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    // PBR: set 3 = normal map e ORM; set 4 = parâmetros por desenho (vitarecomp/usse/pbr.py).
    VkDescriptorSetLayoutBinding b3[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutBinding b4[1] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const VkDescriptorSetLayoutBinding *bs[5] = {b0, b1, b2, b3, b4};
    const uint32_t bn[5] = {2, kTexBindings, 2, 2, 1};
    for (int i = 0; i < 5; i++) {
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = bn[i];
        li.pBindings = bs[i];
        VKC(vkCreateDescriptorSetLayout(g_dev, &li, nullptr, &g_dsl[i]));
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 5;
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
        ci.size = kRingSize + kUBWindow; // folga: deslocamento + janela dinâmica cabe no buffer
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
        g_ring.size = kRingSize + kUBWindow;
        void *p;
        VKC(vkMapMemory(g_dev, g_ring.mem, 0, kRingSize + kUBWindow, 0, &p));
        g_ring_ptr = static_cast<uint8_t *>(p);
    }
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 5};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 3;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        for (auto &dp : g_dpools) VKC(vkCreateDescriptorPool(g_dev, &dpi, nullptr, &dp));
        VkDescriptorPoolSize tps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexBindings * kTexSets};
        VkDescriptorPoolCreateInfo tpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        tpi.maxSets = kTexSets;
        tpi.poolSizeCount = 1;
        tpi.pPoolSizes = &tps;
        VKC(vkCreateDescriptorPool(g_dev, &tpi, nullptr, &g_tex_pool));
        select_slot(0);
    }
    // A janela nasce na primeira apresentação: as mensagens só chegam à thread que a criou, e quem
    // apresenta é a thread de vídeo, não a que inicializa.
    const uint8_t flat[4] = {128, 128, 255, 255}, none[4] = {255, 204, 0, 0};
    if (!frame_reserve(64) || !upload_texture(g_white, 1, 1, white) || !upload_texture(g_pbr_flat, 1, 1, flat) ||
        !upload_texture(g_pbr_none, 1, 1, none))
        return false;
    return frame_flush(true);
}

// ------------------------------------------------------------------ iluminação de tela (runtime/vitart/post)
// Pós-processamento no estilo dos jogos atuais, no alvo que vai para a tela ANTES do HUD: oclusão de
// ambiente em duas escalas, profundidade de campo com foco automático, raios de luz da direção da luz do
// jogo, bloom, curva fílmica, color grading, nitidez, aberração cromática e granulação. Onde o HUD
// começa sai do quadro anterior: o índice logo depois do último desenho 3D (teste ou escrita de
// profundidade) naquele alvo. A ordem dos desenhos do jogo é estável; quadros sem nada 3D (menus,
// vídeos) ficam como estão.
struct PostRes {
    Image col, dep, a, b, c, d, focus; // cópias, meia resolução (bloom/AO, cor do DOF/raios) e o foco 1x1
    uint32_t w = 0, h = 0;
};
PostRes g_post;
constexpr uint32_t kPostBindings = 11, kPostPipes = 5;
VkDescriptorSetLayout g_post_dsl = VK_NULL_HANDLE;
VkPipelineLayout g_post_layout = VK_NULL_HANDLE;
VkPipeline g_post_pipe[kPostPipes] = {};
VkDescriptorPool g_post_pool = VK_NULL_HANDLE;
VkDescriptorSet g_post_sets[kSlots] = {};
bool g_post_failed = false;
bool g_post_bypass = false; // comparação com e sem (VITART_PBR_AB)
uint32_t g_main_addr = 0, g_main_idx = 0, g_hud_at = UINT32_MAX;
int g_last3d = -1;
bool g_post_done = false, g_post_s2v_ok = false;
float g_post_s2v[16];
float g_post_sun[3] = {0, 0, 0}, g_post_sun_col[3] = {0, 0, 0}; // luz do último desenho com PBR (câmera)
bool g_post_sun_ok = false;
uint32_t g_post_frames = 0;

struct PostPush {
    float s2v[16];
    float p[12][4];
};
static_assert(sizeof(PostPush) == 256, "push constants do pós");

bool post_init() {
    if (g_post_layout) return true;
    if (g_post_failed) return false;
    g_post_failed = true;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g_phys, &props);
    if (props.limits.maxPushConstantsSize < sizeof(PostPush)) {
        g_err = "pós-processamento desligado: a GPU aceita menos de 256 bytes de push constants";
        return false;
    }
    VkDescriptorSetLayoutBinding b[kPostBindings]{};
    const bool sampled[kPostBindings] = {true, true, false, false, true, false, false, false, true, true, false};
    for (uint32_t i = 0; i < kPostBindings; i++) {
        b[i].binding = i;
        b[i].descriptorType = sampled[i] ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = kPostBindings;
    dl.pBindings = b;
    VKC(vkCreateDescriptorSetLayout(g_dev, &dl, nullptr, &g_post_dsl));
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PostPush)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_post_dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VKC(vkCreatePipelineLayout(g_dev, &pl, nullptr, &g_post_layout));
    const uint32_t *code[kPostPipes] = {kPost_prep, kPost_blur, kPost_focus, kPost_shafts, kPost_final};
    const size_t size[kPostPipes] = {sizeof kPost_prep, sizeof kPost_blur, sizeof kPost_focus, sizeof kPost_shafts,
                                     sizeof kPost_final};
    for (uint32_t i = 0; i < kPostPipes; i++) {
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = size[i];
        mi.pCode = code[i];
        VkShaderModule m;
        VKC(vkCreateShaderModule(g_dev, &mi, nullptr, &m));
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = g_post_layout;
        const VkResult r = vkCreateComputePipelines(g_dev, g_pcache, 1, &ci, nullptr, &g_post_pipe[i]);
        vkDestroyShaderModule(g_dev, m, nullptr);
        if (r != VK_SUCCESS) return fail("pipeline do pós-processamento", r);
    }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 * kSlots},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 * kSlots}};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = kSlots;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    VKC(vkCreateDescriptorPool(g_dev, &dp, nullptr, &g_post_pool));
    VkDescriptorSetLayout ls[kSlots];
    for (auto &l : ls) l = g_post_dsl;
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = g_post_pool;
    da.descriptorSetCount = kSlots;
    da.pSetLayouts = ls;
    VKC(vkAllocateDescriptorSets(g_dev, &da, g_post_sets));
    g_post_failed = false;
    return true;
}

bool post_resources(uint32_t w, uint32_t h) {
    if (g_post.w == w && g_post.h == h) return true;
    vkDeviceWaitIdle(g_dev); // as imagens antigas podem estar em uso no outro quadro em voo
    for (Image *im : {&g_post.col, &g_post.dep, &g_post.a, &g_post.b, &g_post.c, &g_post.d, &g_post.focus}) destroy(*im);
    g_post.w = g_post.h = 0;
    const uint32_t hw = (w + 1) / 2, hh = (h + 1) / 2;
    const VkImageUsageFlags half = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkFormat f16 = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!make_image(g_post.col, w, h, kColor, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !make_image(g_post.dep, w, h, kDepth, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT) ||
        !make_image(g_post.a, hw, hh, f16, half) || !make_image(g_post.b, hw, hh, f16, half) ||
        !make_image(g_post.c, hw, hh, f16, half) || !make_image(g_post.d, hw, hh, f16, half) ||
        !make_image(g_post.focus, 1, 1, f16, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    // foco começa em 0 (o primeiro quadro pega o valor medido direto)
    barrier(g_post.focus, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange all{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(g_cmd, g_post.focus.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    g_post.w = w, g_post.h = h;
    return true;
}

void compute_barrier() {
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0,
                         nullptr, 0, nullptr);
}

bool invert4x4(const float *m, float *inv) {
    float t[16];
    t[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    t[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    t[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    t[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    t[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    t[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    t[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    t[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    t[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    t[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    t[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    t[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    t[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    t[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    t[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    t[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    if (std::fabs(det) < 1e-30f) return false;
    for (int i = 0; i < 16; i++) inv[i] = t[i] / det;
    return true;
}

// Aplica o pós-processamento no alvo (gravado no quadro atual). Marca o quadro como feito mesmo se não
// houver o que fazer, para não tentar de novo.
void run_post(Target &t) {
    g_post_done = true;
    const vt::pbr::Config &cfg = vt::pbr::config();
    static const bool env_off = getenv("VITART_POST") && *getenv("VITART_POST") == '0';
    if (env_off || g_post_bypass || !cfg.post || !g_post_s2v_ok || !post_init()) return;
    if (!frame_begin()) return;
    const uint32_t w = t.color.w, h = t.color.h;
    end_pass();
    if (!post_resources(w, h)) return;
    // cópias amostráveis da cor e da profundidade do alvo
    barrier(t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(t.depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(g_post.col, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    barrier(g_post.dep, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {w, h, 1};
    vkCmdCopyImage(g_cmd, t.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_post.col.img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImage(g_cmd, t.depth.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_post.dep.img,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(g_post.col, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(g_post.dep, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(t.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    barrier(t.color, VK_IMAGE_LAYOUT_GENERAL);
    for (Image *im : {&g_post.a, &g_post.b, &g_post.c, &g_post.d, &g_post.focus}) barrier(*im, VK_IMAGE_LAYOUT_GENERAL);

    VkDescriptorSet set = g_post_sets[g_fi];
    const VkImageLayout G = VK_IMAGE_LAYOUT_GENERAL, R = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo ii[kPostBindings] = {
        {g_sampler, g_post.col.view, R},       {g_sampler_nearest, g_post.dep.view, R}, {VK_NULL_HANDLE, g_post.a.view, G},
        {VK_NULL_HANDLE, g_post.b.view, G},    {g_sampler, g_post.a.view, G},           {VK_NULL_HANDLE, t.color.view, G},
        {VK_NULL_HANDLE, g_post.c.view, G},    {VK_NULL_HANDLE, g_post.d.view, G},      {g_sampler, g_post.c.view, G},
        {g_sampler, g_post.d.view, G},         {VK_NULL_HANDLE, g_post.focus.view, G},
    };
    VkWriteDescriptorSet wr[kPostBindings]{};
    for (uint32_t i = 0; i < kPostBindings; i++) {
        wr[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wr[i].dstSet = set;
        wr[i].dstBinding = i;
        wr[i].descriptorCount = 1;
        wr[i].descriptorType = ii[i].sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        wr[i].pImageInfo = &ii[i];
    }
    vkUpdateDescriptorSets(g_dev, kPostBindings, wr, 0, nullptr);

    // foco: pixels por unidade do jogo a 1 unidade de distância, medido na própria matriz
    auto view = [&](float x, float y, float z, float o[3]) {
        float v[4];
        for (int r = 0; r < 4; r++) v[r] = g_post_s2v[r] * x + g_post_s2v[4 + r] * y + g_post_s2v[8 + r] * z + g_post_s2v[12 + r];
        for (int k = 0; k < 3; k++) o[k] = v[k] / (std::fabs(v[3]) > 1e-12f ? v[3] : 1e-12f);
    };
    float q0[3], q1[3];
    view(w * 0.5f, h * 0.5f, 0.5f, q0);
    view(w * 0.5f + 1.f, h * 0.5f, 0.5f, q1);
    const float upp = std::sqrt((q1[0] - q0[0]) * (q1[0] - q0[0]) + (q1[1] - q0[1]) * (q1[1] - q0[1]) + (q1[2] - q0[2]) * (q1[2] - q0[2]));
    const float focal = upp > 1e-9f ? std::fabs(q0[2]) / upp : 0.f;

    // luz na tela: a direção da luz do jogo (câmera) projetada; só se estiver na frente da câmera
    float sun_px[2] = {0, 0};
    bool sun_vis = false;
    float fwd[16];
    if (g_post_sun_ok && cfg.shafts > 0 && invert4x4(g_post_s2v, fwd)) {
        const float len = std::sqrt(g_post_sun[0] * g_post_sun[0] + g_post_sun[1] * g_post_sun[1] + g_post_sun[2] * g_post_sun[2]);
        const float fz = q0[2] < 0 ? -1.f : 1.f; // sinal de z na frente da câmera
        if (len > 1e-6f && g_post_sun[2] * fz > 0.15f * len) {
            const float d = std::fabs(q0[2]) * 50.f / len;
            const float p[4] = {g_post_sun[0] * d, g_post_sun[1] * d, g_post_sun[2] * d, 1.f};
            float s[4];
            for (int r = 0; r < 4; r++) s[r] = fwd[r] * p[0] + fwd[4 + r] * p[1] + fwd[8 + r] * p[2] + fwd[12 + r] * p[3];
            if (std::fabs(s[3]) > 1e-9f) {
                sun_px[0] = s[0] / s[3], sun_px[1] = s[1] / s[3];
                sun_vis = sun_px[0] > -0.5f * w && sun_px[0] < 1.5f * w && sun_px[1] > -0.5f * h && sun_px[1] < 1.5f * h;
            }
        }
    }

    PostPush pp{};
    std::memcpy(pp.s2v, g_post_s2v, sizeof pp.s2v);
    const float hw = (float)((w + 1) / 2), hh = (float)((h + 1) / 2), rs = (float)scale_now();
    auto set4 = [&](int i, float a, float b, float c2, float d) { pp.p[i][0] = a, pp.p[i][1] = b, pp.p[i][2] = c2, pp.p[i][3] = d; };
    set4(0, focal, cfg.ssao_radius, focal > 0 ? cfg.ssao : 0.f, cfg.bloom_threshold);
    set4(1, cfg.bloom, cfg.exposure, cfg.tonemap, cfg.saturation);
    set4(2, cfg.vignette, (float)w, (float)h, 0.f);
    set4(3, hw, hh, (float)(g_post_frames++ & 63), 1.5f * rs);
    set4(4, focal > 0 ? cfg.dof : 0.f, cfg.dof_range, cfg.dof_near ? 1.f : 0.f, 1.25f * rs);
    set4(5, sun_px[0], sun_px[1], cfg.shafts, 0.965f);
    const float sc = std::max({g_post_sun_col[0], g_post_sun_col[1], g_post_sun_col[2], 1e-3f});
    set4(6, g_post_sun_col[0] / sc, g_post_sun_col[1] / sc, g_post_sun_col[2] / sc, sun_vis ? 1.f : 0.f);
    set4(7, cfg.grain, cfg.chroma, cfg.sharpen, cfg.focus_speed);
    set4(8, cfg.lift[0], cfg.lift[1], cfg.lift[2], cfg.grade);
    set4(9, cfg.gain[0], cfg.gain[1], cfg.gain[2], cfg.contrast);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_post_layout, 0, 1, &set, 0, nullptr);
    auto run = [&](int pipe, uint32_t gw, uint32_t gh) {
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_post_pipe[pipe]);
        vkCmdPushConstants(g_cmd, g_post_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pp, &pp);
        vkCmdDispatch(g_cmd, (gw + 7) / 8, (gh + 7) / 8, 1);
        compute_barrier();
    };
    run(0, (uint32_t)hw, (uint32_t)hh); // bloom, AO, cor e céu
    pp.p[2][3] = 0.f;
    run(1, (uint32_t)hw, (uint32_t)hh); // desfoque horizontal
    pp.p[2][3] = 1.f;
    run(1, (uint32_t)hw, (uint32_t)hh); // vertical
    if (cfg.dof > 0) {
        vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_post_pipe[2]);
        vkCmdPushConstants(g_cmd, g_post_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pp, &pp);
        vkCmdDispatch(g_cmd, 1, 1, 1); // foco automático
        compute_barrier();
    }
    run(3, (uint32_t)hw, (uint32_t)hh); // raios de luz (zera uD sem luz)
    run(4, w, h);                       // final
    barrier(t.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    t.cver++;
}

// Chamado em cada desenho, antes de gravá-lo: conta os desenhos do alvo da tela e aplica o pós no ponto
// onde o HUD começou no quadro anterior.
void post_before_draw(const Draw &d, Target &t) {
    if (d.target_addr != g_main_addr) return;
    const bool is3d = d.depth_func != 7 || d.depth_write;
    const uint32_t idx = g_main_idx++;
    if (!is3d && !g_post_done && g_hud_at != UINT32_MAX && idx >= g_hud_at) run_post(t);
    if (is3d) g_last3d = (int)idx;
}

void set_post_bypass(bool on) { g_post_bypass = on; }

// No present: o quadro que não chegou ao ponto do HUD (sem HUD) recebe o pós aqui; prepara o próximo.
void post_at_present(uint32_t addr) {
    if (addr == g_main_addr && !g_post_done && g_last3d >= 0) {
        auto it = g_targets.find(addr);
        if (it != g_targets.end()) run_post(it->second);
    }
    g_hud_at = (addr == g_main_addr && g_last3d >= 0) ? (uint32_t)g_last3d + 1 : UINT32_MAX;
    g_main_addr = addr;
    g_main_idx = 0, g_last3d = -1, g_post_done = false, g_post_s2v_ok = false, g_post_sun_ok = false;
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
    VkDeviceSize need = vbytes + (VkDeviceSize)d.index_count * (d.index_data ? d.index_size : 4) + 8 + d.vert_uniforms_size +
                        d.frag_uniforms_size + 64 * 1024;
    std::vector<uint64_t> keys(d.textures.size());
    for (size_t i = 0; i < d.textures.size(); i++) {
        const Texture &t = d.textures[i];
        if (t.target_addr || !t.rgba) continue;
        keys[i] = t.key ? t.key : fnv(t.rgba, (size_t)t.width * t.height * 4, fnv(&t.width, 8));
        if (!g_textures.count(keys[i])) need += (VkDeviceSize)t.width * t.height * 4 + 16;
    }
    if (d.pbr) {
        need += sizeof d.pbr_params + g_ssbo_align;
        for (const Texture &t : d.pbr_maps)
            if (t.rgba && t.key && !g_textures.count(t.key)) need += (VkDeviceSize)t.width * t.height * 4 + 16;
    }
    if (!frame_reserve(need)) return false;
    Target *tgt;
    if (!get_target(d.target_addr, d.target_w, d.target_h, tgt)) return false;
    if (d.depth_addr) g_depth_owner[d.depth_addr] = d.target_addr;
    post_before_draw(d, *tgt);

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

    // PBR: normal map e ORM (cache por conteúdo, com mipmaps) no set 3, cacheado como as texturas.
    VkDescriptorSet pbr_set = VK_NULL_HANDLE;
    if (d.pbr) {
        VkDescriptorImageInfo mi[2] = {{g_sampler, g_pbr_flat.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {g_sampler, g_pbr_none.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        for (int k = 0; k < 2; k++) {
            const Texture &t = d.pbr_maps[k];
            if (!t.rgba || !t.key) continue;
            auto it = g_textures.find(t.key);
            if (it == g_textures.end()) {
                Image im;
                if (!upload_texture(im, t.width, t.height, t.rgba, t.width > 1 && t.height > 1)) return false;
                it = g_textures.emplace(t.key, im).first;
            }
            mi[k].imageView = it->second.view;
            mi[k].sampler = sampler_for(t);
            g_tex_used[t.key] = g_frame_no;
        }
        uint64_t key = 0x5042520000000000ull; // "PBR": não colide com os conjuntos do set 1
        for (const VkDescriptorImageInfo &ii : mi) {
            key = (key ^ (uint64_t)(uintptr_t)ii.imageView) * 1099511628211ull;
            key = (key ^ (uint64_t)(uintptr_t)ii.sampler) * 1099511628211ull;
        }
        auto it = g_tex_sets.find(key);
        if (it != g_tex_sets.end()) {
            pbr_set = it->second;
        } else {
            VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = g_tex_pool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &g_dsl[3];
            VKC(vkAllocateDescriptorSets(g_dev, &dai, &pbr_set));
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = pbr_set;
            w.descriptorCount = 2;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = mi;
            vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);
            g_tex_sets[key] = pbr_set;
        }
    }

    tgt->cver++;
    if (d.depth_write) tgt->dver++;

    VkDeviceSize voff[16];
    for (uint32_t k = 0; k < na; k++)
        voff[k] = raw ? ring_put(d.raw_streams[k].data, d.raw_streams[k].size, 16)
                      : ring_put(d.attrs + (size_t)k * d.vertex_count * 4, (VkDeviceSize)d.vertex_count * 16, 16);
    const VkDeviceSize ioff = d.index_data ? ring_put(d.index_data, (VkDeviceSize)d.index_count * d.index_size, 4)
                                           : ring_put(d.indices, (VkDeviceSize)d.index_count * 4, 4);
    const VkDeviceSize vuoff = ring_put(d.vert_uniforms, d.vert_uniforms_size, g_ssbo_align);
    const VkDeviceSize dbgoff = d.debug_vs ? ring_put(nullptr, (VkDeviceSize)d.vertex_count * 16, 4) : 0;
    const VkDeviceSize fuoff = ring_put(d.frag_uniforms, d.frag_uniforms_size, g_ssbo_align);
    const VkDeviceSize vurange = d.debug_vs ? (dbgoff + (VkDeviceSize)d.vertex_count * 16 - vuoff)
                                            : (d.vert_uniforms_size ? d.vert_uniforms_size : 16);
    const VkDeviceSize furange = d.frag_uniforms_size ? d.frag_uniforms_size : 16;
    g_draws_in_frame++;
    g_stats.draws++;
    Timer tdesc(g_stats.desc_ns);

    (void)vurange, (void)furange;
    VkDescriptorSet sets[3] = {g_ubsets[g_fi][0], VK_NULL_HANDLE, g_ubsets[g_fi][1]};
    {
        uint64_t key = 1469598103934665603ull;
        for (const VkDescriptorImageInfo &ii : img_info) {
            key = (key ^ (uint64_t)(uintptr_t)ii.imageView) * 1099511628211ull;
            key = (key ^ (uint64_t)(uintptr_t)ii.sampler) * 1099511628211ull;
        }
        auto it = g_tex_sets.find(key);
        if (it != g_tex_sets.end()) {
            sets[1] = it->second;
        } else {
            VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dai.descriptorPool = g_tex_pool;
            dai.descriptorSetCount = 1;
            dai.pSetLayouts = &g_dsl[1];
            VKC(vkAllocateDescriptorSets(g_dev, &dai, &sets[1]));
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = sets[1];
            w.descriptorCount = kTexBindings;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = img_info;
            vkUpdateDescriptorSets(g_dev, 1, &w, 0, nullptr);
            g_tex_sets[key] = sets[1];
        }
    }
    const uint32_t dyn[4] = {(uint32_t)vuoff, (uint32_t)vuoff, (uint32_t)fuoff, (uint32_t)fuoff};

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
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 3, sets, 4, dyn);
    // Viewport do GXM: tela = deslocamento + escala * ndc. No Vulkan, x = desl - escala, largura = 2 * escala
    // (altura negativa inverte y, como a escala negativa do GXM).
    VkViewport vp{d.vps[0] - d.vps[1], d.vps[2] - d.vps[3], 2 * d.vps[1], 2 * d.vps[3], d.vps[4], d.vps[4] + d.vps[5]};
    // O jogo conta com o centro do pixel em coordenada inteira (como o D3D9); no Vulkan ele fica em +0,5.
    // Sem compensar, toda passada de tela cheia lê entre texels: o quadro borra e a máscara de névoa
    // vaza 1 px além dos personagens (o contorno escuro). Medido no replay: +0,5 deixa nítido e sem halo.
    static const float halfpix = getenv("VITART_VK_HALFPIX") ? (float)std::atof(getenv("VITART_VK_HALFPIX")) : 0.5f;
    const float rs = (float)scale_now();
    vp.x = (vp.x + halfpix) * rs, vp.y = (vp.y + halfpix) * rs, vp.width *= rs, vp.height *= rs;
    vp.minDepth = vp.minDepth < 0 ? 0 : (vp.minDepth > 1 ? 1 : vp.minDepth);
    vp.maxDepth = vp.maxDepth < 0 ? 0 : (vp.maxDepth > 1 ? 1 : vp.maxDepth);
    vkCmdSetViewport(g_cmd, 0, 1, &vp);
    if (d.view_inv_ok && d.target_addr == g_main_addr && (d.depth_func != 7 || d.depth_write)) {
        vt::pbr::screen_to_view(d.view_inv, vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth, g_post_s2v);
        g_post_s2v_ok = true;
    }
    if (d.pbr && d.target_addr == g_main_addr) {
        std::memcpy(g_post_sun, d.pbr_params + 16, 12);
        std::memcpy(g_post_sun_col, d.pbr_params + 20, 12);
        g_post_sun_ok = true;
    }
    if (d.pbr) {
        float prm[kPbrParamFloats];
        std::memcpy(prm, d.pbr_params, sizeof prm);
        // Pixel -> câmera: a inversa de g_Projection vezes a inversa do viewport usado na GPU.
        vt::pbr::screen_to_view(d.pbr_params, vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth, prm);
        const uint32_t poff = (uint32_t)ring_put(prm, sizeof prm, g_ssbo_align);
        VkDescriptorSet s34[2] = {pbr_set, g_pbrsets[g_fi]};
        vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 3, 2, s34, 1, &poff);
    }
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
    vkCmdBindIndexBuffer(g_cmd, g_ring.buf, ioff,
                         d.index_data && d.index_size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(g_cmd, d.index_count, 1, 0, d.index_data ? d.vertex_offset : 0, 0);
    if (d.debug_vs) {
        if (!frame_flush(true)) return false;
        g_debug_vs.assign(reinterpret_cast<const float *>(g_ring_ptr + dbgoff),
                          reinterpret_cast<const float *>(g_ring_ptr + dbgoff) + (size_t)d.vertex_count * 4);
    }
    return true;
}

const std::vector<float> &debug_vs_output() { return g_debug_vs; }

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

// Troca a resolução interna com o jogo rodando: cada alvo é recriado na escala nova e recebe a imagem
// atual reduzida/ampliada com filtro (a tela de pausa não pisca); a profundidade volta limpa, as cópias
// amostráveis e os conjuntos de textura que apontavam para as imagens velhas são refeitos sob demanda.
uint32_t render_scale() { return scale_now(); }
uint32_t auto_render_scale() { return scale_auto(); }
bool set_render_scale(uint32_t s) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    s = s < 1 ? 1 : s > 8 ? 8 : s;
    const uint32_t cur = scale_now();
    if (s == cur) return true;
    if (!g_dev || g_targets.empty()) {
        g_scale_override = s;
        return true;
    }
    if (!frame_flush(true)) return false;
    vkDeviceWaitIdle(g_dev);
    if (!begin_cmd()) return false;
    std::vector<Image> old;
    std::vector<VkFramebuffer> old_fb;
    for (auto &kv : g_targets) {
        Target &t = kv.second;
        const uint32_t w = t.color.w / cur * s, h = t.color.h / cur * s;
        Target n;
        if (!make_image(n.color, w, h, kColor,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_STORAGE_BIT) ||
            !make_image(n.depth, w, h, kDepth,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            return false;
        VkImageView views[2] = {n.color.view, n.depth.att_view};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g_rp;
        fi.attachmentCount = 2;
        fi.pAttachments = views;
        fi.width = w, fi.height = h, fi.layers = 1;
        VKC(vkCreateFramebuffer(g_dev, &fi, nullptr, &n.fb));
        barrier(t.color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(n.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit b{};
        b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {(int32_t)t.color.w, (int32_t)t.color.h, 1};
        b.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
        vkCmdBlitImage(g_cmd, t.color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, n.color.img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        barrier(n.depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearDepthStencilValue dc{1.0f, 0};
        VkImageSubresourceRange dr{kDepthAspects, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(g_cmd, n.depth.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dc, 1, &dr);
        barrier(n.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        barrier(n.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        n.cver = t.cver + 1, n.dver = t.dver + 1;
        old.push_back(t.color), old.push_back(t.depth), old_fb.push_back(t.fb);
        t = n;
    }
    if (!submit_wait()) return false;
    for (auto &im : old) destroy(im);
    for (auto fb : old_fb) vkDestroyFramebuffer(g_dev, fb, nullptr);
    for (auto &kv : g_snaps) destroy(kv.second.im);
    g_snaps.clear();
    g_tex_sets_stale = true;
    g_scale_override = s;
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
    vkDestroyDescriptorPool(g_dev, g_tex_pool, nullptr);
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
    destroy(g_ovl);
    destroy(g_banner_buf);
    for (auto &kv : g_targets) {
        vkDestroyFramebuffer(g_dev, kv.second.fb, nullptr);
        destroy(kv.second.color), destroy(kv.second.depth);
    }
    destroy(g_white);
    destroy(g_pbr_flat), destroy(g_pbr_none);
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
