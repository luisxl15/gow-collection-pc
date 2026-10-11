// Reproduz um quadro empacotado por tools/gxm_pack_frame.py no renderizador Vulkan do vitart e grava cada
// alvo em PNG.
//
//   gxm_replay <quadro.gxmf> <dir de saída> [--validation] [--limit N]
#include "../../runtime/vitart/gxm_vk.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {
struct Reader {
    const uint8_t *p, *end;
    bool ok = true;
    uint32_t u32() {
        if (end - p < 4) return ok = false, 0;
        uint32_t v;
        std::memcpy(&v, p, 4);
        p += 4;
        return v;
    }
    const uint8_t *bytes(size_t n) {
        if ((size_t)(end - p) < n) return ok = false, nullptr;
        const uint8_t *r = p;
        p += n;
        return r;
    }
};
} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "uso: gxm_replay <quadro.gxmf> <dir de saída> [--validation] [--limit N]\n");
        return 2;
    }
    bool validation = false;
    int limit = 1 << 30;
    for (int i = 3; i < argc; i++) {
        if (!std::strcmp(argv[i], "--validation")) validation = true;
        if (!std::strcmp(argv[i], "--limit") && i + 1 < argc) limit = std::atoi(argv[++i]);
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Reader r{data.data(), data.data() + data.size()};
    if (data.size() < 12 || std::memcmp(r.bytes(4), "GXMF", 4) != 0) {
        std::fprintf(stderr, "não é GXMF\n");
        return 1;
    }
    const uint32_t version = r.u32(), ndraws = r.u32();
    if (version != 1) {
        std::fprintf(stderr, "versão %u não suportada\n", version);
        return 1;
    }
    if (!gvk::init(validation)) {
        std::fprintf(stderr, "Vulkan: %s\n", gvk::last_error());
        return 1;
    }
    int drawn = 0, skipped = 0, failed = 0;
    for (uint32_t i = 0; i < ndraws && r.ok; i++) {
        gvk::Draw d;
        const uint32_t valid = r.u32();
        d.prim = r.u32(), d.cull = r.u32(), d.has_blend = r.u32() != 0, d.blend = r.u32();
        d.depth_func = r.u32(), d.depth_write = r.u32() != 0;
        std::memcpy(d.vps, r.bytes(24), 24);
        d.target_addr = r.u32(), d.target_w = r.u32(), d.target_h = r.u32(), d.depth_addr = r.u32();
        d.vertex_count = r.u32(), d.attr_count = r.u32(), d.index_count = r.u32();
        d.attrs = reinterpret_cast<const float *>(r.bytes((size_t)d.attr_count * d.vertex_count * 16));
        d.indices = reinterpret_cast<const uint32_t *>(r.bytes((size_t)d.index_count * 4));
        d.vert_uniforms_size = r.u32(), d.vert_uniforms = r.bytes(d.vert_uniforms_size);
        d.frag_uniforms_size = r.u32(), d.frag_uniforms = r.bytes(d.frag_uniforms_size);
        d.vs_size = r.u32(), d.vs = reinterpret_cast<const uint32_t *>(r.bytes(d.vs_size));
        d.fs_size = r.u32(), d.fs = reinterpret_cast<const uint32_t *>(r.bytes(d.fs_size));
        const uint32_t ntex = r.u32();
        for (uint32_t t = 0; t < ntex && r.ok; t++) {
            gvk::Texture tx;
            tx.binding = r.u32(), tx.target_addr = r.u32(), tx.width = r.u32(), tx.height = r.u32();
            if (!tx.target_addr) tx.rgba = r.bytes((size_t)tx.width * tx.height * 4);
            d.textures.push_back(tx);
        }
        if (!r.ok) break;
        if (!valid || (int)i >= limit || !d.vs_size || !d.fs_size) {
            skipped++;
            continue;
        }
        if (gvk::draw(d))
            drawn++;
        else {
            failed++;
            std::fprintf(stderr, "desenho %u: %s\n", i, gvk::last_error());
        }
    }
    gvk::end_frame();
    std::printf("%d desenhados, %d pulados, %d com erro%s\n", drawn, skipped, failed, r.ok ? "" : " (arquivo truncado)");
    for (uint32_t addr : gvk::targets()) {
        std::vector<uint8_t> px;
        uint32_t w, h;
        if (!gvk::read_target(addr, px, w, h)) continue;
        char path[512];
        std::snprintf(path, sizeof path, "%s/vk_%08x.png", argv[2], addr);
        stbi_write_png(path, (int)w, (int)h, 4, px.data(), (int)w * 4);
        std::printf("alvo %08x %ux%u -> %s\n", addr, w, h, path);
    }
    gvk::shutdown();
    return failed ? 1 : 0;
}
