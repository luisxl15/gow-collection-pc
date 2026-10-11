// Renderizador Vulkan dos desenhos do GXM (ver gxm_vk.h).
//
// Primeira versão, voltada à correção: cada desenho grava e submete o próprio command buffer e espera a
// GPU. Isso torna trivial a leitura de um alvo como textura no mesmo quadro (o alvo é copiado para uma
// imagem amostrável no momento do desenho, sem laço de realimentação). Otimizar vem depois de a imagem
// estar certa.
#include "gxm_vk.h"

#include <vulkan/vulkan.h>

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
    if (!begin_cmd()) return false;
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
    if (!submit_wait()) return false;
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

bool upload_texture(Image &im, uint32_t w, uint32_t h, const uint8_t *rgba) {
    if (!make_image(im, w, h, kColor, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    Buffer staging;
    if (!make_buffer(staging, (VkDeviceSize)w * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba))
        return false;
    if (!begin_cmd()) return false;
    barrier(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(g_cmd, staging.buf, im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(im, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    bool ok = submit_wait();
    destroy(staging);
    return ok;
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

bool init(bool validation) {
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
    return upload_texture(g_white, 1, 1, white);
}

bool draw(const Draw &d) {
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
    Target *tgt;
    if (!get_target(d.target_addr, d.target_w, d.target_h, tgt)) return false;
    if (d.depth_addr) g_depth_owner[d.depth_addr] = d.target_addr;

    // Texturas: imagens enviadas (cache por conteúdo) ou cópia de um alvo deste quadro.
    std::vector<Image> snapshots;
    VkDescriptorImageInfo img_info[kTexBindings];
    for (auto &ii : img_info) ii = {g_sampler, g_white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    for (const Texture &t : d.textures) {
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
            if (!begin_cmd()) return false;
            barrier(src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            barrier(snap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageCopy c{};
            c.srcSubresource = c.dstSubresource = {aspect, 0, 0, 1};
            c.extent = {src.w, src.h, 1};
            vkCmdCopyImage(g_cmd, src.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snap.img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(src, back);
            barrier(snap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (!submit_wait()) return false;
            snapshots.push_back(snap);
            img_info[t.binding].imageView = snap.view;
            if (t.depth) img_info[t.binding].sampler = g_sampler_nearest;
            continue;
        }
        if (!t.rgba) continue;
        const uint64_t h = fnv(t.rgba, (size_t)t.width * t.height * 4, fnv(&t.width, 8));
        auto it = g_textures.find(h);
        if (it == g_textures.end()) {
            Image im;
            if (!upload_texture(im, t.width, t.height, t.rgba)) return false;
            it = g_textures.emplace(h, im).first;
        }
        img_info[t.binding].imageView = it->second.view;
    }

    Buffer vb[16], ib, vu, fu;
    const uint32_t na = d.attr_count < 16 ? d.attr_count : 16;
    for (uint32_t k = 0; k < na; k++)
        if (!make_buffer(vb[k], (VkDeviceSize)d.vertex_count * 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         d.attrs + (size_t)k * d.vertex_count * 4))
            return false;
    if (!make_buffer(ib, (VkDeviceSize)d.index_count * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, d.indices)) return false;
    if (!make_buffer(vu, d.vert_uniforms_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, d.vert_uniforms)) return false;
    if (!make_buffer(fu, d.frag_uniforms_size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, d.frag_uniforms)) return false;

    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexBindings}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 3;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    VkDescriptorPool pool;
    VKC(vkCreateDescriptorPool(g_dev, &dpi, nullptr, &pool));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 3;
    dai.pSetLayouts = g_dsl;
    VkDescriptorSet sets[3];
    VKC(vkAllocateDescriptorSets(g_dev, &dai, sets));
    VkDescriptorBufferInfo bi[4] = {{vu.buf, 0, VK_WHOLE_SIZE}, {vu.buf, 0, VK_WHOLE_SIZE},
                                    {fu.buf, 0, VK_WHOLE_SIZE}, {fu.buf, 0, VK_WHOLE_SIZE}};
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

    if (!begin_cmd()) return false;
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = g_rp;
    rbi.framebuffer = tgt->fb;
    rbi.renderArea = {{0, 0}, {d.target_w, d.target_h}};
    vkCmdBeginRenderPass(g_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
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
    VkDeviceSize offs[16] = {};
    VkBuffer bufs[16];
    for (uint32_t k = 0; k < na; k++) bufs[k] = vb[k].buf;
    if (na) vkCmdBindVertexBuffers(g_cmd, 0, na, bufs, offs);
    vkCmdBindIndexBuffer(g_cmd, ib.buf, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(g_cmd, d.index_count, 1, 0, 0, 0);
    vkCmdEndRenderPass(g_cmd);
    const bool ok = submit_wait();
    vkDestroyDescriptorPool(g_dev, pool, nullptr);
    for (uint32_t k = 0; k < na; k++) destroy(vb[k]);
    destroy(ib), destroy(vu), destroy(fu);
    for (auto &s : snapshots) destroy(s);
    return ok;
}

void end_frame() {
    if (g_dev) vkDeviceWaitIdle(g_dev);
}

std::vector<uint32_t> targets() {
    std::vector<uint32_t> v;
    for (auto &kv : g_targets) v.push_back(kv.first);
    return v;
}

bool read_target(uint32_t addr, std::vector<uint8_t> &rgba, uint32_t &w, uint32_t &h) {
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
    if (!g_dev) return;
    vkDeviceWaitIdle(g_dev);
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
