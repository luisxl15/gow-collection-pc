/* Cola da biblioteca dinâmica: os serviços que o código gerado chama (arc_import, arc_dispatch_miss,
 * arc_ret_mismatch...) são encaminhados ao host registrado por arc_bind. A memória do guest é a do
 * próprio host (no Vita3K, a janela plana de 4 GiB de MemState). */
#define ARC_BUILD_DLL 1
#include "arc.h"
#include "arc_host.h"

static const arc_host_api *host;

#ifndef ARC_MODULE_NAME
#define ARC_MODULE_NAME "recomp"
#endif

int arc_bind(const arc_host_api *api, uint8_t *guest_mem) {
  if (!api || api->abi != ARC_HOST_ABI) return -1;
  host = api;
  arc_mem = guest_mem;
  return 0;
}

const arc_func_entry *arc_get_table(unsigned *count) {
  *count = arc_func_count;
  return arc_func_table;
}

const char *arc_get_module_name(void) { return ARC_MODULE_NAME; }

const arc_segment *arc_get_segments(unsigned *count) {
  *count = arc_segment_count;
  return arc_segments;
}

void arc_import(arc_ctx *ctx, uint32_t nid, uint32_t stub) { host->import_call(ctx, nid, stub); }
void arc_dispatch_miss(arc_ctx *ctx, uint32_t target, uint32_t from) { host->dispatch_miss(ctx, target, from); }
void arc_ret_mismatch(arc_ctx *ctx, uint32_t expected) {
  host->ret_mismatch(ctx, expected);
  __builtin_unreachable();
}
void arc_svc(arc_ctx *ctx, uint32_t imm, uint32_t pc) { host->svc(ctx, imm, pc); }

/* Plataforma: só o necessário para arc_runtime.c (arc_trap/arc_unimpl/log). */
void *arc_platform_reserve_4g(void) { return 0; }
long arc_platform_write(int fd, const void *buf, uint32_t len) { (void)fd, (void)buf; return (long)len; }
void arc_platform_exit(int code) {
  (void)code;
  host->fatal(0, 0, "arc_platform_exit");
  __builtin_unreachable();
}
void arc_platform_log(const char *msg) { host->log(msg); }
int arc_platform_initial_state(arc_ctx *ctx) { (void)ctx; return 0; }
