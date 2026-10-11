// Núcleo do vitart: memória do guest, carga dos módulos recompilados, ligação de imports, threads
// e a sequência de partida do Vita (module_start das bibliotecas, depois a thread principal).
#include "vitart.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

extern "C" {
#include "arc_host.h"
}

namespace vt {

// ======================================================================= log
static std::mutex g_log_mutex;
static FILE *g_logfile;

void logf(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    const std::lock_guard<std::mutex> lock(g_log_mutex);
    fputs(buf, stderr);
    fputc('\n', stderr);
    if (g_logfile) {
        fputs(buf, g_logfile);
        fputc('\n', g_logfile);
        fflush(g_logfile);
    }
}

void fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    logf("vitart: ERRO FATAL: %s", buf);
    if (Thread *t = current())
        logf("  thread %d (%s) pc~ r15=%08x lr=%08x sp=%08x", t->id, t->name.c_str(), t->ctx.r[15], t->ctx.r[14], t->ctx.r[13]);
    fflush(stderr);
    _exit(3);
}

Stats &stats() {
    static Stats s;
    return s;
}

// ================================================================== memória
static uint8_t *g_mem;
struct Region {
    uint32_t start, size;
    std::string what;
};
static std::mutex g_regions_mutex;
static std::map<uint32_t, Region> g_regions; // por início

uint8_t *mem_base() { return g_mem; }

void map_region(uint32_t addr, uint32_t size, const char *what) {
    const std::lock_guard<std::mutex> lock(g_regions_mutex);
    g_regions[addr] = Region{ addr, size, what };
}

bool region_of(uint32_t addr, uint32_t *start, uint32_t *size, std::string *what) {
    const std::lock_guard<std::mutex> lock(g_regions_mutex);
    auto it = g_regions.upper_bound(addr);
    if (it == g_regions.begin())
        return false;
    --it;
    if (addr - it->second.start >= it->second.size)
        return false;
    if (start) *start = it->second.start;
    if (size) *size = it->second.size;
    if (what) *what = it->second.what;
    return true;
}

// Função do guest que contém um endereço de código do host (para diagnóstico de falhas).
static std::string describe_host_pc(const void *pc);
static void dump_thread_regs();

void commit_range(uint32_t addr, uint32_t size) {
    if (!size) return;
    const uint64_t first = (uint64_t)addr & ~0xFFFFull, last = ((uint64_t)addr + size - 1) & ~0xFFFFull;
    for (uint64_t page = first; page <= last; page += 0x10000)
        if (region_of((uint32_t)page, nullptr, nullptr, nullptr) || region_of((uint32_t)(page + 0xFFFF), nullptr, nullptr, nullptr))
            VirtualAlloc(g_mem + page, 0x10000, MEM_COMMIT, PAGE_READWRITE);
}

void dump_regions(const std::string &dir, uint32_t lo, uint32_t hi) {
    std::vector<Region> rs;
    {
        const std::lock_guard<std::mutex> lock(g_regions_mutex);
        for (auto &[st, r] : g_regions)
            if (st >= lo && st < hi)
                rs.push_back(r);
    }
    std::ofstream idx(dir + "/regions.txt");
    for (const Region &r : rs) {
        char name[64];
        std::snprintf(name, sizeof name, "/mem_%08x.bin", r.start);
        std::ofstream f(dir + name, std::ios::binary);
        // Só as páginas já confirmadas: as demais são zero e não precisam existir no arquivo.
        for (uint32_t off = 0; off < r.size;) {
            MEMORY_BASIC_INFORMATION mbi{};
            VirtualQuery(g_mem + r.start + off, &mbi, sizeof mbi);
            const uint8_t *end = static_cast<const uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
            const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(end - (g_mem + r.start + off), r.size - off));
            if (mbi.State == MEM_COMMIT)
                f.write(reinterpret_cast<const char *>(g_mem + r.start + off), n);
            else {
                f.seekp(n, std::ios::cur);
                if (off + n == r.size) { f.seekp(-1, std::ios::cur); f.put(0); }
            }
            off += n;
        }
        idx << std::hex << r.start << " " << r.size << " " << r.what << '\n';
    }
}

// Watchpoint de escrita (diagnóstico): VITART_WATCH=endereço,tamanho. A página fica só-leitura;
// cada escrita na faixa registra a função do guest e o valor anterior; o passo único religa a proteção.
static uint32_t g_watch_addr = 0, g_watch_len = 0;
static thread_local bool t_watch_rearm = false;
static thread_local uint32_t t_watch_logged = 0; // endereço cuja escrita foi registrada (valor novo no passo único)
static unsigned g_watch_max = 3;
static std::mutex g_watch_mutex;
static std::map<std::string, unsigned> g_watch_hits;

static void watch_protect(bool on) {
    DWORD old;
    VirtualProtect(g_mem + (g_watch_addr & ~0xFFFu), ((g_watch_addr & 0xFFFu) + g_watch_len + 0xFFFu) & ~0xFFFu,
        on ? PAGE_READONLY : PAGE_READWRITE, &old);
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode == EXCEPTION_SINGLE_STEP && t_watch_rearm) {
        t_watch_rearm = false;
        if (t_watch_logged) {
            logf("vitart:   novo valor em 0x%08x: %08x", t_watch_logged, rd32(t_watch_logged & ~3u));
            t_watch_logged = 0;
        }
        watch_protect(true);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_watch_len && er->ExceptionInformation[0] == 1) {
        const uint64_t ha = er->ExceptionInformation[1];
        const uint64_t lo = (uint64_t)(g_mem + (g_watch_addr & ~0xFFFu));
        const uint64_t hi = (uint64_t)(g_mem + g_watch_addr + g_watch_len + 0xFFF) & ~0xFFFull;
        if (ha >= lo && ha < hi) {
            const uint32_t ga = (uint32_t)(ha - (uint64_t)g_mem);
            if (ga >= g_watch_addr && ga < g_watch_addr + g_watch_len) {
                const std::string where = describe_host_pc(er->ExceptionAddress);
                const std::lock_guard<std::mutex> lock(g_watch_mutex);
                if (g_watch_hits[where]++ < g_watch_max) {
                    t_watch_logged = ga;
                    logf("vitart: escrita vigiada em 0x%08x (+%u) por %s; valor anterior %08x", ga, ga - g_watch_addr,
                        where.c_str(), rd32(ga & ~3u));
                    if (Thread *t = current())
                        logf("vitart:   guest r0=%08x r1=%08x r2=%08x r3=%08x sp=%08x lr=%08x", t->ctx.r[0], t->ctx.r[1],
                            t->ctx.r[2], t->ctx.r[3], t->ctx.r[13], t->ctx.r[14]);
                }
            }
            watch_protect(false);
            t_watch_rearm = true;
            ep->ContextRecord->EFlags |= 0x100; // passo único
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    uint8_t *host = reinterpret_cast<uint8_t *>(er->ExceptionInformation[1]);
    if (!g_mem || host < g_mem || host >= g_mem + (1ull << 32))
        return EXCEPTION_CONTINUE_SEARCH;
    const uint32_t ga = static_cast<uint32_t>(host - g_mem);
    if (!region_of(ga, nullptr, nullptr, nullptr)) {
        logf("vitart: acesso a endereço do guest não mapeado 0x%08x (%s, rip host %p)", ga,
            er->ExceptionInformation[0] == 1 ? "escrita" : "leitura", er->ExceptionAddress);
        logf("    em %s", describe_host_pc(er->ExceptionAddress).c_str());
        dump_thread_regs();
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uint64_t page = (uint64_t)ga & ~0xFFFFull;
    if (!VirtualAlloc(g_mem + page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    if (g_watch_len && g_watch_addr + g_watch_len > page && g_watch_addr < page + 0x10000)
        watch_protect(true); // a confirmação sob demanda devolveu a página vigiada como gravável
    return EXCEPTION_CONTINUE_EXECUTION;
}

static void mem_init() {
    g_mem = static_cast<uint8_t *>(VirtualAlloc(nullptr, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS));
    if (!g_mem)
        fatal("não foi possível reservar 4 GiB de espaço de endereços para o guest");
    AddVectoredExceptionHandler(1, on_fault);
}

// Faixas dos alocadores. Todas com o bit 30 zerado, como a memória do usuário no Vita
// (0x8xxxxxxx): o God of War guarda uma flag no bit 30 de ponteiros e a apaga ao usá-los.
// Nenhum módulo do plano cai nelas (gow1 0x81000000, libc 0x82000000, libfios2 0x82100000).
struct Bump {
    uint32_t cur, end;
    std::mutex m;
};
static Bump g_pools[3] = { { 0xB0000000u, 0xC0000000u, {} }, { 0x83000000u, 0xA0000000u, {} }, { 0xA0000000u, 0xB0000000u, {} } };

static const uint32_t g_pool_start[3] = { 0xB0000000u, 0x83000000u, 0xA0000000u };

uint32_t galloc(Pool pool, uint32_t size, uint32_t align, const char *what) {
    Bump &b = g_pools[static_cast<int>(pool)];
    if (align < 16) align = 16;
    uint32_t a;
    {
        const std::lock_guard<std::mutex> lock(b.m);
        a = (b.cur + align - 1) & ~(align - 1);
        if ((uint64_t)a + size > b.end) {
            // Diagnóstico: quem ocupa a faixa esgotada.
            std::map<std::string, std::pair<unsigned, uint64_t>> by_what;
            {
                const std::lock_guard<std::mutex> rl(g_regions_mutex);
                for (auto &[st, r] : g_regions)
                    if (st >= g_pool_start[static_cast<int>(pool)] && st < b.end) {
                        auto &e = by_what[r.what];
                        e.first++;
                        e.second += r.size;
                    }
            }
            logf("vitart: faixa %d esgotada pedindo 0x%x bytes para %s; ocupação:", static_cast<int>(pool), size, what);
            for (auto &[w, e] : by_what)
                if (e.first > 16 || e.second > 0x100000)
                    logf("    %-32s %6u blocos, %llu KiB", w.c_str(), e.first, (unsigned long long)(e.second >> 10));
            return 0;
        }
        b.cur = a + (size >= 0x1000 ? ((size + 0xFFF) & ~0xFFFu) : ((size + 15) & ~15u));
    }
    map_region(a, size ? size : 1, what);
    return a;
}

// =================================================================== módulos
struct Module {
    std::string path;
    HMODULE lib = nullptr;
    const arc_module_info *info = nullptr;
    const arc_func_entry *funcs = nullptr;
    unsigned nfuncs = 0;
    const arc_segment *segs = nullptr;
    unsigned nsegs = 0;
};
static std::vector<Module> g_modules;
static std::unordered_map<uint32_t, arc_func> g_funcs;

arc_func lookup_func(uint32_t key) {
    auto it = g_funcs.find(key);
    return it == g_funcs.end() ? nullptr : it->second;
}

const arc_module_info *main_module() { return g_modules.empty() ? nullptr : g_modules[0].info; }

// Ligação de cada stub de import.
struct Binding {
    enum Kind { exported, hle, missing } kind = missing;
    uint32_t target = 0;
    HleFn fn = nullptr;
    const arc_import_entry *imp = nullptr;
    std::atomic<bool> reported{ false };
};
static std::unordered_map<uint32_t, std::unique_ptr<Binding>> g_bindings;

static std::unordered_map<std::string, HleFn> &hle_table() {
    static std::unordered_map<std::string, HleFn> t;
    return t;
}
HleReg::HleReg(const char *name, HleFn fn) { hle_table()[name] = fn; }

static bool g_trace = false, g_strict = false;
static std::vector<std::string> g_trace_filter; // VITART_TRACE=a,b,c: só nomes que contêm a, b ou c

static bool trace_wanted(const char *name) {
    if (g_trace_filter.empty()) return true;
    if (!name) return false;
    for (const std::string &f : g_trace_filter)
        if (std::strstr(name, f.c_str())) return true;
    return false;
}

// ============================================================ serviços da DLL
static thread_local Thread *t_current;
Thread *current() { return t_current; }

void call_guest_raw(arc_ctx *ctx, uint32_t addr) {
    const arc_func fn = lookup_func(addr);
    if (!fn)
        fatal("chamada para código sem tradução em 0x%08x (lr=0x%08x)", addr, ctx->r[14]);
    fn(ctx);
}

uint32_t call_guest(arc_ctx *ctx, uint32_t addr, std::initializer_list<uint32_t> args) {
    int k = 0;
    for (uint32_t a : args) {
        if (k >= 4)
            fatal("call_guest com mais de 4 argumentos");
        ctx->r[k++] = a;
    }
    const uint32_t saved_lr = ctx->r[14];
    ctx->r[14] = 0xFFFFFFF1u; // retorno "para o host": só registrado em r[15]
    call_guest_raw(ctx, addr);
    ctx->r[14] = saved_lr;
    return ctx->r[0];
}

static void host_import(arc_ctx *ctx, uint32_t nid, uint32_t stub) {
    auto it = g_bindings.find(stub);
    if (it == g_bindings.end())
        fatal("chamada a stub de import desconhecido 0x%08x (NID 0x%08x)", stub, nid);
    Binding &b = *it->second;
    switch (b.kind) {
    case Binding::exported:
    case Binding::hle: {
        const bool tr = g_trace && trace_wanted(b.imp->name);
        uint32_t a[4] = { ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3] };
        const uint32_t lr = ctx->r[14];
        if (b.kind == Binding::exported) {
            stats().export_calls++;
            call_guest_raw(ctx, b.target);
        } else {
            stats().hle_calls++;
            b.fn(ctx);
        }
        if (tr)
            logf("[%s t%d] %s(%08x, %08x, %08x, %08x) = %08x lr=%08x", b.kind == Binding::hle ? "hle" : "exp",
                t_current ? t_current->id & 0xFFFF : 0, b.imp->name ? b.imp->name : "?", a[0], a[1], a[2], a[3], ctx->r[0], lr);
        return;
    }
    case Binding::missing:
        stats().missing_calls++;
        if (!b.reported.exchange(true))
            logf("vitart: HLE ausente: %s:%s (NID 0x%08x) args %08x %08x %08x %08x, lr=%08x -> devolve 0",
                b.imp->lib ? b.imp->lib : "?", b.imp->name ? b.imp->name : "?", nid, ctx->r[0], ctx->r[1], ctx->r[2], ctx->r[3], ctx->r[14]);
        if (g_strict)
            fatal("modo estrito: HLE ausente");
        ctx->r[0] = 0;
        return;
    }
}

static void host_dispatch_miss(arc_ctx *ctx, uint32_t target, uint32_t from) {
    // Ponteiro de função para outro módulo (callback): procura em todos.
    if (const arc_func fn = lookup_func(target)) {
        stats().dispatch_cross++;
        fn(ctx);
        return;
    }
    // Ponteiro para um stub de import (o jogo passa p. ex. sceFiosIOFilterPsarcDearchiver como
    // callback): mesma ligação de uma chamada direta.
    auto it = g_bindings.find(target & ~1u);
    if (it != g_bindings.end()) {
        stats().dispatch_cross++;
        const uint32_t lr = ctx->r[14];
        host_import(ctx, it->second->imp->nid, target & ~1u);
        if (it->second->kind != Binding::exported)
            ctx->r[15] = lr; // HLE volta para o LR do chamador (um export recompilado já gravou r[15])
        return;
    }
    fatal("desvio indireto para código sem tradução 0x%08x (de 0x%08x)", target, from);
}

static void host_ret_mismatch(arc_ctx *ctx, uint32_t expected) {
    fatal("retorno para 0x%08x, esperado 0x%08x (longjmp/troca de contexto do guest ainda não suportados)", ctx->r[15], expected);
}

static void host_svc(arc_ctx *, uint32_t imm, uint32_t pc) { fatal("svc #%u fora de stub em 0x%08x", imm, pc); }
static void host_fatal(arc_ctx *, uint32_t pc, const char *why) { fatal("código gerado: %s (0x%08x)", why ? why : "?", pc); }
static void snap_write(const char *tag) {
    const char *dir = getenv("VITART_SNAP_DIR");
    const std::string d = std::string(dir ? dir : "snap") + "/" + tag;
    std::filesystem::create_directories(d);
    if (Thread *t = t_current) {
        std::ofstream f(d + "/ctx.bin", std::ios::binary);
        f.write(reinterpret_cast<const char *>(&t->ctx), sizeof(arc_ctx));
    }
    dump_regions(d, 0x80000000u, 0xC0000000u);
    logf("vitart: estado salvo em %s", d.c_str());
}

static void host_log(const char *msg) {
    if (std::strncmp(msg, "@SNAP-ENTRY", 11) == 0) return snap_write("entry");
    if (std::strncmp(msg, "@SNAP-EXIT", 10) == 0) return snap_write("exit");
    std::string s(msg);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
        s.pop_back();
    logf("arc: %s", s.c_str());
}

static const arc_host_api g_api = { ARC_HOST_ABI, host_import, host_dispatch_miss, host_ret_mismatch, host_svc, host_fatal, host_log };

static void load_module(const std::string &path) {
    Module m;
    m.path = path;
    m.lib = LoadLibraryA(path.c_str());
    if (!m.lib)
        fatal("não foi possível carregar %s (erro %lu)", path.c_str(), GetLastError());
    auto sym = [&](const char *n) {
        void *p = reinterpret_cast<void *>(GetProcAddress(m.lib, n));
        if (!p)
            fatal("%s não exporta %s", path.c_str(), n);
        return p;
    };
    auto bind = reinterpret_cast<int (*)(const arc_host_api *, uint8_t *)>(sym("arc_bind"));
    auto get_table = reinterpret_cast<const arc_func_entry *(*)(unsigned *)>(sym("arc_get_table"));
    auto get_segs = reinterpret_cast<const arc_segment *(*)(unsigned *)>(sym("arc_get_segments"));
    auto get_mod = reinterpret_cast<const arc_module_info *(*)()>(sym("arc_get_module"));
    if (bind(&g_api, g_mem) != 0)
        fatal("%s: ABI incompatível", path.c_str());
    m.funcs = get_table(&m.nfuncs);
    m.segs = get_segs(&m.nsegs);
    m.info = get_mod();
    for (unsigned i = 0; i < m.nsegs; i++) {
        const arc_segment &s = m.segs[i];
        uint32_t st;
        std::string what;
        if (region_of(s.vaddr, &st, nullptr, &what) || region_of(s.vaddr + s.memsz - 1, &st, nullptr, &what))
            fatal("%s: segmento 0x%08x sobrepõe %s", path.c_str(), s.vaddr, what.c_str());
        map_region(s.vaddr, s.memsz, (std::string(m.info->name) + ":seg" + std::to_string(i)).c_str());
        std::memcpy(g_mem + s.vaddr, s.data, s.filesz);
    }
    for (unsigned i = 0; i < m.nfuncs; i++)
        g_funcs.emplace(m.funcs[i].key, m.funcs[i].fn);
    logf("vitart: %s (%s) carregado em 0x%08x: %u funções, %u imports, %u exports", m.info->name, path.c_str(),
        m.info->load_base, m.nfuncs, m.info->import_count, m.info->export_count);
    g_modules.push_back(m);
}

static std::string describe_host_pc(const void *pc) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(pc);
    for (const Module &m : g_modules) {
        HMODULE h = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(pc), &h) || h != m.lib)
            continue;
        // A função traduzida mais próxima abaixo do pc (as funções ficam contíguas no .text).
        uintptr_t best = 0;
        uint32_t key = 0;
        for (unsigned i = 0; i < m.nfuncs; i++) {
            const uintptr_t f = reinterpret_cast<uintptr_t>(m.funcs[i].fn);
            if (f <= a && f > best) { best = f; key = m.funcs[i].key; }
        }
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s, função do guest 0x%08x + 0x%llx no host", m.info->name, key,
            (unsigned long long)(a - best));
        return buf;
    }
    return "código fora dos módulos do guest (runtime/HLE)";
}

static void dump_thread_regs() {
    Thread *t = t_current;
    if (!t)
        return;
    const arc_ctx &c = t->ctx;
    logf("    thread %d (%s): r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x", t->id, t->name.c_str(),
        c.r[0], c.r[1], c.r[2], c.r[3], c.r[4], c.r[5], c.r[6], c.r[7]);
    logf("        r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x sp=%08x lr=%08x pc=%08x", c.r[8], c.r[9], c.r[10], c.r[11],
        c.r[12], c.r[13], c.r[14], c.r[15]);
}

static void link_imports() {
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> exports;
    for (const Module &m : g_modules)
        for (unsigned i = 0; i < m.info->export_count; i++) {
            const arc_export_entry &e = m.info->exports[i];
            if (e.kind == 0 && e.lib && std::strcmp(e.lib, "<main>") != 0)
                exports[{ e.lib_nid, e.nid }] = e.addr;
        }
    unsigned n_exp = 0, n_hle = 0, n_missing = 0;
    std::map<std::string, unsigned> missing_by_lib;
    for (const Module &m : g_modules)
        for (unsigned i = 0; i < m.info->import_count; i++) {
            const arc_import_entry &imp = m.info->imports[i];
            if (imp.kind != 0)
                continue; // variáveis já ligadas na tradução
            auto b = std::make_unique<Binding>();
            b->imp = &imp;
            auto e = exports.find({ imp.lib_nid, imp.nid });
            if (e != exports.end()) {
                b->kind = Binding::exported;
                b->target = e->second;
                n_exp++;
            } else if (imp.name && hle_table().count(imp.name)) {
                b->kind = Binding::hle;
                b->fn = hle_table()[imp.name];
                n_hle++;
            } else {
                n_missing++;
                missing_by_lib[imp.lib ? imp.lib : "?"]++;
            }
            g_bindings[imp.stub] = std::move(b);
        }
    logf("vitart: imports de função: %u para outros módulos, %u HLE implementados, %u ausentes", n_exp, n_hle, n_missing);
    for (auto &[lib, n] : missing_by_lib)
        logf("    ausentes em %-24s %u", lib.c_str(), n);
}

static void load_hle_vars(const std::string &path) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream in(line);
        std::string addr, lib, name;
        in >> addr >> lib >> name;
        if (addr.empty())
            continue;
        const uint32_t a = static_cast<uint32_t>(std::stoul(addr, nullptr, 16));
        map_region(a & ~0xFu, 16, ("var:" + name).c_str());
        if (name == "__stack_chk_guard")
            wr32(a, 0x5A17CA11u); // canário: qualquer valor fixo serve, o jogo só compara
        logf("vitart: variável do sistema %s:%s em 0x%08x", lib.c_str(), name.c_str(), a);
    }
}

// ==================================================================== threads
static std::mutex g_threads_mutex;
static std::unordered_map<int, std::unique_ptr<Thread>> g_threads;
static std::atomic<int> g_next_thread_id{ 0x40010001 };
constexpr uint32_t KERNEL_TLS_SIZE = 0x800;

Thread *find_thread(int id) {
    const std::lock_guard<std::mutex> lock(g_threads_mutex);
    auto it = g_threads.find(id);
    return it == g_threads.end() ? nullptr : it->second.get();
}

Thread *create_thread(const char *name, uint32_t entry, int priority, uint32_t stack_size) {
    auto t = std::make_unique<Thread>();
    t->id = g_next_thread_id++;
    t->name = name ? name : "";
    t->entry = entry;
    t->priority = priority;
    t->stack_size = stack_size ? stack_size : 0x40000;
    t->stack = galloc(Pool::kernel, t->stack_size, 0x1000, ("stack:" + t->name).c_str());
    const arc_module_info *mi = main_module();
    const uint32_t tls_msize = mi ? mi->tls_memsz : 0;
    t->tls = galloc(Pool::kernel, KERNEL_TLS_SIZE + tls_msize + 16, 0x10, ("tls:" + t->name).c_str());
    if (!t->stack || !t->tls)
        return nullptr;
    std::memset(g_mem + t->tls, 0, KERNEL_TLS_SIZE + tls_msize);
    wr32(t->tls + 0, 1);                          // TLS_PROCESS_ID
    wr32(t->tls + 4, (uint32_t)t->id);            // TLS_THREAD_ID
    wr32(t->tls + 8, t->stack);                   // TLS_SP_TOP
    wr32(t->tls + 12, t->stack + t->stack_size);  // TLS_SP_BOTTOM
    wr32(t->tls + 32, (uint32_t)priority);        // TLS_CURRENT_PRIORITY
    if (mi && mi->tls_addr && mi->tls_filesz)
        std::memcpy(g_mem + t->tls + KERNEL_TLS_SIZE, g_mem + mi->tls_addr, mi->tls_filesz);
    t->ctx.tpidruro = t->tls + KERNEL_TLS_SIZE;
    t->ctx.r[13] = (t->stack + t->stack_size) & ~7u;
    Thread *raw = t.get();
    const std::lock_guard<std::mutex> lock(g_threads_mutex);
    g_threads[raw->id] = std::move(t);
    return raw;
}

// ================================================================ perfilador
// VITART_PROF=1: amostra a cada 1 ms o RIP de cada thread do guest que gastou CPU desde a amostra anterior
// (thread parada em espera não conta) e grava prof.txt a cada 10 s: "amostras módulo+rva thread".
// Os RVAs da DLL do jogo viram funções com tools/prof_resolve.py e o mapa do lld-link.
static std::mutex g_prof_mutex;
struct ProfThread { HANDLE h; std::string name; ULONG64 cycles = 0; };
static std::vector<ProfThread> g_prof_threads;

static void prof_register(const Thread *t) {
    static const bool on = getenv("VITART_PROF") != nullptr;
    if (!on) return;
    HANDLE h = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    const std::lock_guard<std::mutex> lock(g_prof_mutex);
    g_prof_threads.push_back({h, t->name});
}

static void prof_start() {
    if (!getenv("VITART_PROF")) return;
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        timeBeginPeriod(1);
        std::map<std::pair<std::string, uint64_t>, uint64_t> hist; // (thread, rip) -> amostras
        std::map<std::string, uint64_t> busy;
        uint64_t ticks = 0;
        auto last = std::chrono::steady_clock::now();
        for (;;) {
            Sleep(1);
            ticks++;
            {
                const std::lock_guard<std::mutex> lock(g_prof_mutex);
                for (ProfThread &pt : g_prof_threads) {
                    ULONG64 c = 0;
                    QueryThreadCycleTime(pt.h, &c);
                    const ULONG64 d = c - pt.cycles;
                    pt.cycles = c;
                    if (d < 200000) continue; // menos de ~5% de 1 ms: dormindo
                    if (SuspendThread(pt.h) == (DWORD)-1) continue;
                    CONTEXT cx{};
                    cx.ContextFlags = CONTEXT_CONTROL;
                    if (GetThreadContext(pt.h, &cx)) hist[{pt.name, cx.Rip}]++;
                    ResumeThread(pt.h);
                    busy[pt.name]++;
                }
            }
            if (std::chrono::steady_clock::now() - last < std::chrono::seconds(10)) continue;
            last = std::chrono::steady_clock::now();
            // Agrega por módulo+RVA e grava.
            std::map<std::string, uint64_t> agg;
            for (auto &kv : hist) {
                HMODULE m = nullptr;
                char mod[MAX_PATH] = "?";
                uint64_t rva = kv.first.second;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)kv.first.second, &m) && m) {
                    GetModuleFileNameA(m, mod, sizeof mod);
                    rva -= (uint64_t)m;
                }
                const char *b = std::strrchr(mod, '\\');
                char key[MAX_PATH + 96];
                std::snprintf(key, sizeof key, "%s+%llx %s", b ? b + 1 : mod, (unsigned long long)rva, kv.first.first.c_str());
                agg[key] += kv.second;
            }
            std::vector<std::pair<uint64_t, std::string>> v;
            for (auto &kv : agg) v.push_back({kv.second, kv.first});
            std::sort(v.rbegin(), v.rend());
            if (FILE *f = fopen("prof.txt", "w")) {
                fprintf(f, "# ticks %llu\n", (unsigned long long)ticks);
                for (auto &kv : busy) fprintf(f, "# thread %s %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
                for (auto &e : v) fprintf(f, "%llu %s\n", (unsigned long long)e.first, e.second.c_str());
                fclose(f);
            }
        }
    }).detach();
}

static void thread_main(Thread *t, uint32_t arglen, uint32_t argp) {
    t_current = t;
    prof_register(t);
    t->state = 1;
    arc_ctx *ctx = &t->ctx;
    ctx->r[0] = arglen;
    ctx->r[1] = argp;
    ctx->r[14] = 0xFFFFFFF1u;
    t->can_exit = true;
    if (setjmp(t->exit_jb) == 0) {
        call_guest_raw(ctx, t->entry);
        t->exit_status = (int)ctx->r[0];
    }
    {
        const std::lock_guard<std::mutex> lock(t->m);
        t->state = 2;
    }
    t->cv.notify_all();
    if (g_trace)
        logf("vitart: thread %d (%s) terminou com %d", t->id, t->name.c_str(), t->exit_status);
}

int start_thread(Thread *t, uint32_t arglen, uint32_t argp) {
    if (t->state != 0)
        return (int)0x80028023; // SCE_KERNEL_ERROR_NOT_DORMANT
    uint32_t copy = 0;
    if (argp && arglen) {
        uint32_t sp = (t->ctx.r[13] - ((arglen + 7) & ~7u)) & ~7u;
        std::memcpy(g_mem + sp, g_mem + argp, arglen);
        t->ctx.r[13] = sp;
        copy = sp;
    }
    std::thread(thread_main, t, arglen, copy).detach();
    return 0;
}

Thread *spawn_host_thread(const char *name, std::function<void(Thread *)> body) {
    Thread *t = create_thread(name, 0, 0x10000100, 0x10000);
    if (!t)
        fatal("sem memória para a thread %s", name);
    std::thread([t, body = std::move(body)] {
        t_current = t;
        prof_register(t);
        t->state = 1;
        t->ctx.r[14] = 0xFFFFFFF1u;
        body(t);
        t->state = 2;
    }).detach();
    return t;
}

void exit_current_thread(int status) {
    Thread *t = current();
    if (!t || !t->can_exit)
        fatal("saída de thread fora de uma thread do guest");
    t->exit_status = status;
    std::longjmp(t->exit_jb, 1);
}

int wait_thread_end(Thread *t, uint32_t timeout_us, int *status) {
    std::unique_lock<std::mutex> lock(t->m);
    if (timeout_us) {
        if (!t->cv.wait_for(lock, std::chrono::microseconds(timeout_us), [&] { return t->state == 2; }))
            return (int)0x80028005; // SCE_KERNEL_ERROR_WAIT_TIMEOUT
    } else
        t->cv.wait(lock, [&] { return t->state == 2; });
    if (status)
        *status = t->exit_status;
    return 0;
}

// =================================================================== partida
static std::string g_app, g_data;
std::string app_path() { return g_app; }
std::string data_path() { return g_data; }

static uint32_t process_param_field(const arc_module_info *mi, unsigned word) {
    for (unsigned i = 0; i < mi->export_count; i++) {
        const arc_export_entry &e = mi->exports[i];
        if (e.nid == 0x70FBA1E7u && rd32(e.addr + 4) == 0x32505350u) // "PSP2"
            return rd32(e.addr + 4 * word);
    }
    return 0;
}

} // namespace vt

using namespace vt;

static void usage() {
    fprintf(stderr, "uso: vitart --app <dir app0> [--data <dir>] [--hle-vars link.hlevars.txt] [--log arquivo] "
                    "principal.dll [biblioteca.dll ...]\n"
                    "ambiente: VITART_TRACE=1 (registra cada chamada HLE), VITART_STRICT=1 (HLE ausente é fatal)\n");
    exit(2);
}

static std::vector<std::string> g_argv;  // linha de comando original
static std::string g_main_dll;            // DLL do módulo principal (a primeira da lista)

[[noreturn]] void vt::exec_main_module(const std::string &dll) {
    std::string cmd;
    for (size_t i = 0; i < g_argv.size(); i++) {
        const std::string &a = (g_argv[i] == g_main_dll) ? dll : g_argv[i];
        if (i) cmd += ' ';
        cmd += '"' + a + '"';
    }
    logf("vitart: LoadExec -> %s", cmd.c_str());
    if (g_logfile) fflush(g_logfile);
    STARTUPINFOA si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
        fatal("LoadExec: CreateProcess falhou (%lu)", GetLastError());
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

int main(int argc, char **argv) {
    g_argv.assign(argv, argv + argc);
    std::vector<std::string> dlls;
    std::string hle_vars;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--app" && i + 1 < argc) g_app = argv[++i];
        else if (a == "--data" && i + 1 < argc) g_data = argv[++i];
        else if (a == "--hle-vars" && i + 1 < argc) hle_vars = argv[++i];
        else if (a == "--log" && i + 1 < argc) g_logfile = fopen(argv[++i], "w");
        else if (a.rfind("--", 0) == 0) usage();
        else dlls.push_back(a);
    }
    if (dlls.empty() || g_app.empty())
        usage();
    g_main_dll = dlls[0];
    if (g_data.empty())
        g_data = g_app + "/../vitart-data";
    if (const char *tr = getenv("VITART_TRACE")) {
        g_trace = true;
        std::string f = tr;
        if (f != "1")
            for (size_t p = 0; p <= f.size();) {
                const size_t q = (std::min)(f.find(',', p), f.size());
                if (q > p) g_trace_filter.push_back(f.substr(p, q - p));
                p = q + 1;
            }
    }
    g_strict = getenv("VITART_STRICT") != nullptr;
    prof_start();

    mem_init();
    for (const std::string &d : dlls)
        load_module(d);
    if (!hle_vars.empty())
        load_hle_vars(hle_vars);
    link_imports();
    if (const char *w = getenv("VITART_WATCH")) {
        g_watch_addr = (uint32_t)std::strtoul(w, nullptr, 0);
        const char *c = std::strchr(w, ',');
        g_watch_len = c ? (uint32_t)std::strtoul(c + 1, nullptr, 0) : 4;
        if (const char *m = getenv("VITART_WATCH_MAX"))
            g_watch_max = (unsigned)std::strtoul(m, nullptr, 0);
        commit_range(g_watch_addr, g_watch_len);
        watch_protect(true);
        logf("vitart: vigiando escritas em 0x%08x..+%u", g_watch_addr, g_watch_len);
    }

    const arc_module_info *mi = main_module();
    const uint32_t prio_p = process_param_field(mi, 5), stack_p = process_param_field(mi, 6);
    const int prio = prio_p ? (int)rd32(prio_p) : 0x10000100;
    const uint32_t stack = stack_p ? rd32(stack_p) : 0x40000;
    logf("vitart: thread principal: prioridade 0x%x, pilha 0x%x, entrada 0x%08x", prio, stack, mi->module_start);

    Thread *main_thread = create_thread("main", mi->module_start, prio, stack);
    if (!main_thread)
        fatal("não foi possível criar a thread principal");

    // module_start das bibliotecas antes do jogo, cada um numa thread própria (o carregador do Vita
    // faz assim; a libc termina o seu com sceKernelExitThread).
    for (size_t i = 1; i < g_modules.size(); i++) {
        const arc_module_info *li = g_modules[i].info;
        if (!li->module_start)
            continue;
        Thread *lt = create_thread((std::string("module_start:") + li->name).c_str(), li->module_start, prio, 0x10000);
        if (!lt)
            fatal("sem memória para a thread de module_start de %s", li->name);
        start_thread(lt, 0, 0);
        int r = 0;
        wait_thread_end(lt, 0, &r);
        logf("vitart: module_start de %s terminou com 0x%08x", li->name, (uint32_t)r);
    }

    const auto t0 = std::chrono::steady_clock::now();
    start_thread(main_thread, 0, 0);
    int status = 0;
    wait_thread_end(main_thread, 0, &status);
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    logf("vitart: thread principal terminou com %d após %.2f s; HLE %llu, entre módulos %llu, ausentes %llu, callbacks %llu", status, secs,
        (unsigned long long)stats().hle_calls.load(), (unsigned long long)stats().export_calls.load(),
        (unsigned long long)stats().missing_calls.load(), (unsigned long long)stats().dispatch_cross.load());
    return status;
}
