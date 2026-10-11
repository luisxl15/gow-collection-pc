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

#include <mutex>

#include <cstdio>
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
constexpr VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM, kDepth = VK_FORMAT_D32_SFLOAT;

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
    uint32_t w = 0, h = 0;
    VkFormat fmt = kColor;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

bool make_image(Image &im, uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage) {
    im.w = w, im.h = h, im.fmt = fmt;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
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
    vi.subresourceRange = {(VkImageAspectFlags)(fmt == kDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    VKC(vkCreateImageView(g_dev, &vi, nullptr, &im.view));
    return true;
}

void destroy(Image &im) {
    if (im.view) vkDestroyImageView(g_dev, im.view, nullptr);
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
    b.subresourceRange = {(VkImageAspectFlags)(im.fmt == kDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, 1};
    vkCmdPipelineBarrier(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    im.layout = to;
}

bool begin_cmd() {
    VKC(vkResetCommandBuffer(g_cmd, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(g_cmd, &bi));
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
};
std::map<uint32_t, Target> g_targets;

// ------------------------------------------------------------- quadro em gravação
bool g_recording = false, g_pending = false;
Target *g_open = nullptr; // alvo com render pass aberto
Buffer g_ring;
uint8_t *g_ring_ptr = nullptr;
VkDeviceSize g_ring_off = 0, g_ssbo_align = 256;
constexpr VkDeviceSize kRingSize = 256ull << 20;
VkDescriptorPool g_dpool = VK_NULL_HANDLE;
uint32_t g_draws_in_frame = 0;
constexpr uint32_t kMaxDrawsPerFrame = 2048;
std::vector<Image> g_transient; // cópias de alvos lidas como textura: vivem até o fim do quadro

void end_pass() {
    if (g_open) {
        vkCmdEndRenderPass(g_cmd);
        g_open = nullptr;
    }
}

bool frame_wait() {
    if (g_pending) {
        VKC(vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX));
        g_pending = false;
        for (auto &im : g_transient) destroy(im);
        g_transient.clear();
    }
    return true;
}

bool frame_begin() {
    if (g_recording) return true;
    if (!frame_wait()) return false;
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
        VKC(vkEndCommandBuffer(g_cmd));
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &g_cmd;
        VKC(vkResetFences(g_dev, 1, &g_fence));
        VKC(vkQueueSubmit(g_queue, 1, &si, g_fence));
        g_pending = true;
        g_recording = false;
    }
    return !wait || frame_wait();
}

// Garante espaço no anel e no pool para um desenho; se faltar, fecha o quadro e começa outro.
bool frame_reserve(VkDeviceSize bytes) {
    if (bytes > kRingSize) {
        g_err = "upload maior que o anel";
        return false;
    }
    if (!frame_begin()) return false;
    if (g_ring_off + bytes <= kRingSize && g_draws_in_frame < kMaxDrawsPerFrame) return true;
    return frame_flush(true) && frame_begin();
}

VkDeviceSize ring_put(const void *data, VkDeviceSize n, VkDeviceSize align) {
    g_ring_off = (g_ring_off + align - 1) & ~(align - 1);
    const VkDeviceSize at = g_ring_off, len = n ? n : 16;
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
    if (!make_image(t.color, w, h, kColor,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    if (!make_image(t.depth, w, h, kDepth,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return false;
    VkImageView views[2] = {t.color.view, t.depth.view};
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
    VkImageSubresourceRange dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
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

std::unordered_map<uint64_t, Image> g_textures;
std::map<uint32_t, uint32_t> g_depth_owner; // superfície de profundidade -> alvo de cor que a usa
Image g_white;

// Grava o envio no quadro atual (o chamador já reservou os bytes no anel).
bool upload_texture(Image &im, uint32_t w, uint32_t h, const uint8_t *rgba) {
    if (!make_image(im, w, h, kColor, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    const VkDeviceSize at = ring_put(rgba, (VkDeviceSize)w * h * 4, 16);
    end_pass();
    barrier(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.bufferOffset = at;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(g_cmd, g_ring.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
}

std::unordered_map<uint64_t, VkShaderModule> g_modules;

VkShaderModule module(const uint32_t *code, uint32_t size) {
    const uint64_t h = fnv(code, size);
    auto it = g_modules.find(h);
    if (it != g_modules.end()) return it->second;
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

std::map<std::tuple<VkShaderModule, VkShaderModule, uint32_t, uint32_t, uint32_t, uint32_t>, VkPipeline> g_pipes;

VkPipeline pipeline(const Draw &d, VkShaderModule vs, VkShaderModule fs) {
    const uint32_t blend = d.has_blend ? d.blend : 0xFFFFFFFFu;
    const uint32_t depth = d.depth_func | (d.depth_write ? 8u : 0u);
    auto key = std::make_tuple(vs, fs, d.attr_count, blend, depth, d.cull);
    auto it = g_pipes.find(key);
    if (it != g_pipes.end()) return it->second;

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main"};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main"};
    std::vector<VkVertexInputBindingDescription> binds(d.attr_count);
    std::vector<VkVertexInputAttributeDescription> attrs(d.attr_count);
    for (uint32_t k = 0; k < d.attr_count; k++) {
        binds[k] = {k, 16, VK_VERTEX_INPUT_RATE_VERTEX};
        attrs[k] = {k, k, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = d.attr_count;
    vi.pVertexBindingDescriptions = binds.data();
    vi.vertexAttributeDescriptionCount = d.attr_count;
    vi.pVertexAttributeDescriptions = attrs.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE; // o sentido da face com viewport invertido ainda não foi medido
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = d.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = (VkCompareOp)(d.depth_func & 7);
    VkPipelineColorBlendAttachmentState ba{};
    if (d.has_blend) {
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
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
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
    if (vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &pci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return g_pipes[key] = p;
}

} // namespace

const char *last_error() { return g_err.c_str(); }

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

bool present(uint32_t addr) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (!g_dev) return false;
    if (!frame_flush(true)) return false;
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
    if (!begin_cmd()) return false;
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
    vkCmdBlitImage(g_cmd, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b,
                   VK_FILTER_LINEAR);
    barrier(src, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(dst, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VKC(vkEndCommandBuffer(g_cmd));
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_acquired;
    si.pWaitDstStageMask = &stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_rendered;
    VKC(vkResetFences(g_dev, 1, &g_fence));
    VKC(vkQueueSubmit(g_queue, 1, &si, g_fence));
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_rendered;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_swap;
    pi.pImageIndices = &idx;
    vkQueuePresentKHR(g_queue, &pi);
    VKC(vkWaitForFences(g_dev, 1, &g_fence, VK_TRUE, UINT64_MAX));
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
    VKC(vkAllocateCommandBuffers(g_dev, &cai, &g_cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VKC(vkCreateFence(g_dev, &fci, nullptr, &g_fence));

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
        VKC(vkCreateDescriptorPool(g_dev, &dpi, nullptr, &g_dpool));
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
    const uint32_t na = d.attr_count < 16 ? d.attr_count : 16;
    VkDeviceSize need = (VkDeviceSize)na * d.vertex_count * 16 + (VkDeviceSize)d.index_count * 4 + d.vert_uniforms_size +
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
            Image snap;
            if (!make_image(snap, src.w, src.h, src.fmt, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
                return false;
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
            g_transient.push_back(snap);
            img_info[t.binding].imageView = snap.view;
            if (t.depth) img_info[t.binding].sampler = g_sampler_nearest;
            continue;
        }
        if (!t.rgba) continue;
        const uint64_t h = keys[ti];
        auto it = g_textures.find(h);
        if (it == g_textures.end()) {
            Image im;
            if (!upload_texture(im, t.width, t.height, t.rgba)) return false;
            it = g_textures.emplace(h, im).first;
        }
        img_info[t.binding].imageView = it->second.view;
    }

    VkDeviceSize voff[16];
    for (uint32_t k = 0; k < na; k++)
        voff[k] = ring_put(d.attrs + (size_t)k * d.vertex_count * 4, (VkDeviceSize)d.vertex_count * 16, 16);
    const VkDeviceSize ioff = ring_put(d.indices, (VkDeviceSize)d.index_count * 4, 4);
    const VkDeviceSize vuoff = ring_put(d.vert_uniforms, d.vert_uniforms_size, g_ssbo_align);
    const VkDeviceSize fuoff = ring_put(d.frag_uniforms, d.frag_uniforms_size, g_ssbo_align);
    const VkDeviceSize vurange = d.vert_uniforms_size ? d.vert_uniforms_size : 16;
    const VkDeviceSize furange = d.frag_uniforms_size ? d.frag_uniforms_size : 16;
    g_draws_in_frame++;

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
        rbi.renderArea = {{0, 0}, {d.target_w, d.target_h}};
        vkCmdBeginRenderPass(g_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        g_open = tgt;
    }
    vkCmdBindPipeline(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 3, sets, 0, nullptr);
    // Viewport do GXM: tela = deslocamento + escala * ndc. No Vulkan, x = desl - escala, largura = 2 * escala
    // (altura negativa inverte y, como a escala negativa do GXM).
    VkViewport vp{d.vps[0] - d.vps[1], d.vps[2] - d.vps[3], 2 * d.vps[1], 2 * d.vps[3], d.vps[4], d.vps[4] + d.vps[5]};
    vp.minDepth = vp.minDepth < 0 ? 0 : (vp.minDepth > 1 ? 1 : vp.minDepth);
    vp.maxDepth = vp.maxDepth < 0 ? 0 : (vp.maxDepth > 1 ? 1 : vp.maxDepth);
    vkCmdSetViewport(g_cmd, 0, 1, &vp);
    VkRect2D sc{{0, 0}, {d.target_w, d.target_h}};
    vkCmdSetScissor(g_cmd, 0, 1, &sc);
    VkBuffer bufs[16];
    for (uint32_t k = 0; k < na; k++) bufs[k] = g_ring.buf;
    if (na) vkCmdBindVertexBuffers(g_cmd, 0, na, bufs, voff);
    vkCmdBindIndexBuffer(g_cmd, g_ring.buf, ioff, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(g_cmd, d.index_count, 1, 0, 0, 0);
    return true;
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
    vkDestroyDescriptorPool(g_dev, g_dpool, nullptr);
    vkUnmapMemory(g_dev, g_ring.mem);
    destroy(g_ring);
    if (g_swap) vkDestroySwapchainKHR(g_dev, g_swap, nullptr);
    if (g_acquired) vkDestroySemaphore(g_dev, g_acquired, nullptr);
    if (g_rendered) vkDestroySemaphore(g_dev, g_rendered, nullptr);
    if (g_surface) vkDestroySurfaceKHR(g_inst, g_surface, nullptr);
    for (auto &kv : g_pipes) if (kv.second) vkDestroyPipeline(g_dev, kv.second, nullptr);
    for (auto &kv : g_modules) if (kv.second) vkDestroyShaderModule(g_dev, kv.second, nullptr);
    for (auto &kv : g_textures) destroy(kv.second);
    for (auto &kv : g_targets) {
        vkDestroyFramebuffer(g_dev, kv.second.fb, nullptr);
        destroy(kv.second.color), destroy(kv.second.depth);
    }
    destroy(g_white);
    vkDestroySampler(g_dev, g_sampler, nullptr);
    vkDestroySampler(g_dev, g_sampler_nearest, nullptr);
    vkDestroyPipelineLayout(g_dev, g_layout, nullptr);
    for (auto l : g_dsl) vkDestroyDescriptorSetLayout(g_dev, l, nullptr);
    vkDestroyRenderPass(g_dev, g_rp, nullptr);
    vkDestroyFence(g_dev, g_fence, nullptr);
    vkDestroyCommandPool(g_dev, g_pool, nullptr);
    vkDestroyDevice(g_dev, nullptr);
    vkDestroyInstance(g_inst, nullptr);
    g_dev = VK_NULL_HANDLE;
}

} // namespace gvk
