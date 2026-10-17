// Teste de GPU da variante PBR (Linux, qualquer driver Vulkan; no CI, o lavapipe do Mesa).
//
// Desenha um quadrilátero com um fragment shader do jogo traduzido em variante PBR e grava a imagem
// RGBA crua. O caminho dos parâmetros é o do runtime: vt::pbr::params lê g_Projection e as luzes de um
// default uniform buffer de vértice e vt::pbr::screen_to_view combina a inversa com o viewport no
// formato do gxm_vk.cpp (escala interna, meio pixel e altura negativa). tests/pbr_gpu/run.py monta a
// cena, chama este programa e compara com uma referência numpy.
//
//   pbr_gpu <vert.spv> <frag.spv> <vu.bin> <maps.bin> <saída.rgba> <rs>
//   vu.bin: 40 floats (g_Projection nas palavras 0..15, g_LightDir 16, g_LightColor 24, g_Ambient 32)
//   maps.bin: difusa, normal e ORM, 4x4 RGBA8 cada
#include "pbr.h"

#include <vulkan/vulkan.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"

namespace vt {
void logf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
std::string data_path() {
    const char *d = getenv("PBR_TEST_DATA");
    return d ? d : ".";
}
} // namespace vt

#define CK(x)                                                                                                          \
    do {                                                                                                               \
        VkResult r_ = (x);                                                                                             \
        if (r_ != VK_SUCCESS) {                                                                                        \
            fprintf(stderr, "%s falhou: %d (linha %d)\n", #x, r_, __LINE__);                                           \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static std::vector<char> read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "não abriu %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    std::vector<char> d((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(d.data(), 1, d.size(), f) != d.size()) exit(1);
    fclose(f);
    return d;
}

static VkDevice g_dev;
static VkPhysicalDevice g_phys;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    fprintf(stderr, "sem tipo de memória\n");
    exit(1);
}

struct Buf {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
};
static Buf make_buf(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buf b{};
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size;
    ci.usage = usage;
    CK(vkCreateBuffer(g_dev, &ci, nullptr, &b.buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g_dev, b.buf, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CK(vkAllocateMemory(g_dev, &ai, nullptr, &b.mem));
    CK(vkBindBufferMemory(g_dev, b.buf, b.mem, 0));
    CK(vkMapMemory(g_dev, b.mem, 0, size, 0, &b.map));
    std::memset(b.map, 0, size);
    return b;
}

struct Img {
    VkImage img;
    VkDeviceMemory mem;
    VkImageView view;
};
static Img make_img(uint32_t w, uint32_t h, VkImageUsageFlags usage) {
    Img im{};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    CK(vkCreateImage(g_dev, &ci, nullptr, &im.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_dev, im.img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CK(vkAllocateMemory(g_dev, &ai, nullptr, &im.mem));
    CK(vkBindImageMemory(g_dev, im.img, im.mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CK(vkCreateImageView(g_dev, &vi, nullptr, &im.view));
    return im;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static VkShaderModule module(const char *path) {
    std::vector<char> code = read_file(path);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = reinterpret_cast<const uint32_t *>(code.data());
    VkShaderModule m;
    CK(vkCreateShaderModule(g_dev, &ci, nullptr, &m));
    return m;
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "uso: pbr_gpu <vert.spv> <frag.spv> <vu.bin> <maps.bin> <saída.rgba> <rs>\n");
        return 2;
    }
    const uint32_t rs = (uint32_t)atoi(argv[6]);
    const uint32_t W = 720 * rs, H = 408 * rs;

    // ---- parâmetros pelo caminho do runtime
    std::vector<char> vub = read_file(argv[3]);
    const float *vu = reinterpret_cast<const float *>(vub.data());
    if (!vt::pbr::enabled()) {
        fprintf(stderr, "PBR desligado no pbr.ini\n");
        return 1;
    }
    vt::pbr::VertexInfo vi;
    vi.proj = 0, vi.light_dir = 16, vi.light_color = 24, vi.ambient = 32;
    float raw[gvk::kPbrParamFloats], prm[gvk::kPbrParamFloats];
    if (!vt::pbr::params(vi, vu, (uint32_t)(vub.size() / 4), 0, raw)) {
        fprintf(stderr, "params recusou a projeção\n");
        return 1;
    }
    // Viewport como no gxm_vk.cpp: vps do GXM (360, 360, 204, -204, 0, 1), meio pixel e escala.
    const float vps[6] = {360.f, 360.f, 204.f, -204.f, 0.f, 1.f};
    VkViewport vp{vps[0] - vps[1], vps[2] - vps[3], 2 * vps[1], 2 * vps[3], vps[4], vps[4] + vps[5]};
    vp.x = (vp.x + 0.5f) * rs, vp.y = (vp.y + 0.5f) * rs, vp.width *= rs, vp.height *= rs;
    std::memcpy(prm, raw, sizeof prm);
    vt::pbr::screen_to_view(raw, vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth, prm);

    // ---- dispositivo
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    CK(vkCreateInstance(&ici, nullptr, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &g_phys);
    if (!n) {
        fprintf(stderr, "sem GPU Vulkan\n");
        return 1;
    }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(g_phys, &props);
    fprintf(stderr, "GPU: %s\n", props.deviceName);
    float prio = 1.f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = 0;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    CK(vkCreateDevice(g_phys, &dci, nullptr, &g_dev));
    VkQueue q;
    vkGetDeviceQueue(g_dev, 0, 0, &q);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkCommandPool pool;
    CK(vkCreateCommandPool(g_dev, &pci, nullptr, &pool));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb;
    CK(vkAllocateCommandBuffers(g_dev, &cai, &cb));

    // ---- recursos
    Img color = make_img(W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    std::vector<char> maps = read_file(argv[4]);
    Img tex[3];
    Buf staging = make_buf(3 * 64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.map, maps.data(), 3 * 64);
    for (auto &t : tex) t = make_img(4, 4, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    Buf ub = make_buf(1 << 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    std::memcpy(static_cast<char *>(ub.map) + 4096, prm, sizeof prm); // parâmetros em 4096; uniforms zerados em 0
    Buf readback = make_buf((VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSampler smp;
    CK(vkCreateSampler(g_dev, &sci, nullptr, &smp));

    // ---- layout igual ao do gxm_vk.cpp
    VkDescriptorSetLayoutBinding b0[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
    VkDescriptorSetLayoutBinding b1[16];
    for (uint32_t i = 0; i < 16; i++) b1[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutBinding b2[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutBinding b3[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                          {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutBinding b4[1] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    const VkDescriptorSetLayoutBinding *bs[5] = {b0, b1, b2, b3, b4};
    const uint32_t bn[5] = {2, 16, 2, 2, 1};
    VkDescriptorSetLayout dsl[5];
    for (int i = 0; i < 5; i++) {
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = bn[i];
        li.pBindings = bs[i];
        CK(vkCreateDescriptorSetLayout(g_dev, &li, nullptr, &dsl[i]));
    }
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 5;
    pli.pSetLayouts = dsl;
    VkPipelineLayout layout;
    CK(vkCreatePipelineLayout(g_dev, &pli, nullptr, &layout));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 5}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 18}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 5;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    VkDescriptorPool dpool;
    CK(vkCreateDescriptorPool(g_dev, &dpi, nullptr, &dpool));
    VkDescriptorSet sets[5];
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = dpool;
    dai.descriptorSetCount = 5;
    dai.pSetLayouts = dsl;
    CK(vkAllocateDescriptorSets(g_dev, &dai, sets));
    VkDescriptorBufferInfo bi{ub.buf, 0, 4096};
    VkDescriptorImageInfo ti[16], mi[2];
    for (auto &x : ti) x = {smp, tex[0].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    mi[0] = {smp, tex[1].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    mi[1] = {smp, tex[2].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    std::vector<VkWriteDescriptorSet> ws;
    auto wbuf = [&](VkDescriptorSet s, uint32_t b) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = s, w.dstBinding = b, w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC;
        w.pBufferInfo = &bi;
        ws.push_back(w);
    };
    wbuf(sets[0], 0), wbuf(sets[0], 1), wbuf(sets[2], 0), wbuf(sets[2], 1), wbuf(sets[4], 0);
    VkWriteDescriptorSet wt{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wt.dstSet = sets[1], wt.descriptorCount = 16, wt.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wt.pImageInfo = ti;
    ws.push_back(wt);
    VkWriteDescriptorSet wm = wt;
    wm.dstSet = sets[3], wm.descriptorCount = 2, wm.pImageInfo = mi;
    ws.push_back(wm);
    vkUpdateDescriptorSets(g_dev, (uint32_t)ws.size(), ws.data(), 0, nullptr);

    // ---- render pass e pipeline
    VkAttachmentDescription at{};
    at.format = VK_FORMAT_R8G8B8A8_UNORM;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    at.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cref;
    VkRenderPassCreateInfo rci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rci.attachmentCount = 1;
    rci.pAttachments = &at;
    rci.subpassCount = 1;
    rci.pSubpasses = &sp;
    VkRenderPass rp;
    CK(vkCreateRenderPass(g_dev, &rci, nullptr, &rp));
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = rp;
    fci.attachmentCount = 1;
    fci.pAttachments = &color.view;
    fci.width = W, fci.height = H, fci.layers = 1;
    VkFramebuffer fb;
    CK(vkCreateFramebuffer(g_dev, &fci, nullptr, &fb));

    VkPipelineShaderStageCreateInfo st[2]{};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, module(argv[1]), "main", nullptr};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, module(argv[2]), "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vis{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkRect2D sc{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vps_{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps_.viewportCount = vps_.scissorCount = 1;
    vps_.pViewports = &vp;
    vps_.pScissors = &sc;
    VkPipelineRasterizationStateCreateInfo rsi{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rsi.polygonMode = VK_POLYGON_MODE_FILL;
    rsi.cullMode = VK_CULL_MODE_NONE;
    rsi.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &ba;
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = st;
    gpi.pVertexInputState = &vis;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps_;
    gpi.pRasterizationState = &rsi;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cbs;
    gpi.layout = layout;
    gpi.renderPass = rp;
    VkPipeline pipe;
    CK(vkCreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));

    // ---- gravação
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CK(vkBeginCommandBuffer(cb, &cbi));
    for (int i = 0; i < 3; i++) {
        barrier(cb, tex[i].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy c{};
        c.bufferOffset = 64 * i;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {4, 4, 1};
        vkCmdCopyBufferToImage(cb, staging.buf, tex[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        barrier(cb, tex[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    VkClearValue clear{};
    clear.color = {{0.f, 0.f, 1.f, 0.f}}; // azul com alfa 0: pixel fora do quadrilátero
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea = sc;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const uint32_t dyn[5] = {0, 0, 0, 0, 4096};
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 3, sets, 4, dyn);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 3, 2, sets + 3, 1, dyn + 4);
    vkCmdDraw(cb, 4, 1, 0, 0);
    vkCmdEndRenderPass(cb);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cb, color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buf, 1, &c);
    CK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
    CK(vkQueueWaitIdle(q));
    FILE *f = fopen(argv[5], "wb");
    fwrite(readback.map, 1, (size_t)W * H * 4, f);
    fclose(f);
    // Parâmetros finais (para a referência): 56 floats depois da imagem não; arquivo à parte.
    std::string pp = std::string(argv[5]) + ".params";
    f = fopen(pp.c_str(), "wb");
    fwrite(prm, sizeof prm, 1, f);
    fclose(f);
    vkDeviceWaitIdle(g_dev);
    return 0;
}
