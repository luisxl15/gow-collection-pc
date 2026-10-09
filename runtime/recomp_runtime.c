/*
 * Portable runtime for mipsrecomp output. Freestanding: depends only on the
 * rc_platform_* layer, so the same file builds for Linux hosts and the Vita.
 *
 * Where MIPS leaves results UNPREDICTABLE (divide by zero, invalid FP->int),
 * we reproduce QEMU's behaviour, because QEMU is the reference oracle used by
 * the differential tests. Real-hardware quirks can be layered on per game.
 */
#include "recomp.h"

uint8_t *rc_mem;

#define ENOSYS_MIPS 89

static void log_hex(const char *prefix, uint32_t v, const char *suffix) {
  char buf[96];
  unsigned n = 0;
  while (*prefix && n < 60) buf[n++] = *prefix++;
  buf[n++] = '0';
  buf[n++] = 'x';
  for (int s = 28; s >= 0; s -= 4) buf[n++] = "0123456789abcdef"[(v >> s) & 15];
  while (*suffix && n < sizeof(buf) - 1) buf[n++] = *suffix++;
  buf[n] = 0;
  rc_platform_log(buf);
}

static void fatal(const char *what, uint32_t pc) {
  log_hex(what, pc, "\n");
  rc_platform_exit(128 + 6);
}

void rc_overflow(rc_ctx *ctx, uint32_t pc) { (void)ctx; fatal("rc: integer overflow trap at ", pc); for (;;) {} }
void rc_trap(rc_ctx *ctx, uint32_t pc) { (void)ctx; fatal("rc: trap instruction at ", pc); for (;;) {} }
void rc_break(rc_ctx *ctx, uint32_t pc, uint32_t code) {
  (void)ctx;
  log_hex("rc: break code ", code, "");
  fatal(" at ", pc);
  for (;;) {}
}
void rc_unknown(rc_ctx *ctx, uint32_t pc, uint32_t word) {
  (void)ctx;
  log_hex("rc: undecodable instruction ", word, "");
  fatal(" at ", pc);
  for (;;) {}
}
void rc_fallthrough(rc_ctx *ctx, uint32_t pc) { (void)ctx; fatal("rc: fell through into non-function code at ", pc); for (;;) {} }

void rc_dispatch(rc_ctx *ctx, uint32_t target, uint32_t from) {
  unsigned lo = 0, hi = rc_func_count;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    uint32_t a = rc_func_table[mid].addr;
    if (a == target) {
      rc_func_table[mid].fn(ctx);
      return;
    }
    if (a < target) lo = mid + 1;
    else hi = mid;
  }
  log_hex("rc: indirect branch to unknown address ", target, "");
  fatal(" from ", from);
}

/* QEMU (target/mips/tcg/translate.c): a zero divisor, or INT_MIN / -1, is replaced by 1. */
void rc_div(rc_ctx *ctx, uint32_t a, uint32_t b) {
  int32_t x = (int32_t)a, y = (int32_t)b;
  if (y == 0 || (x == INT32_MIN && y == -1)) y = 1;
  ctx->lo = (uint32_t)(x / y);
  ctx->hi = (uint32_t)(x % y);
}

void rc_divu(rc_ctx *ctx, uint32_t a, uint32_t b) {
  if (b == 0) b = 1;
  ctx->lo = a / b;
  ctx->hi = a % b;
}

/* FP -> int32 with MIPS rounding modes (0 RN, 1 RZ, 2 RP, 3 RM).
 * Invalid / out of range -> 0x7fffffff (legacy MIPS FP_TO_INT32_OVERFLOW). */
int32_t rc_to_w(double v, unsigned mode) {
  if (v != v || v >= 2147483648.0 || v <= -2147483649.0) return 0x7fffffff;
  int64_t t = (int64_t)v; /* truncation toward zero */
  double frac = v - (double)t;
  switch (mode & 3) {
    case 0:
      if (frac > 0.5 || (frac == 0.5 && (t & 1))) t++;
      else if (frac < -0.5 || (frac == -0.5 && (t & 1))) t--;
      break;
    case 1:
      break;
    case 2:
      if (frac > 0) t++;
      break;
    case 3:
      if (frac < 0) t--;
      break;
  }
  if (t > INT32_MAX || t < INT32_MIN) return 0x7fffffff;
  return (int32_t)t;
}

/* Legacy (pre-NaN2008) MIPS under QEMU runs softfloat in default-NaN mode:
 * every NaN produced by arithmetic or format conversion is the default NaN,
 * regardless of the input NaN's sign or payload (verified by lockstep fuzzing). */
float rc_nan_pick_s(float a, float b, int nargs) {
  (void)a, (void)b, (void)nargs;
  return rc_bitsf(RC_DNAN_S);
}

double rc_nan_pick_d(double a, double b, int nargs) {
  (void)a, (void)b, (void)nargs;
  return rc_bitsd(RC_DNAN_D);
}

double rc_cvt_d_s(float a) { return a == a ? (double)a : rc_bitsd(RC_DNAN_D); }

float rc_cvt_s_d(double a) { return a == a ? (float)a : rc_bitsf(RC_DNAN_S); }

uint32_t rc_cfc1(rc_ctx *ctx, int reg) {
  if (reg == 31) return ctx->fcr31;
  if (reg == 0) return 0x00739300u; /* FIR: has S, D, W, L, F64=0 */
  return 0;
}

void rc_ctc1(rc_ctx *ctx, int reg, uint32_t v) {
  if (reg == 31) ctx->fcr31 = v & 0x0183FFFFu;
}

void rc_trace_enter(rc_ctx *ctx, uint32_t addr) {
  (void)ctx;
  log_hex("rc-trace: ", addr, "\n");
}

/* One line per instruction: pc hi lo r1..r31 fcr31 f0..f31 (all hex). */
void rc_trace_insn(rc_ctx *ctx, uint32_t pc) {
  char buf[8 + 9 * 67 + 2];
  unsigned n = 0;
  uint32_t vals[67];
  unsigned k = 0;
  vals[k++] = pc;
  vals[k++] = ctx->hi;
  vals[k++] = ctx->lo;
  for (int i = 1; i < 32; i++) vals[k++] = ctx->r[i];
  vals[k++] = ctx->fcr31;
  for (int i = 0; i < 32; i++) vals[k++] = ctx->f[i];
  buf[n++] = 'T';
  for (unsigned i = 0; i < k; i++) {
    buf[n++] = ' ';
    for (int s = 28; s >= 0; s -= 4) buf[n++] = "0123456789abcdef"[(vals[i] >> s) & 15];
  }
  buf[n++] = '\n';
  buf[n] = 0;
  rc_platform_log(buf);
}

static void sys_ret(rc_ctx *ctx, long r) {
  if (r < 0) {
    ctx->r[2] = (uint32_t)-r;
    ctx->r[7] = 1;
  } else {
    ctx->r[2] = (uint32_t)r;
    ctx->r[7] = 0;
  }
}

void rc_syscall(rc_ctx *ctx) {
  uint32_t a0 = ctx->r[4], a1 = ctx->r[5], a2 = ctx->r[6];
  switch (ctx->r[2]) {
    case 4001: /* exit */
    case 4246: /* exit_group */
      rc_platform_exit((int)a0);
    case 4004: /* write */
      if (a1 + a2 > RC_MEM_SIZE || a1 + a2 < a1) {
        sys_ret(ctx, -14); /* EFAULT */
        return;
      }
      sys_ret(ctx, rc_platform_write((int)a0, rc_mem + a1, a2));
      return;
    case 4900: { /* plat_present(fb, w, h) */
      long r = -ENOSYS_MIPS;
      if (a1 && a2 && a1 <= 4096 && a2 <= 4096 && a0 + a1 * a2 * 4 <= RC_MEM_SIZE)
        r = rc_platform_present(rc_mem + a0, a1, a2);
      sys_ret(ctx, r < 0 ? -ENOSYS_MIPS : r);
      return;
    }
    case 4901: { /* plat_buttons */
      long r = rc_platform_buttons();
      sys_ret(ctx, r < 0 ? -ENOSYS_MIPS : r);
      return;
    }
    case 4902: { /* plat_ticks */
      long r = rc_platform_ticks_us();
      sys_ret(ctx, r < 0 ? -ENOSYS_MIPS : r);
      return;
    }
    default:
      log_hex("rc: unimplemented syscall ", ctx->r[2], "");
      log_hex(" at ", ctx->pc, "\n");
      sys_ret(ctx, -ENOSYS_MIPS);
      return;
  }
}

static rc_ctx main_ctx;

int rc_run(void) {
  rc_mem = rc_platform_alloc_guest_ram(RC_MEM_SIZE);
  if (!rc_mem) {
    rc_platform_log("rc: cannot allocate guest RAM\n");
    return 1;
  }
  for (uint32_t i = 0; i < RC_MEM_SIZE; i += 4) *(uint32_t *)(rc_mem + i) = 0;
  for (unsigned s = 0; s < rc_segment_count; s++) {
    const rc_segment *seg = &rc_segments[s];
    for (uint32_t i = 0; i < seg->filesz; i++) rc_mem[RC_ADDR(seg->vaddr + i)] = seg->data[i];
  }
  rc_ctx *ctx = &main_ctx;
  ctx->r[29] = RC_STACK_TOP;
  ctx->r[31] = 0;
  rc_dispatch(ctx, rc_entry, 0);
  rc_platform_log("rc: entry function returned\n");
  return (int)ctx->r[2];
}
