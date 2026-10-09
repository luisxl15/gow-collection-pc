/* Linux ARM EABI personality for differential tests against qemu-arm. */
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include "arc.h"

void *arc_platform_reserve_4g(void) {
  void *p = mmap(NULL, 1ull << 32, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}
long arc_platform_write(int fd, const void *buf, uint32_t len) {
  ssize_t r = write(fd, buf, len);
  return r < 0 ? -9 : r;
}
void arc_platform_exit(int code) { exit(code); }
void arc_platform_log(const char *msg) { fputs(msg, stderr); }

void arc_svc(arc_ctx *ctx, uint32_t imm, uint32_t pc) {
  (void)imm, (void)pc;
  uint32_t nr = ctx->r[7], a0 = ctx->r[0], a1 = ctx->r[1], a2 = ctx->r[2];
  switch (nr) {
    case 1:   /* exit */
    case 248: /* exit_group */
      arc_platform_exit((int)a0);
    case 4:   /* write */
      ctx->r[0] = (uint32_t)arc_platform_write((int)a0, arc_mem + a1, a2);
      return;
    default:
      ctx->r[0] = (uint32_t)-38; /* -ENOSYS, as qemu-arm for the platform syscalls */
      return;
  }
}

void arc_import(arc_ctx *ctx, uint32_t nid, uint32_t pc) {
  (void)ctx;
  fprintf(stderr, "arc: import NID %08x called at %08x (no HLE in Linux personality)\n", nid, pc);
  exit(134);
}

/* ARC_INIT="r0 r1 ... r14" (hex): start from the same registers as qemu-arm (lockstep). */
int arc_platform_initial_state(arc_ctx *ctx) {
  const char *s = getenv("ARC_INIT");
  if (!s) return 0;
  for (int i = 0; i < 15 && *s; i++) ctx->r[i] = (uint32_t)strtoul(s, (char **)&s, 16);
  return 1;
}

int main(void) { return arc_run(); }
