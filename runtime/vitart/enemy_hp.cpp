// Barras de vida dos inimigos (como no GoW3). Por enquanto só o diagnóstico: VITART_ENEMYLOG=1 percorre a tabela de
// handles dos objetos do jogo e anota as criaturas com vida em enemy_log.txt.
//
// GoW2 (medido no código do medidor de chefe, 0x81029cdc): criatura +0x170 vida, +0x174 vida máxima (float); o
// medidor do HUD é vida / máxima * 100. Tabela de handles em [0x812cba94]: entrada k em +8 + 12k = {handle, ?, objeto},
// com k = handle >> 23. [0x812cbae4] -> +4 = criatura do Kratos.
#include "vitart.h"
#include "gxm_vk.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace vt {

namespace {

bool readable(uint32_t a, uint32_t n) {
    if (a < 0x81000000u || a > 0xC0000000u - n) return false;
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery(mem_base() + a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    return (uint64_t)((uint8_t *)mi.BaseAddress - mem_base()) + mi.RegionSize >= (uint64_t)a + n;
}

float rdf(uint32_t a) {
    float f;
    std::memcpy(&f, mem_base() + a, 4);
    return f;
}

// Procura a matriz da câmera: rotação ortonormal (+ translação) que põe o Kratos à frente, a uma distância de câmera.
// Dois arranjos: 3x4 por linhas (t na 4a coluna) e 4x4 por colunas (t em m[12..14]).
void scan_camera(FILE *log, uint32_t kratos, const uint32_t *enemies, int ne) {
    const float P[3] = {rdf(kratos + 0x80), rdf(kratos + 0x84), rdf(kratos + 0x88)};
    int found = 0;
    for (uint32_t a = 0x81000000u; a < 0x90000000u && found < 400;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(mem_base() + a, &mi, sizeof mi)) break;
        const uint32_t end =
            (uint32_t)std::min<uint64_t>(0x90000000ull, (uint64_t)((uint8_t *)mi.BaseAddress - mem_base()) + mi.RegionSize);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            for (uint32_t q = (a + 15) & ~15u; q + 64 <= end && found < 400; q += 16) {
                const float *m = ptr<float>(q);
                for (int lay = 0; lay < 2; lay++) {
                    // R[i][j]: lay 0 = m[4i+j] (linhas), lay 1 = m[4j+i] (colunas)
                    auto R = [&](int i, int j) { return lay == 0 ? m[4 * i + j] : m[4 * j + i]; };
                    bool ok = true;
                    for (int i = 0; i < 3 && ok; i++)
                        for (int j = i; j < 3 && ok; j++) {
                            const float dp = R(i, 0) * R(j, 0) + R(i, 1) * R(j, 1) + R(i, 2) * R(j, 2);
                            ok = std::fabs(dp - (i == j ? 1.f : 0.f)) < 2e-3f;
                        }
                    if (!ok) continue;
                    if (lay == 1 && (m[3] != 0.f || m[7] != 0.f || m[11] != 0.f || std::fabs(m[15] - 1.f) > 1e-4f)) continue;
                    const float t[3] = {lay == 0 ? m[3] : m[12], lay == 0 ? m[7] : m[13], lay == 0 ? m[11] : m[14]};
                    auto view = [&](const float *X, float *v) {
                        for (int i = 0; i < 3; i++) v[i] = R(i, 0) * X[0] + R(i, 1) * X[1] + R(i, 2) * X[2] + t[i];
                    };
                    float v[3];
                    view(P, v);
                    const float d = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    if (!(d > 50.f && d < 5000.f && std::fabs(v[0]) < 0.8f * std::fabs(v[2]) &&
                          std::fabs(v[1]) < 0.8f * std::fabs(v[2])))
                        continue;
                    // as cópias iguais (anéis de uniformes) só contam
                    static float last[3];
                    static uint32_t reps = 0;
                    if (std::fabs(v[0] - last[0]) < 0.5f && std::fabs(v[1] - last[1]) < 0.5f && std::fabs(v[2] - last[2]) < 0.5f) {
                        reps++;
                        continue;
                    }
                    if (reps) fprintf(log, "    (+%u cópias)\n", reps);
                    reps = 0, last[0] = v[0], last[1] = v[1], last[2] = v[2];
                    found++;
                    fprintf(log, "  cam? %08x lay%d kratos_v=(%.0f %.0f %.0f)", q, lay, v[0], v[1], v[2]);
                    for (int e = 0; e < ne && e < 6; e++) {
                        const float X[3] = {rdf(enemies[e] + 0x80), rdf(enemies[e] + 0x84), rdf(enemies[e] + 0x88)};
                        float w[3];
                        view(X, w);
                        fprintf(log, " e(%.0f %.0f %.0f)", w[0], w[1], w[2]);
                    }
                    fprintf(log, "\n");
                }
            }
        }
        a = end;
    }
}

void diag_gow2(uint32_t frame) {
    static FILE *log = fopen("enemy_log.txt", "w");
    if (!log) return;
    const uint32_t table = rd32(0x812cba94u);
    const uint32_t kp = rd32(0x812cbae4u);
    const uint32_t kratos = kp && readable(kp + 4, 4) ? rd32(kp + 4) : 0;
    fprintf(log, "q%u tabela=%08x kratos=%08x\n", frame, table, kratos);
    // todas as criaturas no heap (vtable do Kratos), estejam ou não na tabela de handles
    if (kratos && frame % 120 == 0) {
        const uint32_t vt_c = rd32(kratos);
        for (uint32_t a = 0x82000000u; a < 0x90000000u;) {
            MEMORY_BASIC_INFORMATION mi{};
            if (!VirtualQuery(mem_base() + a, &mi, sizeof mi)) break;
            const uint32_t end =
                (uint32_t)std::min<uint64_t>(0x90000000ull, (uint64_t)((uint8_t *)mi.BaseAddress - mem_base()) + mi.RegionSize);
            if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
                for (uint32_t q = (a + 15) & ~15u; q + 0x180 <= end; q += 16)
                    if (rd32(q) == vt_c)
                        fprintf(log, "  heap %08x hp=%g/%g pos=(%.0f %.0f %.0f)%s\n", q, rdf(q + 0x170), rdf(q + 0x174),
                                rdf(q + 0x80), rdf(q + 0x84), rdf(q + 0x88), q == kratos ? " KRATOS" : "");
            a = end;
        }
    }
    if (!table || !readable(table, 8)) return;
    uint32_t enemies[64];
    int ne = 0;
    for (uint32_t k = 0; k < 1024; k++) {
        const uint32_t e = table + 8 + 12 * k;
        if (!readable(e, 12)) break;
        const uint32_t h = rd32(e), obj = rd32(e + 8);
        if (!obj || (h >> 23) != k || !readable(obj, 0x400)) continue;
        const float hp = rdf(obj + 0x170), mx = rdf(obj + 0x174);
        if (!(mx > 0.f && mx < 1e6f && hp >= -1e3f && hp <= mx * 1.01f)) continue;
        fprintf(log, "  k%-4u obj=%08x vt=%08x flags=%08x hp=%g/%g", k, obj, rd32(obj), rd32(obj + 0x168), hp, mx);
        // candidatos a posição: floats "de mundo" nos primeiros 0x170 bytes (anotados para comparar entre quadros)
        for (uint32_t o = 0x10; o < 0x170; o += 0x10) {
            const float x = rdf(obj + o), y = rdf(obj + o + 4), z = rdf(obj + o + 8), w = rdf(obj + o + 12);
            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::fabs(w - 1.f) < 1e-4f &&
                (std::fabs(x) + std::fabs(y) + std::fabs(z)) > 1.f)
                fprintf(log, " [+%x %.1f %.1f %.1f]", o, x, y, z);
        }
        fprintf(log, obj == kratos ? " KRATOS\n" : "\n");
        if (obj != kratos && ne < 64 && mx < 5000.f) enemies[ne++] = obj;
    }
    if (kratos && frame % 600 == 0) scan_camera(log, kratos, enemies, ne);
    // The_Viewport ([0x812cc1ec], 0x3c0 bytes): a matriz da câmera está no início
    if (const uint32_t vp = rd32(0x812cc1ecu); vp && frame % 600 == 0 && readable(vp, 0x3c0)) {
        fprintf(log, "  viewport %08x:", vp);
        for (uint32_t o = 0; o < 0x3c0; o += 4) {
            if (o % 64 == 0) fprintf(log, "\n   +%03x", o);
            fprintf(log, " %g", rdf(vp + o));
        }
        fprintf(log, "\n");
    }
    // quem aponta para perto da câmera achada (0x83810bf0): dados globais e um nível de heap
    if (const char *e = getenv("VITART_CAMREF"); e && frame % 600 == 0) {
        const uint32_t target = strtoul(e, nullptr, 16);
        for (uint32_t a = 0x81235000u; a < 0x86000000u; a += 4) {
            if ((a & 0xFFFF) == 0 && !readable(a, 0x10000)) {
                a += 0xFFFC;
                continue;
            }
            const uint32_t v = rd32(a);
            if (v <= target && target - v < 0x1000) fprintf(log, "  ref %08x -> %08x (+%x)\n", a, v, target - v);
        }
    }
    fflush(log);
}

} // namespace

void enemy_hp_tick(int game) {
    static const bool diag = getenv("VITART_ENEMYLOG") != nullptr;
    static uint32_t frame = 0;
    frame++;
    if (diag && game == 2 && frame % 60 == 0) diag_gow2(frame);
}

int game_id();
bool enemy_bars_enabled(); // game_options.cpp (opção enemy_health_bars)

// No present, antes da captura: uma barra sobre cada inimigo ferido e vivo, só na ação ([0x812ec498] == 0xc, a mesma
// condição do medidor de chefe; pausa, Power Up e cenas têm outro estado). Ponto da barra = posição (+0x80) com a
// altura da cabeça somada em y, projetado por The_Viewport +0x40 (mundo -> clip, por colunas).
void enemy_hp_draw(uint32_t fb) {
    if (game_id() != 2 || !enemy_bars_enabled() || rd32(0x812ec498u) != 0xcu) return;
    static const float head = getenv("VITART_HPBAR_H") ? (float)atof(getenv("VITART_HPBAR_H")) : 42.f;
    const uint32_t vp = rd32(0x812cc1ecu), table = rd32(0x812cba94u), kp = rd32(0x812cbae4u);
    if (!vp || !table || !kp || !readable(vp, 0x80) || !readable(kp + 4, 4)) return;
    const uint32_t kratos = rd32(kp + 4);
    if (!kratos || !readable(kratos, 0x180)) return;
    const uint32_t creature_vt = rd32(kratos);
    float M[16];
    std::memcpy(M, ptr<float>(vp + 0x40), sizeof M);
    gvk::EnemyBar bars[48];
    uint32_t n = 0;
    for (uint32_t k = 0; k < 1024 && n < 48; k++) {
        const uint32_t e = table + 8 + 12 * k;
        if ((k & 63) == 0 && !readable(e, 12 * 64)) break;
        const uint32_t h = rd32(e), obj = rd32(e + 8);
        if (!obj || obj == kratos || (h >> 23) != k || !readable(obj, 0x180) || rd32(obj) != creature_vt) continue;
        const float hp = rdf(obj + 0x170), mx = rdf(obj + 0x174);
        if (static FILE *hl = getenv("VITART_ENEMYLOG") ? fopen("enemy_hp_changes.txt", "w") : nullptr; hl) {
            // floats do objeto (até +0x800) que diminuíram desde a última amostra: onde fica a vida dos inimigos comuns
            static float snap[64][512];
            static uint32_t who[64];
            static uint32_t q = 0;
            if (k >= 40 && k < 104 && ++q % 4 == 0 && readable(obj, 0x800)) {
                const uint32_t s = k - 40;
                if (who[s] == obj)
                    for (uint32_t i = 0; i < 512; i++) {
                        const float v = rdf(obj + 4 * i);
                        if (std::isfinite(v) && std::isfinite(snap[s][i]) && v < snap[s][i] && snap[s][i] - v >= 0.5f &&
                            snap[s][i] <= 10000.f && v >= -1.f && i * 4 != 0x80 && i * 4 != 0x84 && i * 4 != 0x88)
                            fprintf(hl, "k%u obj=%08x +%03x %g -> %g\n", k, obj, i * 4, snap[s][i], v);
                    }
                who[s] = obj;
                for (uint32_t i = 0; i < 512; i++) snap[s][i] = rdf(obj + 4 * i);
                fflush(hl);
            }
        }
        static const bool all = getenv("VITART_HPBAR_ALL") != nullptr; // teste: barra em todas as criaturas
        if (!(mx > 0.f && mx < 5000.f && hp > 0.f && (hp < mx || all))) continue;
        const float X = rdf(obj + 0x80), Y = rdf(obj + 0x84) + head, Z = rdf(obj + 0x88);
        const float cx = M[0] * X + M[4] * Y + M[8] * Z + M[12];
        const float cy = M[1] * X + M[5] * Y + M[9] * Z + M[13];
        const float cw = M[3] * X + M[7] * Y + M[11] * Z + M[15];
        if (!(cw > 1.f && cw < 4000.f)) continue;
        bars[n++] = {0.5f + 0.5f * cx / cw, 0.5f + 0.5f * cy / cw, hp / mx};
    }
    if (n) gvk::draw_enemy_bars(fb, bars, n);
}

} // namespace vt
