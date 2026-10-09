/* Portable runtime for ARM code recompiled by vitarecomp. */
#include "arc.h"

uint8_t *arc_mem;
static arc_ctx main_ctx;

static void log_hex(const char *prefix, uint32_t v, const char *suffix) {
  char buf[128];
  unsigned n = 0;
  while (*prefix && n < 80) buf[n++] = *prefix++;
  buf[n++] = '0';
  buf[n++] = 'x';
  for (int s = 28; s >= 0; s -= 4) buf[n++] = "0123456789abcdef"[(v >> s) & 15];
  while (*suffix && n < sizeof(buf) - 1) buf[n++] = *suffix++;
  buf[n] = 0;
  arc_platform_log(buf);
}

void arc_trap(arc_ctx *ctx, uint32_t pc) {
  (void)ctx;
  log_hex("arc: trap (udf/bkpt/bad table index) at ", pc, "\n");
  arc_platform_exit(134);
}

void arc_unimpl(arc_ctx *ctx, uint32_t pc) {
  (void)ctx;
  log_hex("arc: instruction not translated at ", pc, "\n");
  arc_platform_exit(134);
}

void arc_dispatch(arc_ctx *ctx, uint32_t target, uint32_t from) {
  unsigned lo = 0, hi = arc_func_count;
  while (lo < hi) {
    unsigned mid = (lo + hi) / 2;
    uint32_t k = arc_func_table[mid].key;
    if (k == target) {
      arc_func_table[mid].fn(ctx);
      return;
    }
    if (k < target) lo = mid + 1;
    else hi = mid;
  }
  log_hex("arc: indirect branch to unknown code ", target, "");
  log_hex(" from ", from, "\n");
  arc_platform_exit(134);
}

/* ---- VFP helpers ---- */
static int snan_s(uint32_t b) { return (b & 0x7fc00000u) == 0x7f800000u && (b & 0x003fffffu); }
static int nan_s(uint32_t b) { return (b & 0x7fffffffu) > 0x7f800000u; }
static int snan_d(uint64_t b) { return (b & 0x7ff8000000000000ull) == 0x7ff0000000000000ull && (b & 0x0007ffffffffffffull); }
static int nan_d(uint64_t b) { return (b & ~(1ull << 63)) > 0x7ff0000000000000ull; }

float arc_nan_s(float a, float b, int nargs) {
  uint32_t x = arc_fbits(a), y = arc_fbits(b);
  if (snan_s(x)) return arc_bitsf(x | 0x00400000u);
  if (nargs > 1 && snan_s(y)) return arc_bitsf(y | 0x00400000u);
  if (nan_s(x)) return a;
  if (nargs > 1 && nan_s(y)) return b;
  return arc_bitsf(0x7fc00000u);
}

double arc_nan_d(double a, double b, int nargs) {
  uint64_t x = arc_dbits(a), y = arc_dbits(b);
  if (snan_d(x)) return arc_bitsd(x | 0x0008000000000000ull);
  if (nargs > 1 && snan_d(y)) return arc_bitsd(y | 0x0008000000000000ull);
  if (nan_d(x)) return a;
  if (nargs > 1 && nan_d(y)) return b;
  return arc_bitsd(0x7ff8000000000000ull);
}

double arc_cvt_d_s(float a) {
  uint32_t x = arc_fbits(a);
  if (!nan_s(x)) return (double)a;
  return arc_bitsd(((uint64_t)(x >> 31) << 63) | 0x7ff8000000000000ull | ((uint64_t)(x & 0x003fffffu) << 29));
}

float arc_cvt_s_d(double a) {
  uint64_t x = arc_dbits(a);
  if (!nan_d(x)) return (float)a;
  return arc_bitsf((uint32_t)(x >> 63) << 31 | 0x7fc00000u | (uint32_t)((x & 0x0007ffffffffffffull) >> 29));
}

/* rmode: 0 nearest-even, 1 +inf, 2 -inf, 3 zero. NaN -> 0, saturating. */
static double arc_round(double v, unsigned rmode) {
  double t = (double)(int64_t)v; /* |v| < 2^63 guaranteed by callers */
  double frac = v - t;
  switch (rmode & 3) {
    case 0:
      if (frac > 0.5 || (frac == 0.5 && ((int64_t)t & 1))) t += 1;
      else if (frac < -0.5 || (frac == -0.5 && ((int64_t)t & 1))) t -= 1;
      break;
    case 1: if (frac > 0) t += 1; break;
    case 2: if (frac < 0) t -= 1; break;
    default: break;
  }
  return t;
}

uint32_t arc_f2s32(double v, unsigned rmode) {
  if (v != v) return 0;
  if (v >= 2147483648.0) return 0x7fffffffu;
  if (v <= -2147483649.0) return 0x80000000u;
  double r = arc_round(v, rmode);
  if (r > 2147483647.0) return 0x7fffffffu;
  if (r < -2147483648.0) return 0x80000000u;
  return (uint32_t)(int32_t)r;
}

uint32_t arc_f2u32(double v, unsigned rmode) {
  if (v != v || v <= -1.0) return 0;
  if (v >= 4294967296.0) return 0xffffffffu;
  double r = arc_round(v, rmode);
  if (r < 0) return 0;
  if (r > 4294967295.0) return 0xffffffffu;
  return (uint32_t)r;
}

uint32_t arc_f2s16(double v, unsigned rmode) {
  if (v != v) return 0;
  double r = v >= 32768.0 ? 32767.0 : v <= -32769.0 ? -32768.0 : arc_round(v, rmode);
  if (r > 32767.0) r = 32767.0;
  if (r < -32768.0) r = -32768.0;
  return (uint32_t)(int32_t)r;
}

uint32_t arc_f2u16(double v, unsigned rmode) {
  if (v != v || v <= -1.0) return 0;
  double r = v >= 65536.0 ? 65535.0 : arc_round(v, rmode);
  if (r < 0) r = 0;
  if (r > 65535.0) r = 65535.0;
  return (uint32_t)r;
}

uint32_t arc_vcmp_flags(double a, double b) {
  if (a != a || b != b) return 0x30000000u; /* C V */
  if (a == b) return 0x60000000u;           /* Z C */
  if (a < b) return 0x80000000u;            /* N */
  return 0x20000000u;                       /* C */
}

/* One line per instruction: pc r0..r14 nzcv fpscr d0..d31 (hex). */
void arc_trace_insn(arc_ctx *ctx, uint32_t pc) {
  static const char hx[] = "0123456789abcdef";
  char buf[2 + 9 * 18 + 17 * 32 + 2];
  unsigned n = 0;
  uint32_t v[18];
  v[0] = pc;
  for (int i = 0; i < 15; i++) v[1 + i] = ctx->r[i];
  v[16] = ctx->n << 31 | ctx->z << 30 | ctx->c << 29 | ctx->v << 28;
  v[17] = ctx->fpscr;
  buf[n++] = 'T';
  for (int i = 0; i < 18; i++) {
    buf[n++] = ' ';
    for (int s = 28; s >= 0; s -= 4) buf[n++] = hx[(v[i] >> s) & 15];
  }
  for (int i = 0; i < 32; i++) {
    buf[n++] = ' ';
    for (int s = 60; s >= 0; s -= 4) buf[n++] = hx[(ctx->d[i] >> s) & 15];
  }
  buf[n++] = '\n';
  buf[n] = 0;
  arc_platform_log(buf);
}

int arc_run(void) {
  arc_mem = arc_platform_reserve_4g();
  if (!arc_mem) {
    arc_platform_log("arc: cannot reserve guest address space\n");
    return 1;
  }
  for (unsigned s = 0; s < arc_segment_count; s++) {
    const arc_segment *seg = &arc_segments[s];
    for (uint32_t i = 0; i < seg->filesz; i++) arc_mem[seg->vaddr + i] = seg->data[i];
  }
  arc_ctx *ctx = &main_ctx;
  ctx->r[13] = 0xE0000000u - 0x1000u;
  arc_platform_initial_state(ctx);
  arc_dispatch(ctx, arc_entry, 0);
  arc_platform_log("arc: entry returned\n");
  return (int)ctx->r[0];
}
