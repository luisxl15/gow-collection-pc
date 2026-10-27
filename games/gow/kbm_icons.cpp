// troca o quadradinho/bolinha pelas teclas quando você tá no teclado
// (a explicação comprida mora em docs/teclado-mouse-qte.md)
#include "vitart.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace vt {
std::string mods_dir_for(const std::string &psarc_name); // hle_io.cpp: pasta do patch (com o mod ligado)

int game_id();           // game_options.cpp
int input_icon_mode(); // hle_sys.cpp: 0 = PlayStation, 1 = teclado/mouse, 2 = Xbox
bool icon_atlas_available(int mode); // hle_gxm.cpp: há atlas de ícones instalado para o modo

namespace {

struct Swap {
    uint32_t off;
    uint8_t ps[10], kb[10];
};
struct FlpTable {
    std::string name;
    std::vector<Swap> swaps;
};

std::vector<uint8_t> unhex(const std::string &h) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i + 1 < h.size(); i += 2) v.push_back((uint8_t)std::strtoul(h.substr(i, 2).c_str(), nullptr, 16));
    return v;
}

std::vector<FlpTable> load_tables(int game) {
    std::vector<FlpTable> out;
    const std::string psarc = game == 1 ? "gow1_psp2_SCEA.psarc" : "gow2_psp2_SCEA.psarc";
    std::ifstream f(mods_dir_for(psarc) + "/" + psarc + ".kbm.tsv");
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream is(line);
        if (line.rfind("flp ", 0) == 0) {
            FlpTable t; // "flp <arquivo do WAD> ..." (a assinatura antiga do arquivo não é usada: ver find_flp)
            std::string tag;
            is >> tag >> t.name;
            out.push_back(std::move(t));
        } else if (!out.empty()) {
            Swap s{};
            std::string ps, kb;
            is >> s.off >> ps >> kb;
            const auto a = unhex(ps), b = unhex(kb);
            if (a.size() != 10 || b.size() != 10) continue;
            std::memcpy(s.ps, a.data(), 10), std::memcpy(s.kb, b.data(), 10);
            out.back().swaps.push_back(s);
        }
    }
    return out;
}

// caça o FLP na memória; 0 se ele se escondeu bem
uint32_t find_flp(const FlpTable &t) {
    if (t.swaps.size() < 2) return 0;
    const Swap &s0 = t.swaps[0];
    uint8_t *base = mem_base();
    auto confirm = [&](uint32_t flp) {
        for (size_t k = 1; k < t.swaps.size() && k < 9; k++)
            if (std::memcmp(base + flp + t.swaps[k].off, t.swaps[k].ps, 10) && std::memcmp(base + flp + t.swaps[k].off, t.swaps[k].kb, 10))
                return false;
        return true;
    };
    for (uint64_t a = 0x80000000ull; a < 0x100000000ull;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(base + a, &mi, sizeof mi)) break;
        const uint64_t start = (uint64_t)((uint8_t *)mi.BaseAddress - base), end = start + mi.RegionSize;
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) && end - start >= 10) {
            const uint8_t *p = base + start, *e = base + end - 10;
            while (p <= e) {
                p = static_cast<const uint8_t *>(std::memchr(p, s0.ps[0], (size_t)(e - p) + 1));
                if (!p) break;
                if (!std::memcmp(p, s0.ps, 10)) {
                    const uint32_t at = (uint32_t)(p - base);
                    // o FLP inteiro tem de caber na região mapeada (a última chave dentro dela)
                    if (at >= s0.off && (uint64_t)at - s0.off + t.swaps.back().off + 10 <= end && confirm(at - s0.off))
                        return at - s0.off;
                }
                p++;
            }
        }
        a = end;
    }
    return 0;
}

// "wad/_VitaTV/r_shella.wad_psp2" -> "r_shella.wad_psp2"
std::string base_name(const std::string &n) {
    const size_t k = n.find_last_of('/');
    return k == std::string::npos ? n : n.substr(k + 1);
}

// ainda tá lá? só conferindo
bool still_there(const FlpTable &t, uint32_t at) {
    const uint8_t *p = ptr<uint8_t>(at + t.swaps[0].off);
    return !std::memcmp(p, t.swaps[0].ps, 10) || !std::memcmp(p, t.swaps[0].kb, 10);
}

void apply(const FlpTable &t, uint32_t at, bool kbm) {
    for (const Swap &s : t.swaps) {
        uint8_t *p = ptr<uint8_t>(at + s.off);
        const uint8_t *want = kbm ? s.kb : s.ps, *other = kbm ? s.ps : s.kb;
        if (std::memcmp(p, want, 10) && !std::memcmp(p, other, 10)) std::memcpy(p, want, 10); // só mexe no que é nosso
    }
}

void worker() {
    int game = 0;
    while (!(game = game_id())) std::this_thread::sleep_for(std::chrono::seconds(1));
    std::vector<FlpTable> tables = load_tables(game);
    tables.erase(std::remove_if(tables.begin(), tables.end(), [](const FlpTable &t) { return t.swaps.empty(); }), tables.end());
    if (tables.empty()) return;
    logf("vitart: ícones de teclado: %zu FLP(s) na tabela", tables.size());
    std::vector<uint32_t> where(tables.size(), 0);
    std::vector<int> applied(tables.size(), -1);
    uint32_t scans = 0;
    for (;;) {
        const bool kbm = icon_atlas_available(input_icon_mode());
        for (size_t i = 0; i < tables.size(); i++) {
            if (where[i] && !still_there(tables[i], where[i])) where[i] = 0, applied[i] = -1;
            if (!where[i]) {
                // paciência: tenta de novo daqui a pouco
                if (scans % 8) continue;
                bool twin = false;      // a outra versão (portátil / PS TV) do mesmo arquivo já está carregada
                for (size_t j = 0; j < tables.size(); j++)
                    twin |= j != i && where[j] && base_name(tables[j].name) == base_name(tables[i].name);
                if (twin) continue;
                const auto t0 = std::chrono::steady_clock::now();
                where[i] = find_flp(tables[i]);
                if (where[i])
                    logf("vitart: ícones de teclado: FLP de %s em 0x%08x (%.0f ms)", tables[i].name.c_str(), where[i],
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            }
            if (where[i] && applied[i] != (int)kbm) apply(tables[i], where[i], kbm), applied[i] = kbm;
        }
        scans++;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

} // namespace

// liga a thread logo no começo do jogo
void kbm_icons_start() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(worker).detach(); });
}

} // namespace vt
