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
#include <map>
#include <vector>
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
std::vector<uint32_t> g_cams; // matrizes achadas na última varredura
void scan_camera(FILE *log, uint32_t kratos, const uint32_t *enemies, int ne, uint32_t pos_off = 0x80) {
    g_cams.clear();
    const float P[3] = {rdf(kratos + pos_off), rdf(kratos + pos_off + 4), rdf(kratos + pos_off + 8)};
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
                    if (g_cams.size() < 12) g_cams.push_back(q);
                    for (int e = 0; e < ne && e < 6; e++) {
                        const float X[3] = {rdf(enemies[e] + pos_off), rdf(enemies[e] + pos_off + 4), rdf(enemies[e] + pos_off + 8)};
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

// matriz mundo -> clip (4x4 por colunas, como a do GoW2 em The_Viewport +0x40): a linha do w é um vetor unitário
// (a direção da câmera) e o ponto P cai dentro da tela com w > 0; os inimigos também, se houver
std::vector<uint32_t> g_clips;
void scan_clip(FILE *log, const float *P, const float (*E)[3], int ne) {
    g_clips.clear();
    for (uint32_t a = 0x81000000u; a < 0x90000000u && g_clips.size() < 16;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(mem_base() + a, &mi, sizeof mi)) break;
        const uint32_t end =
            (uint32_t)std::min<uint64_t>(0x90000000ull, (uint64_t)((uint8_t *)mi.BaseAddress - mem_base()) + mi.RegionSize);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint32_t q = (a + 15) & ~15u; q + 64 <= end && g_clips.size() < 16; q += 16) {
                const float *m = ptr<float>(q);
                const float wl = m[3] * m[3] + m[7] * m[7] + m[11] * m[11];
                if (!(std::fabs(wl - 1.f) < 0.01f) || !std::isfinite(m[15])) continue;
                auto clip = [&](const float *X, float &sx, float &sy, float &w) {
                    w = m[3] * X[0] + m[7] * X[1] + m[11] * X[2] + m[15];
                    sx = (m[0] * X[0] + m[4] * X[1] + m[8] * X[2] + m[12]) / w;
                    sy = (m[1] * X[0] + m[5] * X[1] + m[9] * X[2] + m[13]) / w;
                };
                float sx, sy, w;
                clip(P, sx, sy, w);
                if (!(w > 1.f && w < 20000.f && std::fabs(sx) < 1.f && std::fabs(sy) < 1.f)) continue;
                // câmera de verdade: linhas x, y e w ortogonais, escalas x/y de perspectiva, distância razoável
                const float xr[3] = {m[0], m[4], m[8]}, yr[3] = {m[1], m[5], m[9]}, wr[3] = {m[3], m[7], m[11]};
                auto dot = [](const float *u, const float *v) { return u[0] * v[0] + u[1] * v[1] + u[2] * v[2]; };
                const float lx = std::sqrt(dot(xr, xr)), ly = std::sqrt(dot(yr, yr));
                if (!(lx > 0.5f && lx < 5.f && ly > 0.5f && ly < 5.f) || w < 50.f || w > 5000.f) continue;
                if (std::fabs(dot(xr, wr)) > 0.02f * lx || std::fabs(dot(yr, wr)) > 0.02f * ly ||
                    std::fabs(dot(xr, yr)) > 0.02f * lx * ly)
                    continue;
                int in = 0;
                for (int e = 0; e < ne; e++) {
                    float ex, ey, ew;
                    clip(E[e], ex, ey, ew);
                    in += ew > 1.f && std::fabs(ex) < 1.2f && std::fabs(ey) < 1.2f;
                }
                if (ne && in == 0) continue;
                g_clips.push_back(q);
                fprintf(log, "  clip? %08x kratos=(%.2f %.2f w%.0f) inimigos na tela %d/%d\n   ", q, sx, sy, w, in, ne);
                for (int i = 0; i < 16; i++) fprintf(log, " %.4g", m[i]);
                fprintf(log, "\n");
            }
        a = end;
    }
    for (uint32_t c : g_clips)
        for (uint32_t a = 0x81200000u; a < 0x81800000u; a += 4) {
            if ((a & 0xFFFF) == 0 && !readable(a, 0x10000)) {
                a += 0xFFFC;
                continue;
            }
            const uint32_t v = rd32(a);
            if (v <= c && c - v < 0x1000) fprintf(log, "  ref %08x -> %08x (clip %08x, +%x)\n", a, v, c, c - v);
        }
}

// câmera guardada como matriz de mundo (eixos + posição, a inversa da vista): eixos ortonormais, a posição a uma
// distância razoável do Kratos e, com a projeção dos shaders (fx, fy), o Kratos perto do centro da tela.
// Grava os candidatos e quem aponta pra eles
void scan_camworld(FILE *log, const float *P, float fx, float fy) {
    int found = 0;
    std::vector<uint32_t> hits;
    for (uint32_t a = 0x81000000u; a < 0x90000000u && found < 60;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (!VirtualQuery(mem_base() + a, &mi, sizeof mi)) break;
        const uint32_t end =
            (uint32_t)std::min<uint64_t>(0x90000000ull, (uint64_t)((uint8_t *)mi.BaseAddress - mem_base()) + mi.RegionSize);
        if (mi.State == MEM_COMMIT && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
            for (uint32_t q = (a + 15) & ~15u; q + 64 <= end && found < 60; q += 16) {
                const float *m = ptr<float>(q);
                for (int lay = 0; lay < 2; lay++) {
                    // eixo k: lay 0 = colunas m[4k..], posição m[12..]; lay 1 = linhas, posição m[3], m[7], m[11]
                    auto A = [&](int k, int i) { return lay == 0 ? m[4 * k + i] : m[4 * i + k]; };
                    const float t[3] = {lay == 0 ? m[12] : m[3], lay == 0 ? m[13] : m[7], lay == 0 ? m[14] : m[11]};
                    bool ok = true;
                    for (int i = 0; i < 3 && ok; i++)
                        for (int j = i; j < 3 && ok; j++) {
                            const float dp = A(i, 0) * A(j, 0) + A(i, 1) * A(j, 1) + A(i, 2) * A(j, 2);
                            ok = std::fabs(dp - (i == j ? 1.f : 0.f)) < 2e-3f;
                        }
                    if (!ok) continue;
                    const float d[3] = {P[0] - t[0], P[1] - t[1], P[2] - t[2]};
                    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (!(dist > 80.f && dist < 5000.f)) continue;
                    const float vx = d[0] * A(0, 0) + d[1] * A(0, 1) + d[2] * A(0, 2);
                    const float vy = d[0] * A(1, 0) + d[1] * A(1, 1) + d[2] * A(1, 2);
                    const float vz = d[0] * A(2, 0) + d[1] * A(2, 1) + d[2] * A(2, 2);
                    if (vz > -50.f) continue; // a câmera olha para -z
                    const float sx = fx * vx / -vz, sy = fy * vy / -vz;
                    if (std::fabs(sx) > 0.7f || std::fabs(sy) > 0.9f) continue;
                    found++;
                    hits.push_back(q);
                    fprintf(log, "  camw %08x lay%d dist=%.0f tela=(%.2f %.2f) pos=(%.0f %.0f %.0f)\n", q, lay, dist, sx, sy, t[0],
                            t[1], t[2]);
                }
            }
        a = end;
    }
    for (uint32_t c : hits)
        for (uint32_t a = 0x81200000u; a < 0x81800000u; a += 4) {
            if ((a & 0xFFFF) == 0 && !readable(a, 0x10000)) {
                a += 0xFFFC;
                continue;
            }
            const uint32_t v = rd32(a);
            if (v <= c && c - v < 0x1000) fprintf(log, "  wref %08x -> %08x (camw %08x, +%x)\n", a, v, c, c - v);
        }
}

// GoW1: tabela 0x812a6f48, Kratos [[0x812a6fa4]+4] com a posição em +0x38 (casados com o GoW2 pelo código e
// medidos). Lista as classes da tabela, grava os bytes dos objetos que não são o Kratos (pra achar a vida) e procura
// a câmera a partir da posição do Kratos, com quem no segmento de dados aponta pra perto dela
void diag_gow1(uint32_t frame) {
    static FILE *log = fopen("enemy_log_gow1.txt", "w");
    static FILE *bin = fopen("enemy_dump_gow1_e.bin", "wb");
    if (!log) return;
    const uint32_t table = rd32(0x812a6f48u), kp = rd32(0x812a6fa4u);
    const uint32_t kratos = kp && readable(kp + 4, 4) ? rd32(kp + 4) : 0;
    fprintf(log, "q%u tabela=%08x kratos=%08x estado=%08x\n", frame, table, kratos, rd32(0x812c2e04u));
    if (!kratos || !readable(kratos, 0x800)) return fflush(log), void();
    fprintf(log, "  kratos pos=(%.0f %.0f %.0f)\n", rdf(kratos + 0x38), rdf(kratos + 0x3c), rdf(kratos + 0x40));
    uint32_t enemies[64];
    int ne = 0;
    std::map<uint32_t, int> classes;
    if (table && readable(table, 8))
        for (uint32_t k = 0; k < 1024; k++) {
            const uint32_t e = table + 8 + 12 * k;
            if ((k & 63) == 0 && !readable(e, 12 * 64)) break;
            const uint32_t h = rd32(e), obj = rd32(e + 8);
            if (!obj || (h >> 23) != k || obj == kratos || !readable(obj, 0x800)) continue;
            const uint32_t vt = rd32(obj);
            if (vt < 0x81000000u || vt > 0x81400000u) continue;
            classes[vt]++;
            const float x = rdf(obj + 0x38), y = rdf(obj + 0x3c), z = rdf(obj + 0x40);
            const float dx = x - rdf(kratos + 0x38), dz = z - rdf(kratos + 0x40);
            if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) || dx * dx + dz * dz > 1500.f * 1500.f) continue;
            if (ne < 64) enemies[ne++] = obj;
            if (bin) {
                fwrite(&frame, 4, 1, bin), fwrite(&obj, 4, 1, bin), fwrite(&vt, 4, 1, bin);
                fwrite(ptr<uint8_t>(obj), 1, 0x800, bin);
            }
        }
    fprintf(log, "  classes:");
    for (auto &c : classes) fprintf(log, " %08x x%d", c.first, c.second);
    fprintf(log, "\n  perto do Kratos: %d\n", ne);
    if (frame % 600 == 0) { // posição viva em +0x78 (medida)
        const float P[3] = {rdf(kratos + 0x78), rdf(kratos + 0x7c) + 40.f, rdf(kratos + 0x80)};
        float E[64][3];
        for (int e = 0; e < ne; e++) E[e][0] = rdf(enemies[e] + 0x78), E[e][1] = rdf(enemies[e] + 0x7c), E[e][2] = rdf(enemies[e] + 0x80);
        fprintf(log, "  estado %08x kratos78=(%.0f %.0f %.0f)\n", rd32(0x812c2e04u), P[0], P[1], P[2]);
        scan_clip(log, P, E, ne);
    }
    if (frame % 120 == 60) { // g_ViewProjection dos shaders: projeta o Kratos e os inimigos com ela
        float vp[16];
        uint32_t age = 0;
        if (last_view_projection(vp, age)) {
            auto proj = [&](uint32_t o, float &sx, float &sy, float &w) {
                const float X = rdf(o + 0x78), Y = rdf(o + 0x7c), Z = rdf(o + 0x80);
                for (int lay = 0; lay < 1; lay++) {
                    w = vp[3] * X + vp[7] * Y + vp[11] * Z + vp[15];
                    sx = (vp[0] * X + vp[4] * Y + vp[8] * Z + vp[12]) / w;
                    sy = (vp[1] * X + vp[5] * Y + vp[9] * Z + vp[13]) / w;
                }
            };
            float sx, sy, w;
            proj(kratos, sx, sy, w);
            fprintf(log, "  vp idade %u kratos=(%.2f %.2f w%.0f)", age, sx, sy, w);
            for (int e = 0; e < ne && e < 8; e++) {
                proj(enemies[e], sx, sy, w);
                fprintf(log, " e(%.2f %.2f w%.0f)", sx, sy, w);
            }
            fprintf(log, "\n   ");
            for (int i = 0; i < 16; i++) fprintf(log, " %.4g", vp[i]);
            fprintf(log, "\n");
        } else {
            fprintf(log, "  vp: nenhuma g_ViewProjection ainda\n");
        }
    }
    if (frame % 120 == 30) { // g_Transforms[0] mais repetidos como vista, com a projeção dos shaders
        float pj[16], tf[64][16];
        int cnt[64];
        const int n = frame_transforms(tf, cnt, 64);
        if (last_projection(pj) && n) {
            const float X = rdf(kratos + 0x78), Y = rdf(kratos + 0x7c) + 30.f, Z = rdf(kratos + 0x80);
            fprintf(log, "  tf: %d matrizes, kratos=(%.0f %.0f %.0f) fx=%.3f fy=%.3f\n", n, X, Y, Z, pj[0], pj[5]);
            for (int i = 0; i < n; i++) {
                if (cnt[i] < 3) continue;
                const float *m = tf[i];
                for (int lay = 0; lay < 2; lay++) {
                    float v[3];
                    for (int r = 0; r < 3; r++)
                        v[r] = lay == 0 ? m[r] * X + m[4 + r] * Y + m[8 + r] * Z + m[12 + r]   // colunas
                                        : m[4 * r] * X + m[4 * r + 1] * Y + m[4 * r + 2] * Z + m[4 * r + 3]; // linhas 3x4
                    if (v[2] > -10.f) continue;
                    const float sx = pj[0] * v[0] / -v[2], sy = pj[5] * v[1] / -v[2];
                    if (std::fabs(sx) > 1.f || std::fabs(sy) > 1.f) continue;
                    fprintf(log, "   tf#%d x%d lay%d kratos tela=(%.2f %.2f) z=%.0f :", i, cnt[i], lay, sx, sy, v[2]);
                    for (int k = 0; k < 16; k++) fprintf(log, " %.4g", m[k]);
                    fprintf(log, "\n");
                }
            }
        }
    }
    if (frame % 600 == 300) { // câmera como matriz de mundo, testada com a projeção dos shaders
        float pj[16];
        if (last_projection(pj)) {
            const float P[3] = {rdf(kratos + 0x78), rdf(kratos + 0x7c) + 30.f, rdf(kratos + 0x80)};
            fprintf(log, "  camworld: kratos=(%.0f %.0f %.0f) fx=%.3f fy=%.3f\n", P[0], P[1], P[2], pj[0], pj[5]);
            scan_camworld(log, P, pj[0], pj[5]);
        }
    }
    if (false) { // vista na memória do jogo + projeção dos shaders
        scan_camera(log, kratos, enemies, ne, 0x78);
        float pj[16];
        if (last_projection(pj)) {
            fprintf(log, "  proj:");
            for (int i = 0; i < 16; i++) fprintf(log, " %.4g", pj[i]);
            fprintf(log, "\n");
        }
        for (uint32_t cam : g_cams) {
            const float *m = ptr<float>(cam);
            fprintf(log, "  cam %08x:", cam);
            for (int i = 0; i < 16; i++) fprintf(log, " %.4g", m[i]);
            fprintf(log, "\n");
            for (uint32_t a = 0x81200000u; a < 0x81800000u; a += 4) {
                if ((a & 0xFFFF) == 0 && !readable(a, 0x10000)) {
                    a += 0xFFFC;
                    continue;
                }
                const uint32_t v = rd32(a);
                if (v <= cam && cam - v < 0x1000) fprintf(log, "  vref %08x -> %08x (cam %08x, +%x)\n", a, v, cam, cam - v);
            }
        }
    }
    if (false) {
        scan_camera(log, kratos, enemies, ne, 0x38);
        for (uint32_t cam : g_cams)
            for (uint32_t a = 0x81200000u; a < 0x81800000u; a += 4) {
                if ((a & 0xFFFF) == 0 && !readable(a, 0x10000)) {
                    a += 0xFFFC;
                    continue;
                }
                const uint32_t v = rd32(a);
                if (v <= cam && cam - v < 0x1000) fprintf(log, "  ref %08x -> %08x (cam %08x, +%x)\n", a, v, cam, cam - v);
            }
    }
    if (bin) fflush(bin);
    fflush(log);
}

} // namespace

void enemy_hp_tick(int game) {
    static const bool diag = getenv("VITART_ENEMYLOG") != nullptr;
    static uint32_t frame = 0;
    frame++;
    if (diag && game == 2 && frame % 60 == 0) diag_gow2(frame);
    if (diag && game == 1 && frame % 60 == 0) diag_gow1(frame);
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
        bars[n++] = {0.5f + 0.5f * cx / cw, 0.5f + 0.5f * cy / cw, hp / mx, (int)std::ceil(hp)};
    }
    if (n) gvk::draw_enemy_bars(fb, bars, n);
}

} // namespace vt
