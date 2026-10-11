// Teste do decodificador de texturas em C++ contra a referência em Python (tools/gxm_tex_dump.py).
//
//   gxm_tex_test <dir da captura> <lista.txt> <saída.bin>
// lista.txt: uma textura por linha, as 4 palavras em decimal. Saída: por textura, u32 ok, l, a, rgba.
#include "../../runtime/vitart/gxm_tex.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static bool safe_decode(const uint8_t *guest, const gxmtex::Info *t, std::vector<uint8_t> *rgba) {
    __try {
        return gxmtex::decode(guest, *t, *rgba);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) return 2;
    uint8_t *guest = static_cast<uint8_t *>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
    std::ifstream regions(std::string(argv[1]) + "/regions.txt");
    std::string line;
    while (std::getline(regions, line)) {
        std::istringstream ss(line);
        std::string a, s;
        if (!(ss >> a >> s)) continue;
        const uint32_t start = std::stoul(a, nullptr, 16), size = std::stoul(s, nullptr, 16);
        char path[512];
        std::snprintf(path, sizeof path, "%s/mem_%08x.bin", argv[1], start);
        std::ifstream f(path, std::ios::binary);
        if (!f) continue;
        VirtualAlloc(guest + start, size, MEM_COMMIT, PAGE_READWRITE);
        f.read(reinterpret_cast<char *>(guest + start), size);
    }
    std::ifstream list(argv[2]);
    FILE *out = std::fopen(argv[3], "wb");
    int n = 0;
    while (std::getline(list, line)) {
        std::istringstream ss(line);
        uint32_t w[4];
        if (!(ss >> w[0] >> w[1] >> w[2] >> w[3])) continue;
        const gxmtex::Info t = gxmtex::describe(w);
        std::vector<uint8_t> rgba;
        const bool ok = safe_decode(guest, &t, &rgba);
        const uint32_t hdr[3] = {ok ? 1u : 0u, t.width, t.height};
        std::fwrite(hdr, 4, 3, out);
        if (ok) std::fwrite(rgba.data(), 1, rgba.size(), out);
        n++;
    }
    std::fclose(out);
    std::printf("%d texturas\n", n);
    return 0;
}
