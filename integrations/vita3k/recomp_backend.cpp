// Backend de código recompilado para o Vita3K (fase 3 do vita-recomp-lab).
//
// Fronteira:
//   * o código gerado (DLL) recebe um arc_ctx e a memória plana do guest (MemState::memory);
//   * imports: o código gerado chama o stub pelo endereço; se o stub é HLE (svc #0; mov pc, lr; NID)
//     chamamos KernelState::call_import, se o carregador o religou a um export LLE
//     (movw ip; movt ip; bx ip) seguimos para o alvo, nativo se recompilado, senão no JIT;
//   * desvio para código sem tradução: o JIT executa num run_loop aninhado até voltar ao halt;
//   * sair da pilha C (saída de thread, retorno para outro lugar): longjmp para o quadro do
//     recomp_try_run mais interno desta thread, que entrega o PC certo ao run_loop.
#include "recomp_backend.h"

#include <cpu/functions.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/ptr.h>
#include <mem/state.h>
#include <util/log.h>

extern "C" {
#include "arc_host.h"
}

#include <atomic>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

struct RecompAccess {
    static KernelState &kernel(ThreadState &t) { return t.kernel; }
    static MemState &mem(ThreadState &t) { return t.mem; }
    static int call_level(ThreadState &t) {
        const std::lock_guard<std::mutex> lock(t.mutex);
        return t.call_level;
    }
    static void enter_call(ThreadState &t) {
        const std::lock_guard<std::mutex> lock(t.mutex);
        t.call_level++;
    }
    static bool must_unwind(ThreadState &t, int level) {
        const std::lock_guard<std::mutex> lock(t.mutex);
        return t.to_do == ThreadToDo::remove || t.call_level != level;
    }
};

namespace {

struct Frame {
    ThreadState *thread;
    int level;
    std::jmp_buf jb;
};

thread_local std::vector<Frame *> frames;

struct Backend {
    std::once_flag once;
    bool enabled = false;
    bool verified = false;
    bool log_fallbacks = false;
    std::unordered_map<uint32_t, arc_func> table;
    const arc_segment *segs = nullptr;
    unsigned seg_count = 0;
    std::mutex log_mutex;
    std::unordered_set<uint32_t> fallback_seen;
    std::atomic<uint64_t> native_entries{ 0 }, imports_hle{ 0 }, imports_lle{ 0 }, jit_calls{ 0 }, unwinds{ 0 };
    uint8_t *guest_mem = nullptr;
};
Backend g;

void ctx_from_cpu(CPUState &cpu, arc_ctx &c) {
    const CPUContext x = save_context(cpu);
    for (int i = 0; i < 16; i++)
        c.r[i] = x.cpu_registers[i];
    static_assert(sizeof(c.d) == sizeof(x.fpu_registers));
    std::memcpy(c.d, x.fpu_registers.data(), sizeof(c.d));
    c.n = (x.cpsr >> 31) & 1;
    c.z = (x.cpsr >> 30) & 1;
    c.c = (x.cpsr >> 29) & 1;
    c.v = (x.cpsr >> 28) & 1;
    c.apsr_rest = x.cpsr & 0x0FFFFFFFu;
    c.fpscr = x.fpscr;
    c.tpidruro = read_tpidruro(cpu);
}

void ctx_to_cpu(const arc_ctx &c, CPUState &cpu) {
    CPUContext x;
    for (int i = 0; i < 16; i++)
        x.cpu_registers[i] = c.r[i];
    std::memcpy(x.fpu_registers.data(), c.d, sizeof(c.d));
    x.cpsr = (c.n << 31) | (c.z << 30) | (c.c << 29) | (c.v << 28) | (c.apsr_rest & 0x0FFFFFFFu);
    x.fpscr = c.fpscr;
    load_context(cpu, x);
    write_tpidruro(cpu, c.tpidruro);
}

Frame &top() {
    if (frames.empty()) {
        LOG_CRITICAL("recomp: serviço chamado fora de um quadro nativo");
        std::abort();
    }
    return *frames.back();
}

[[noreturn]] void unwind(Frame &f) {
    g.unwinds++;
    std::longjmp(f.jb, 1);
}

arc_func lookup(uint32_t key) {
    const auto it = g.table.find(key);
    return it == g.table.end() ? nullptr : it->second;
}

// Executa `target` no JIT até ele voltar (LR = halt), como run_callback, mas preservando os
// registradores de retorno. Na volta, r[15] = endereço de retorno que o chamador pôs em LR.
void jit_call(arc_ctx *ctx, uint32_t target) {
    Frame &f = top();
    ThreadState &t = *f.thread;
    CPUState &cpu = *t.cpu;
    KernelState &kernel = RecompAccess::kernel(t);
    const uint32_t ret = ctx->r[14];
    g.jit_calls++;
    if (g.log_fallbacks) {
        const std::lock_guard<std::mutex> lock(g.log_mutex);
        if (g.fallback_seen.insert(target).second)
            LOG_INFO("recomp: JIT para 0x{:08X} (retorno 0x{:08X})", target, ret);
    }
    ctx_to_cpu(*ctx, cpu);
    write_pc(cpu, target);
    write_lr(cpu, kernel.halt_instruction_pc);
    RecompAccess::enter_call(t);
    t.run_loop();
    ctx_from_cpu(cpu, *ctx);
    ctx->excl_valid = 0;
    ctx->r[14] = ret;
    ctx->r[15] = ret;
    if (RecompAccess::must_unwind(t, f.level))
        unwind(f);
}

void call_guest(arc_ctx *ctx, uint32_t target) {
    if (const arc_func fn = lookup(target)) {
        fn(ctx);
        return;
    }
    jit_call(ctx, target);
}

// ---- serviços entregues à DLL ----

void host_import(arc_ctx *ctx, uint32_t nid, uint32_t stub) {
    Frame &f = top();
    ThreadState &t = *f.thread;
    CPUState &cpu = *t.cpu;
    MemState &mem = RecompAccess::mem(t);
    const uint32_t *s = Ptr<uint32_t>(stub).get(mem);
    if (s[0] == 0xEF000000u) { // svc #0; mov pc, lr; NID
        g.imports_hle++;
        ctx_to_cpu(*ctx, cpu);
        write_pc(cpu, stub + 4); // como depois do svc em modo ARM
        RecompAccess::kernel(t).call_import(cpu, s[2], t.id);
        clear_exclusive(cpu);
        const uint32_t pc_after = read_pc(cpu);
        ctx_from_cpu(cpu, *ctx);
        ctx->excl_valid = 0;
        if (RecompAccess::must_unwind(t, f.level))
            unwind(f);
        if (pc_after != stub + 4) { // o HLE redirecionou o fluxo
            ctx->r[15] = pc_after | ((ctx->apsr_rest & 0x20) ? 1u : 0u);
            unwind(f);
        }
        return;
    }
    // movw ip, #lo; movt ip, #hi; bx ip (ARM): import resolvido para um export LLE.
    const auto imm16 = [](uint32_t w) { return ((w >> 4) & 0xF000u) | (w & 0x0FFFu); };
    if ((s[0] & 0x0FF0F000u) == 0x0300C000u && (s[1] & 0x0FF0F000u) == 0x0340C000u) {
        g.imports_lle++;
        call_guest(ctx, imm16(s[0]) | (imm16(s[1]) << 16));
        return;
    }
    LOG_CRITICAL("recomp: stub de import desconhecido em 0x{:08X} (NID 0x{:08X}): {:08X} {:08X} {:08X}", stub, nid, s[0], s[1], s[2]);
    std::abort();
}

void host_dispatch_miss(arc_ctx *ctx, uint32_t target, uint32_t from) {
    (void)from;
    jit_call(ctx, target);
}

void host_ret_mismatch(arc_ctx *ctx, uint32_t expected) {
    (void)ctx, (void)expected;
    unwind(top()); // recomp_try_run continua em ctx->r[15]
}

void host_svc(arc_ctx *ctx, uint32_t imm, uint32_t pc) {
    (void)ctx;
    LOG_CRITICAL("recomp: svc #{} fora de stub de import em 0x{:08X}", imm, pc);
    std::abort();
}

void host_fatal(arc_ctx *ctx, uint32_t pc, const char *why) {
    (void)ctx;
    LOG_CRITICAL("recomp: erro fatal no código gerado em 0x{:08X}: {}", pc, why ? why : "?");
    std::abort();
}

void host_log(const char *msg) {
    std::string s(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    LOG_INFO("recomp: {}", s);
}

const arc_host_api api = { ARC_HOST_ABI, host_import, host_dispatch_miss, host_ret_mismatch, host_svc, host_fatal, host_log };

void *load_symbol(void *lib, const char *name) {
#ifdef _WIN32
    return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

void init(MemState &mem) {
    const char *path = std::getenv("VITA3K_RECOMP_LIB");
    const char *disable = std::getenv("VITA3K_RECOMP_DISABLE");
    g.log_fallbacks = std::getenv("VITA3K_RECOMP_LOG") != nullptr;
    if (!path || !*path || (disable && *disable == '1'))
        return;
    if (mem.use_page_table) {
        LOG_WARN("recomp: memória com tabela de páginas (host de páginas > 4 KiB): backend desligado");
        return;
    }
#ifdef _WIN32
    void *lib = LoadLibraryA(path);
#else
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!lib) {
        LOG_ERROR("recomp: não foi possível carregar {}", path);
        return;
    }
    const auto bind = reinterpret_cast<int (*)(const arc_host_api *, uint8_t *)>(load_symbol(lib, "arc_bind"));
    const auto get_table = reinterpret_cast<const arc_func_entry *(*)(unsigned *)>(load_symbol(lib, "arc_get_table"));
    const auto get_segs = reinterpret_cast<const arc_segment *(*)(unsigned *)>(load_symbol(lib, "arc_get_segments"));
    if (!bind || !get_table || !get_segs) {
        LOG_ERROR("recomp: {} não exporta arc_bind/arc_get_table/arc_get_segments", path);
        return;
    }
    g.guest_mem = mem.memory.get();
    if (bind(&api, g.guest_mem) != 0) {
        LOG_ERROR("recomp: ABI de {} incompatível", path);
        return;
    }
    unsigned n = 0;
    const arc_func_entry *t = get_table(&n);
    g.table.reserve(n);
    for (unsigned i = 0; i < n; i++)
        g.table.emplace(t[i].key, t[i].fn);
    g.segs = get_segs(&g.seg_count);
    g.enabled = true;
    LOG_INFO("recomp: {} carregado, {} funções recompiladas", path, n);
}

// A DLL só vale para a imagem exata que foi traduzida, no endereço de link. Na primeira vez que
// uma entrada bate, compara os segmentos de código com a memória do guest.
bool verify(MemState &mem) {
    for (unsigned i = 0; i < g.seg_count; i++) {
        const arc_segment &s = g.segs[i];
        if (!is_valid_addr_range(mem, s.vaddr, s.vaddr + s.filesz)) {
            LOG_ERROR("recomp: segmento 0x{:08X} da DLL não está mapeado no guest", s.vaddr);
            return false;
        }
    }
    // Só o primeiro segmento (código, RX) é imutável depois de carregado e relocado.
    const arc_segment &code = g.segs[0];
    if (std::memcmp(g.guest_mem + code.vaddr, code.data, code.filesz) != 0) {
        LOG_ERROR("recomp: o código carregado em 0x{:08X} difere do traduzido: backend desligado", code.vaddr);
        return false;
    }
    LOG_INFO("recomp: imagem conferida (0x{:X} bytes de código em 0x{:08X})", code.filesz, code.vaddr);
    return true;
}

} // namespace

bool recomp_try_run(ThreadState &thread) {
    MemState &mem = RecompAccess::mem(thread);
    std::call_once(g.once, [&] { init(mem); });
    if (!g.enabled)
        return false;
    CPUState &cpu = *thread.cpu;
    const uint32_t key = read_pc(cpu) | (is_thumb_mode(cpu) ? 1u : 0u);
    const arc_func fn = lookup(key);
    if (!fn)
        return false;
    if (!g.verified) {
        static std::mutex vm;
        const std::lock_guard<std::mutex> lock(vm);
        if (!g.verified) {
            if (!verify(mem)) {
                g.enabled = false;
                return false;
            }
            g.verified = true;
        }
    }
    g.native_entries++;
    arc_ctx ctx{};
    ctx_from_cpu(cpu, ctx);
    Frame f;
    f.thread = &thread;
    f.level = RecompAccess::call_level(thread);
    frames.push_back(&f);
    if (setjmp(f.jb) == 0)
        fn(&ctx);
    frames.pop_back();
    ctx_to_cpu(ctx, cpu);
    write_pc(cpu, ctx.r[15]);
    return true;
}
