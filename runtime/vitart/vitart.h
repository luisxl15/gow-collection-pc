// vitart: runtime próprio para executar módulos do PS Vita recompilados pelo vitarecomp, sem emulador.
//
//   * memória do guest: janela plana de 4 GiB, só reservada; páginas confirmadas sob demanda dentro
//     de regiões registradas (fora delas o acesso é erro, com o endereço do guest no log);
//   * módulos: uma DLL por módulo (gerada por scripts/build-gow-dlls.sh), cada um na base do plano;
//   * imports: ligados a exports de outros módulos carregados ou a implementações HLE por nome;
//   * threads do guest = threads do host, cada uma com seu arc_ctx, pilha e TLS no guest.
#pragma once

#include <atomic>
#include <condition_variable>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include "arc_abi.h"
}

namespace vt {

// ---- log ----
void logf(const char *fmt, ...);
[[noreturn]] void fatal(const char *fmt, ...);

// ---- memória do guest ----
uint8_t *mem_base();
template <class T>
inline T *ptr(uint32_t a) { return reinterpret_cast<T *>(mem_base() + a); }
inline uint32_t rd32(uint32_t a) { uint32_t v; std::memcpy(&v, mem_base() + a, 4); return v; }
inline void wr32(uint32_t a, uint32_t v) { std::memcpy(mem_base() + a, &v, 4); }
inline void wr64(uint32_t a, uint64_t v) { std::memcpy(mem_base() + a, &v, 8); }
inline const char *gstr(uint32_t a) { return a ? reinterpret_cast<const char *>(mem_base() + a) : ""; }
void map_region(uint32_t addr, uint32_t size, const char *what);
bool region_of(uint32_t addr, uint32_t *start, uint32_t *size, std::string *what);
// Alocadores simples por faixa (sem devolução por enquanto: o objetivo é medir o que o jogo pede).
enum class Pool { kernel, user, cdram };
uint32_t galloc(Pool pool, uint32_t size, uint32_t align, const char *what);

// ---- módulos e código ----
arc_func lookup_func(uint32_t key);
const arc_module_info *main_module();

// ---- threads ----
struct Thread {
    int id = 0;
    std::string name;
    uint32_t entry = 0;
    int priority = 0;
    uint32_t stack = 0, stack_size = 0, tls = 0;
    arc_ctx ctx{};
    std::atomic<int> state{ 0 }; // 0 criada, 1 rodando, 2 terminada
    int exit_status = 0;
    std::mutex m;
    std::condition_variable cv;
    std::jmp_buf exit_jb;
    bool can_exit = false;
};
Thread *current();
Thread *create_thread(const char *name, uint32_t entry, int priority, uint32_t stack_size);
Thread *find_thread(int id);
int start_thread(Thread *t, uint32_t arglen, uint32_t argp);
[[noreturn]] void exit_current_thread(int status);
int wait_thread_end(Thread *t, uint32_t timeout_us, int *status);

// Chama código do guest no contexto da thread atual: r0-r3 = args, devolve r0.
uint32_t call_guest(arc_ctx *ctx, uint32_t addr, std::initializer_list<uint32_t> args);
// Chama `addr` com os registradores já preparados em ctx (r[15] recebe o retorno real).
void call_guest_raw(arc_ctx *ctx, uint32_t addr);

// ---- HLE ----
using HleFn = void (*)(arc_ctx *ctx);
struct HleReg {
    HleReg(const char *name, HleFn fn);
};
#define VT_HLE(name) \
    static void hle_##name(arc_ctx *ctx); \
    static vt::HleReg reg_##name(#name, hle_##name); \
    static void hle_##name(arc_ctx *ctx)

// Argumento n (AAPCS): r0-r3, depois a pilha.
inline uint32_t arg(arc_ctx *ctx, int n) { return n < 4 ? ctx->r[n] : rd32(ctx->r[13] + 4u * (n - 4)); }
inline void ret(arc_ctx *ctx, uint32_t v) { ctx->r[0] = v; }
inline void ret64(arc_ctx *ctx, uint64_t v) { ctx->r[0] = (uint32_t)v; ctx->r[1] = (uint32_t)(v >> 32); }

// Contadores para o relatório de saída.
struct Stats {
    std::atomic<uint64_t> hle_calls{ 0 }, export_calls{ 0 }, missing_calls{ 0 }, dispatch_cross{ 0 };
};
Stats &stats();

std::string app_path(); // diretório do app (app0:)
std::string data_path(); // raiz para ux0:data e afins

} // namespace vt
