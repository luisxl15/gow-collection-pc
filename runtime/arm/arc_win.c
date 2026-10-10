/* Windows host personality for the ARM recompiler tests: the same Linux ARM EABI syscalls as
 * arc_linux.c (write/exit), so guest programs built for qemu-arm/Unicorn run unchanged.
 *
 * Guest memory: the flat 4 GiB window is only RESERVED (committing it would cost 4 GB of commit
 * charge); pages are committed on first touch by a vectored exception handler, in 64 KiB steps. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <io.h>
#include <fcntl.h>
#include "arc.h"

static uint8_t *g_base;
#define COMMIT_STEP (64u * 1024u)

static LONG CALLBACK commit_on_fault(EXCEPTION_POINTERS *ep) {
  EXCEPTION_RECORD *er = ep->ExceptionRecord;
  if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
  uint8_t *addr = (uint8_t *)er->ExceptionInformation[1];
  if (!g_base || addr < g_base || addr >= g_base + (1ull << 32)) return EXCEPTION_CONTINUE_SEARCH;
  uint8_t *page = g_base + (((uint64_t)(addr - g_base)) & ~(uint64_t)(COMMIT_STEP - 1));
  if (!VirtualAlloc(page, COMMIT_STEP, MEM_COMMIT, PAGE_READWRITE)) return EXCEPTION_CONTINUE_SEARCH;
  return EXCEPTION_CONTINUE_EXECUTION;
}

void *arc_platform_reserve_4g(void) {
  g_base = VirtualAlloc(NULL, 1ull << 32, MEM_RESERVE, PAGE_NOACCESS);
  if (!g_base) return NULL;
  AddVectoredExceptionHandler(1, commit_on_fault);
  return g_base;
}
long arc_platform_write(int fd, const void *buf, uint32_t len) {
  int r = _write(fd, buf, len);
  return r < 0 ? -9 : r;
}
void arc_platform_exit(int code) {
  fflush(stdout);
  _exit(code);
}
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
      ctx->r[0] = (uint32_t)-38; /* -ENOSYS */
      return;
  }
}

void arc_import(arc_ctx *ctx, uint32_t nid, uint32_t pc) {
  (void)ctx;
  fprintf(stderr, "arc: import NID %08x called at %08x (no HLE in the test personality)\n", nid, pc);
  _exit(134);
}

int arc_platform_initial_state(arc_ctx *ctx) {
  const char *s = getenv("ARC_INIT");
  if (!s) return 0;
  for (int i = 0; i < 15 && *s; i++) ctx->r[i] = (uint32_t)strtoul(s, (char **)&s, 16);
  return 1;
}

int main(void) {
  _setmode(1, _O_BINARY); /* the tests compare raw bytes written to stdout */
  _setmode(2, _O_BINARY);
  return arc_run();
}

/* Nos testes, um retorno para outro lugar que não o chamador é erro: os guests não usam longjmp. */
void arc_ret_mismatch(arc_ctx *ctx, uint32_t expected) {
  fprintf(stderr, "arc: return to %08x, expected %08x\n", ctx->r[15], expected);
  arc_platform_exit(134);
}
