// HLE do kernel do Vita para o vitart: threads, sincronização, memória, tempo e processo.
// Implementação própria a partir das assinaturas públicas do SDK (vita-headers); a semântica
// segue a documentação dos headers e o comportamento observado do sistema.
#include "vitart.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <thread>
#include <unordered_map>

namespace vt {

constexpr uint32_t SCE_OK = 0;
constexpr uint32_t ERR_UNKNOWN_UID = 0x80028001u;       // genérico de objeto inexistente
constexpr uint32_t ERR_WAIT_TIMEOUT = 0x80028005u;
constexpr uint32_t ERR_WAIT_CANCEL = 0x80028006u;
constexpr uint32_t ERR_SEMA_ZERO = 0x8002819Bu;
constexpr uint32_t ERR_LW_MUTEX_FAILED_TO_OWN = 0x8002820Bu;
constexpr uint32_t ERR_MUTEX_FAILED_TO_OWN = 0x800281CAu;
constexpr uint32_t ERR_EVF_COND = 0x800281A4u;
constexpr uint32_t ERR_NO_MEMORY = 0x80020007u;

static const auto g_t0 = std::chrono::steady_clock::now();
static uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0).count();
}

static std::atomic<uint32_t> g_uid{ 0x00010001 };
static uint32_t new_uid() { return g_uid.fetch_add(2); }

// Espera com timeout opcional (ponteiro de SceUInt32 em microssegundos; 0 = infinito).
template <class Lock, class Pred>
static bool wait_for(std::condition_variable &cv, Lock &lock, uint32_t ptimeout, Pred pred) {
    if (!ptimeout) {
        cv.wait(lock, pred);
        return true;
    }
    const uint32_t us = rd32(ptimeout);
    const bool ok = cv.wait_for(lock, std::chrono::microseconds(us), pred);
    if (!ok)
        wr32(ptimeout, 0);
    return ok;
}

// =================================================================== threads
VT_HLE(sceKernelCreateThread) {
    // (name, entry, initPriority, stackSize, attr, cpuAffinityMask, option)
    Thread *t = create_thread(gstr(arg(ctx, 0)), arg(ctx, 1), (int)arg(ctx, 2), arg(ctx, 3));
    ret(ctx, t ? (uint32_t)t->id : ERR_NO_MEMORY);
}
VT_HLE(sceKernelStartThread) {
    Thread *t = find_thread((int)arg(ctx, 0));
    ret(ctx, t ? (uint32_t)start_thread(t, arg(ctx, 1), arg(ctx, 2)) : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelExitThread) { exit_current_thread((int)arg(ctx, 0)); }
VT_HLE(sceKernelExitDeleteThread) { exit_current_thread((int)arg(ctx, 0)); }
VT_HLE(sceKernelDeleteThread) { ret(ctx, SCE_OK); }
VT_HLE(sceKernelWaitThreadEnd) {
    Thread *t = find_thread((int)arg(ctx, 0));
    if (!t) return ret(ctx, ERR_UNKNOWN_UID);
    int st = 0;
    const uint32_t pto = arg(ctx, 2);
    const int r = wait_thread_end(t, pto ? rd32(pto) : 0, &st);
    if (r == 0 && arg(ctx, 1)) wr32(arg(ctx, 1), (uint32_t)st);
    ret(ctx, (uint32_t)r);
}
VT_HLE(sceKernelGetThreadExitStatus) {
    Thread *t = find_thread((int)arg(ctx, 0));
    if (!t) return ret(ctx, ERR_UNKNOWN_UID);
    if (t->state != 2) return ret(ctx, 0x80028023u); // ainda não terminou
    if (arg(ctx, 1)) wr32(arg(ctx, 1), (uint32_t)t->exit_status);
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelGetThreadId) { ret(ctx, current() ? (uint32_t)current()->id : 0); }
VT_HLE(sceKernelGetProcessId) { ret(ctx, 0x00010005u); }
VT_HLE(sceKernelDelayThread) { std::this_thread::sleep_for(std::chrono::microseconds(arg(ctx, 0))); ret(ctx, SCE_OK); }
VT_HLE(sceKernelDelayThreadCB) { std::this_thread::sleep_for(std::chrono::microseconds(arg(ctx, 0))); ret(ctx, SCE_OK); }
VT_HLE(sceKernelChangeThreadPriority) { ret(ctx, SCE_OK); }
VT_HLE(sceKernelChangeThreadCpuAffinityMask) { ret(ctx, SCE_OK); }
VT_HLE(sceKernelChangeThreadVfpException) { ret(ctx, SCE_OK); }
VT_HLE(sceKernelGetThreadCurrentPriority) { ret(ctx, current() ? (uint32_t)current()->priority : 0x10000100u); }
VT_HLE(sceKernelGetThreadCpuAffinityMask2) { ret(ctx, 0); }
VT_HLE(sceKernelRegisterThreadEventHandler) { ret(ctx, new_uid()); }
VT_HLE(sceKernelUnregisterThreadEventHandler) { ret(ctx, SCE_OK); }

// TLS por chave: área reservada dentro da TLS do kernel de cada thread (0x400.. 0x7FF).
VT_HLE(sceKernelGetTLSAddr) {
    const uint32_t key = arg(ctx, 0);
    Thread *t = current();
    ret(ctx, (t && key < 0x100) ? t->tls + 0x400 + 4 * key : 0);
}

// ============================================================ sincronização
struct Mutex {
    std::mutex m;
    std::condition_variable cv;
    int owner = 0, count = 0;
    bool recursive = false;
    std::string name;
};
struct Sema {
    std::mutex m;
    std::condition_variable cv;
    int count = 0, max = 1;
    std::string name;
};
struct EventFlag {
    std::mutex m;
    std::condition_variable cv;
    uint32_t bits = 0;
    std::string name;
};
struct LwCond {
    std::condition_variable_any cv;
    uint32_t mutex_work = 0;
};

static std::mutex g_obj_mutex;
static std::unordered_map<uint32_t, std::shared_ptr<Mutex>> g_mutexes;     // uid ou endereço do work (lw)
static std::unordered_map<uint32_t, std::shared_ptr<Sema>> g_semas;
static std::unordered_map<uint32_t, std::shared_ptr<EventFlag>> g_evfs;
static std::unordered_map<uint32_t, std::shared_ptr<LwCond>> g_lwconds;

template <class T>
static std::shared_ptr<T> get_obj(std::unordered_map<uint32_t, std::shared_ptr<T>> &tab, uint32_t key) {
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    auto it = tab.find(key);
    return it == tab.end() ? nullptr : it->second;
}

static int self_id() { return current() ? current()->id : -1; }

static uint32_t mutex_lock(Mutex &mx, int count, uint32_t ptimeout, bool try_only) {
    std::unique_lock<std::mutex> lock(mx.m);
    const int me = self_id();
    if (mx.count > 0 && mx.owner == me) {
        if (!mx.recursive)
            return ERR_MUTEX_FAILED_TO_OWN;
        mx.count += count;
        return SCE_OK;
    }
    if (try_only && mx.count > 0)
        return ERR_MUTEX_FAILED_TO_OWN;
    if (!wait_for(mx.cv, lock, ptimeout, [&] { return mx.count == 0; }))
        return ERR_WAIT_TIMEOUT;
    mx.owner = me;
    mx.count = count;
    return SCE_OK;
}
static uint32_t mutex_unlock(Mutex &mx, int count) {
    std::unique_lock<std::mutex> lock(mx.m);
    if (mx.count == 0 || mx.owner != self_id())
        return 0x800281C8u; // SCE_KERNEL_ERROR_MUTEX_UNLOCK_UDF
    mx.count -= count;
    if (mx.count <= 0) {
        mx.count = 0;
        mx.owner = 0;
        lock.unlock();
        mx.cv.notify_one();
    }
    return SCE_OK;
}

VT_HLE(sceKernelCreateMutex) {
    // (name, attr, initCount, option)
    auto mx = std::make_shared<Mutex>();
    mx->name = gstr(arg(ctx, 0));
    mx->recursive = (arg(ctx, 1) & 0x02) != 0;
    const uint32_t uid = new_uid();
    if ((int)arg(ctx, 2) > 0) {
        mx->owner = self_id();
        mx->count = (int)arg(ctx, 2);
    }
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    g_mutexes[uid] = mx;
    ret(ctx, uid);
}
VT_HLE(sceKernelDeleteMutex) {
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    ret(ctx, g_mutexes.erase(arg(ctx, 0)) ? SCE_OK : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelLockMutex) {
    auto mx = get_obj(g_mutexes, arg(ctx, 0));
    ret(ctx, mx ? mutex_lock(*mx, (int)arg(ctx, 1), arg(ctx, 2), false) : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelTryLockMutex) {
    auto mx = get_obj(g_mutexes, arg(ctx, 0));
    ret(ctx, mx ? mutex_lock(*mx, (int)arg(ctx, 1), 0, true) : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelUnlockMutex) {
    auto mx = get_obj(g_mutexes, arg(ctx, 0));
    ret(ctx, mx ? mutex_unlock(*mx, (int)arg(ctx, 1)) : ERR_UNKNOWN_UID);
}

// LwMutex: o objeto vive na área de trabalho do guest (32 bytes); a chave é o endereço dela.
VT_HLE(sceKernelCreateLwMutex) {
    // (pWork, name, attr, initCount, pOptParam)
    auto mx = std::make_shared<Mutex>();
    mx->name = gstr(arg(ctx, 1));
    mx->recursive = (arg(ctx, 2) & 0x02) != 0;
    if ((int)arg(ctx, 3) > 0) {
        mx->owner = self_id();
        mx->count = (int)arg(ctx, 3);
    }
    std::memset(ptr<uint8_t>(arg(ctx, 0)), 0, 32);
    wr32(arg(ctx, 0), new_uid()); // um id qualquer no work, como o sistema faz
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    g_mutexes[arg(ctx, 0)] = mx;
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelDeleteLwMutex) {
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    g_mutexes.erase(arg(ctx, 0));
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelLockLwMutex) {
    auto mx = get_obj(g_mutexes, arg(ctx, 0));
    ret(ctx, mx ? mutex_lock(*mx, (int)arg(ctx, 1), arg(ctx, 2), false) : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelTryLockLwMutex) {
    auto mx = get_obj(g_mutexes, arg(ctx, 0));
    if (!mx) return ret(ctx, ERR_UNKNOWN_UID);
    const uint32_t r = mutex_lock(*mx, (int)arg(ctx, 1), 0, true);
    ret(ctx, r == SCE_OK ? SCE_OK : ERR_LW_MUTEX_FAILED_TO_OWN);
}
VT_HLE(sceKernelUnlockLwMutex) {
    auto mx = get_obj(g_mutexes, arg(ctx, 0));
    ret(ctx, mx ? mutex_unlock(*mx, (int)arg(ctx, 1)) : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelUnlockLwMutex2) {
    auto mx = get_obj(g_mutexes, arg(ctx, 0));
    ret(ctx, mx ? mutex_unlock(*mx, (int)arg(ctx, 1)) : ERR_UNKNOWN_UID);
}

// LwCond: associada a um LwMutex; esperar libera o mutex (com toda a contagem) e o retoma.
VT_HLE(sceKernelCreateLwCond) {
    // (pWork, name, attr, pLwMutex, pOptParam)
    auto c = std::make_shared<LwCond>();
    c->mutex_work = arg(ctx, 3);
    std::memset(ptr<uint8_t>(arg(ctx, 0)), 0, 16);
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    g_lwconds[arg(ctx, 0)] = c;
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelDeleteLwCond) {
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    g_lwconds.erase(arg(ctx, 0));
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelWaitLwCond) {
    auto c = get_obj(g_lwconds, arg(ctx, 0));
    if (!c) return ret(ctx, ERR_UNKNOWN_UID);
    auto mx = get_obj(g_mutexes, c->mutex_work);
    if (!mx) return ret(ctx, ERR_UNKNOWN_UID);
    std::unique_lock<std::mutex> lock(mx->m);
    if (mx->owner != self_id() || mx->count == 0) return ret(ctx, 0x800281C8u);
    const int saved = mx->count;
    mx->count = 0;
    mx->owner = 0;
    mx->cv.notify_one();
    const uint32_t pto = arg(ctx, 1);
    bool ok;
    if (pto)
        ok = c->cv.wait_for(lock, std::chrono::microseconds(rd32(pto))) == std::cv_status::no_timeout;
    else {
        c->cv.wait(lock);
        ok = true;
    }
    mx->cv.wait(lock, [&] { return mx->count == 0; });
    mx->owner = self_id();
    mx->count = saved;
    ret(ctx, ok ? SCE_OK : ERR_WAIT_TIMEOUT);
}
VT_HLE(sceKernelSignalLwCond) {
    auto c = get_obj(g_lwconds, arg(ctx, 0));
    if (c) c->cv.notify_one();
    ret(ctx, c ? SCE_OK : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelSignalLwCondAll) {
    auto c = get_obj(g_lwconds, arg(ctx, 0));
    if (c) c->cv.notify_all();
    ret(ctx, c ? SCE_OK : ERR_UNKNOWN_UID);
}

// Semáforos.
static std::map<std::string, uint32_t> g_named_semas;
VT_HLE(sceKernelCreateSema) {
    // (name, attr, initCount, maxCount, option)
    auto s = std::make_shared<Sema>();
    s->name = gstr(arg(ctx, 0));
    s->count = (int)arg(ctx, 2);
    s->max = (int)arg(ctx, 3);
    const uint32_t uid = new_uid();
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    g_semas[uid] = s;
    g_named_semas[s->name] = uid;
    ret(ctx, uid);
}
VT_HLE(sceKernelOpenSema) {
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    auto it = g_named_semas.find(gstr(arg(ctx, 0)));
    ret(ctx, it == g_named_semas.end() ? ERR_UNKNOWN_UID : it->second);
}
VT_HLE(sceKernelCloseSema) { ret(ctx, SCE_OK); }
VT_HLE(sceKernelDeleteSema) {
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    ret(ctx, g_semas.erase(arg(ctx, 0)) ? SCE_OK : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelWaitSema) {
    auto s = get_obj(g_semas, arg(ctx, 0));
    if (!s) return ret(ctx, ERR_UNKNOWN_UID);
    const int need = (int)arg(ctx, 1);
    std::unique_lock<std::mutex> lock(s->m);
    if (!wait_for(s->cv, lock, arg(ctx, 2), [&] { return s->count >= need; }))
        return ret(ctx, ERR_WAIT_TIMEOUT);
    s->count -= need;
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelPollSema) {
    auto s = get_obj(g_semas, arg(ctx, 0));
    if (!s) return ret(ctx, ERR_UNKNOWN_UID);
    const std::lock_guard<std::mutex> lock(s->m);
    if (s->count < (int)arg(ctx, 1)) return ret(ctx, ERR_SEMA_ZERO);
    s->count -= (int)arg(ctx, 1);
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelSignalSema) {
    auto s = get_obj(g_semas, arg(ctx, 0));
    if (!s) return ret(ctx, ERR_UNKNOWN_UID);
    {
        const std::lock_guard<std::mutex> lock(s->m);
        if (s->count + (int)arg(ctx, 1) > s->max) return ret(ctx, 0x8002819Cu); // SEMA_OVF
        s->count += (int)arg(ctx, 1);
    }
    s->cv.notify_all();
    ret(ctx, SCE_OK);
}

// Event flags: espera E (todos os bits) ou OU (algum), com limpeza opcional.
VT_HLE(sceKernelCreateEventFlag) {
    // (name, attr, initPattern, option)
    auto e = std::make_shared<EventFlag>();
    e->name = gstr(arg(ctx, 0));
    e->bits = arg(ctx, 2);
    const uint32_t uid = new_uid();
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    g_evfs[uid] = e;
    ret(ctx, uid);
}
VT_HLE(sceKernelDeleteEventFlag) {
    const std::lock_guard<std::mutex> lock(g_obj_mutex);
    ret(ctx, g_evfs.erase(arg(ctx, 0)) ? SCE_OK : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelSetEventFlag) {
    auto e = get_obj(g_evfs, arg(ctx, 0));
    if (!e) return ret(ctx, ERR_UNKNOWN_UID);
    {
        const std::lock_guard<std::mutex> lock(e->m);
        e->bits |= arg(ctx, 1);
    }
    e->cv.notify_all();
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelClearEventFlag) {
    auto e = get_obj(g_evfs, arg(ctx, 0));
    if (!e) return ret(ctx, ERR_UNKNOWN_UID);
    const std::lock_guard<std::mutex> lock(e->m);
    e->bits &= arg(ctx, 1); // o argumento é a máscara a MANTER
    ret(ctx, SCE_OK);
}
static bool evf_match(uint32_t bits, uint32_t pattern, uint32_t mode) {
    return (mode & 1) ? (bits & pattern) != 0 : (bits & pattern) == pattern;
}
static uint32_t evf_wait(arc_ctx *ctx, bool poll) {
    auto e = get_obj(g_evfs, arg(ctx, 0));
    if (!e) return ERR_UNKNOWN_UID;
    const uint32_t pattern = arg(ctx, 1), mode = arg(ctx, 2), pout = arg(ctx, 3);
    std::unique_lock<std::mutex> lock(e->m);
    if (poll) {
        if (!evf_match(e->bits, pattern, mode)) return ERR_EVF_COND;
    } else if (!wait_for(e->cv, lock, arg(ctx, 4), [&] { return evf_match(e->bits, pattern, mode); }))
        return ERR_WAIT_TIMEOUT;
    if (pout) wr32(pout, e->bits);
    if (mode & 2) e->bits = 0;                 // SCE_EVENT_WAITCLEAR
    else if (mode & 4) e->bits &= ~pattern;    // SCE_EVENT_WAITCLEAR_PAT
    return SCE_OK;
}
VT_HLE(sceKernelWaitEventFlag) { ret(ctx, evf_wait(ctx, false)); }
VT_HLE(sceKernelPollEventFlag) { ret(ctx, evf_wait(ctx, true)); }

// ==================================================================== memória
struct MemBlock {
    uint32_t base, size, type;
    std::string name;
};
static std::mutex g_mb_mutex;
static std::unordered_map<uint32_t, MemBlock> g_memblocks;

VT_HLE(sceKernelAllocMemBlock) {
    // (name, type, size, opt)
    const uint32_t type = arg(ctx, 1), size = arg(ctx, 2);
    const bool cdram = type == 0x09408060u || type == 0x0C208060u; // USER_CDRAM_RW / USER_MAIN_PHYCONT_NC_RW?
    const Pool pool = (type == 0x09408060u) ? Pool::cdram : Pool::user;
    (void)cdram;
    const std::string name = gstr(arg(ctx, 0));
    const uint32_t base = galloc(pool, size, 0x40000, ("memblock:" + name).c_str());
    if (!base) return ret(ctx, ERR_NO_MEMORY);
    const uint32_t uid = new_uid();
    {
        const std::lock_guard<std::mutex> lock(g_mb_mutex);
        g_memblocks[uid] = MemBlock{ base, size, type, name };
    }
    logf("vitart: memblock %s tipo 0x%08x 0x%x bytes -> 0x%08x", name.c_str(), type, size, base);
    ret(ctx, uid);
}
VT_HLE(sceKernelGetMemBlockBase) {
    const std::lock_guard<std::mutex> lock(g_mb_mutex);
    auto it = g_memblocks.find(arg(ctx, 0));
    if (it == g_memblocks.end()) return ret(ctx, ERR_UNKNOWN_UID);
    wr32(arg(ctx, 1), it->second.base);
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelFreeMemBlock) {
    const std::lock_guard<std::mutex> lock(g_mb_mutex);
    ret(ctx, g_memblocks.erase(arg(ctx, 0)) ? SCE_OK : ERR_UNKNOWN_UID);
}
VT_HLE(sceKernelFindMemBlockByAddr) {
    const uint32_t a = arg(ctx, 0);
    const std::lock_guard<std::mutex> lock(g_mb_mutex);
    for (auto &[uid, mb] : g_memblocks)
        if (a - mb.base < mb.size) return ret(ctx, uid);
    ret(ctx, 0x80020024u); // SCE_KERNEL_ERROR_BLOCK_NOT_FOUND? (nenhum)
}
VT_HLE(sceKernelGetMemBlockInfoByAddr) {
    // (base, SceKernelMemBlockInfo *): {size=0x18, mappedBase, mappedSize, memoryType, access, type}
    const uint32_t a = arg(ctx, 0), info = arg(ctx, 1);
    if (!info) return ret(ctx, 0x80020005u);
    const std::lock_guard<std::mutex> lock(g_mb_mutex);
    for (auto &[uid, mb] : g_memblocks)
        if (a - mb.base < mb.size) {
            wr32(info, 0x18), wr32(info + 4, mb.base), wr32(info + 8, mb.size);
            wr32(info + 12, (mb.type & 0x00F00000u) == 0x00400000u ? 0x40 : 0x20); // CDRAM : RAM do usuário
            wr32(info + 16, 6); // leitura e escrita
            wr32(info + 20, mb.type);
            return ret(ctx, SCE_OK);
        }
    ret(ctx, 0x80020024u);
}
VT_HLE(sceKernelIsPSVitaTV) { ret(ctx, 0); }

// ====================================================================== tempo
VT_HLE(sceKernelGetProcessTimeWide) { ret64(ctx, now_us()); }
VT_HLE(sceKernelGetProcessTimeLow) { ret(ctx, (uint32_t)now_us()); }
VT_HLE(sceKernelGetProcessTime) { wr64(arg(ctx, 0), now_us()); ret(ctx, SCE_OK); }
VT_HLE(sceKernelGetSystemTimeWide) { ret64(ctx, now_us()); }
VT_HLE(sceKernelLibcClock) { ret(ctx, (uint32_t)now_us()); }
VT_HLE(sceKernelLibcTime) {
    const uint32_t t = (uint32_t)std::time(nullptr);
    if (arg(ctx, 0)) wr32(arg(ctx, 0), t);
    ret(ctx, t);
}
VT_HLE(sceKernelLibcGettimeofday) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    if (arg(ctx, 0)) {
        wr32(arg(ctx, 0), (uint32_t)(us / 1000000));
        wr32(arg(ctx, 0) + 4, (uint32_t)(us % 1000000));
    }
    ret(ctx, SCE_OK);
}
VT_HLE(sceKernelPowerTick) { ret(ctx, SCE_OK); }
VT_HLE(sceRtcGetTickResolution) { ret(ctx, 1000000); }
VT_HLE(sceRtcGetCurrentTick) {
    // ticks em microssegundos desde 0001-01-01 (como o Vita); 62135596800 s até a época Unix.
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now).count() + 62135596800ull * 1000000ull;
    wr64(arg(ctx, 0), us);
    ret(ctx, SCE_OK);
}

// ==================================================================== processo
VT_HLE(sceKernelGetProcessParam) {
    const arc_module_info *mi = main_module();
    uint32_t pp = 0;
    for (unsigned i = 0; mi && i < mi->export_count; i++)
        if (mi->exports[i].nid == 0x70FBA1E7u) pp = mi->exports[i].addr;
    ret(ctx, pp);
}
VT_HLE(sceKernelGetStdin) { ret(ctx, 0x00010000u); }
VT_HLE(sceKernelGetStdout) { ret(ctx, 0x00010001u); }
VT_HLE(sceKernelGetStderr) { ret(ctx, 0x00010002u); }
VT_HLE(sceKernelExitProcess) {
    logf("vitart: sceKernelExitProcess(%d)", (int)arg(ctx, 0));
    fflush(stdout);
    _exit((int)arg(ctx, 0));
}
VT_HLE(sceKernelCallModuleExit) {
    logf("vitart: sceKernelCallModuleExit");
    _exit(0);
}
VT_HLE(__stack_chk_fail) { fatal("__stack_chk_fail: estouro de pilha detectado pelo jogo (lr=0x%08x)", ctx->r[14]); }
VT_HLE(__sce_aeabi_idiv0) { ret(ctx, 0); }
VT_HLE(__sce_aeabi_ldiv0) { ret64(ctx, 0); }
VT_HLE(sceKernelCallAbortHandler) { fatal("sceKernelCallAbortHandler (abort do jogo, lr=0x%08x)", ctx->r[14]); }
VT_HLE(sceDbgAssertionHandler) {
    // (file, line, stop, component, msg, ...)
    logf("vitart: assert do jogo: %s:%u %s", gstr(arg(ctx, 0)), arg(ctx, 1), gstr(arg(ctx, 4)));
    ret(ctx, 0);
}

// ================================================================ sysmodule
VT_HLE(sceSysmoduleLoadModule) {
    logf("vitart: sceSysmoduleLoadModule(0x%04x)", arg(ctx, 0));
    ret(ctx, SCE_OK);
}
VT_HLE(sceSysmoduleIsLoaded) { ret(ctx, SCE_OK); }
VT_HLE(sceAppMgrIsGameProgram) { ret(ctx, SCE_OK); }
VT_HLE(sceAppMgrLoadExec) {
    // (appPath, argv, optParam): "app0:gow1.self" -> gow1.dll; "app0:eboot.bin" -> eboot.dll.
    std::string path = gstr(arg(ctx, 0));
    logf("vitart: sceAppMgrLoadExec(%s)", path.c_str());
    const size_t s = path.find_last_of(":/\\");
    std::string base = s == std::string::npos ? path : path.substr(s + 1);
    base = base.substr(0, base.find('.'));
    const std::string dll = base + ".dll";
    if (FILE *f = fopen(dll.c_str(), "rb")) {
        fclose(f);
        exec_main_module(dll);
    }
    logf("vitart: LoadExec: %s não existe", dll.c_str());
    ret(ctx, 0x80802013u); // SCE_APPMGR_ERROR_INVALID_SELF_PATH (aprox.)
}
VT_HLE(sceSysmoduleUnloadModule) { ret(ctx, SCE_OK); }
VT_HLE(scePowerSetConfigurationMode) { ret(ctx, SCE_OK); }

} // namespace vt
