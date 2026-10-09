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

/* ---- NEON reciprocal estimates (ARMv7 ARM pseudocode, 8-bit tables) ---- */
static int recip_est(int q) { /* q in 256..511 */
  double r = 1.0 / (((double)q + 0.5) / 512.0);
  return (int)(256.0 * r + 0.5);
}
static int rsqrt_est(int q, int lowrange) {
  double r = lowrange ? 1.0 / __builtin_sqrt(((double)q + 0.5) / 512.0) : 1.0 / __builtin_sqrt(((double)q + 0.5) / 256.0);
  return (int)(256.0 * r + 0.5);
}

float arc_recpe(float xf) {
  uint32_t op = arc_fbits(arc_nftz(xf)), sign = op & 0x80000000u, exp = (op >> 23) & 0xff;
  if ((op & 0x7fffffffu) > 0x7f800000u) return arc_bitsf(0x7fc00000u);
  if (exp == 0xff) return arc_bitsf(sign);
  if (exp == 0) return arc_bitsf(sign | 0x7f800000u);
  if (exp >= 253) return arc_bitsf(sign);
  int s = recip_est(256 + ((op & 0x7fffff) >> 15));
  return arc_bitsf(sign | ((253 - exp) << 23) | ((uint32_t)(s - 256) << 15));
}

float arc_rsqrte(float xf) {
  uint32_t op = arc_fbits(arc_nftz(xf)), exp = (op >> 23) & 0xff;
  if ((op & 0x7fffffffu) > 0x7f800000u) return arc_bitsf(0x7fc00000u);
  if ((op & 0x7fffffffu) == 0) return arc_bitsf((op & 0x80000000u) | 0x7f800000u);
  if (op & 0x80000000u) return arc_bitsf(0x7fc00000u);
  if (exp == 0xff) return arc_bitsf(0);
  /* even exponent: significand scaled to [0.5,1) (q/256); odd: [0.25,0.5) (q/512) */
  int s = rsqrt_est(128 + (int)((op & 0x7fffff) >> 16), (int)(exp & 1));
  return arc_bitsf(((uint32_t)((380 - exp) / 2) << 23) | ((uint32_t)(s - 256) << 15));
}

uint32_t arc_recpe_u32(uint32_t x) {
  if (!(x & 0x80000000u)) return 0xffffffffu;
  return 0x80000000u | ((uint32_t)(recip_est((int)(x >> 23)) - 256) << 23);
}

uint32_t arc_rsqrte_u32(uint32_t x) {
  if (!(x & 0xc0000000u)) return 0xffffffffu;
  int s = (x & 0x80000000u) ? rsqrt_est((int)(x >> 24), 0) : rsqrt_est((int)(x >> 23), 1);
  return 0x80000000u | ((uint32_t)(s - 256) << 23);
}

float arc_recps(float a, float b) {
  a = arc_nftz(a), b = arc_nftz(b);
  if (a != a || b != b) return arc_bitsf(0x7fc00000u);
  if ((__builtin_isinf(a) && b == 0) || (__builtin_isinf(b) && a == 0)) return 2.0f;
  return arc_nfix(2.0f - arc_nfix(a * b));
}

float arc_rsqrts(float a, float b) {
  a = arc_nftz(a), b = arc_nftz(b);
  if (a != a || b != b) return arc_bitsf(0x7fc00000u);
  if ((__builtin_isinf(a) && b == 0) || (__builtin_isinf(b) && a == 0)) return 1.5f;
  return arc_nfix(arc_nfix(3.0f - arc_nfix(a * b)) / 2.0f);
}

uint64_t arc_nshl(uint64_t x, int8_t sh, int n, int is_signed, int rounding, int saturating) {
  __int128 v = is_signed ? (__int128)(int64_t)(x << (64 - n)) >> (64 - n) : (__int128)(x & (n == 64 ? ~0ull : (1ull << n) - 1));
  __int128 r;
  if (sh >= 0) {
    r = sh >= 64 ? (v ? (v < 0 ? -((__int128)1 << 100) : ((__int128)1 << 100)) : 0) : v << sh;
  } else {
    int s = -sh;
    if (rounding) r = s > 64 ? 0 : (v + ((__int128)1 << (s - 1))) >> s;
    else r = s >= 64 ? (v < 0 ? -1 : 0) : v >> s;
  }
  if (saturating) {
    __int128 lo = is_signed ? -((__int128)1 << (n - 1)) : 0;
    __int128 hi = is_signed ? ((__int128)1 << (n - 1)) - 1 : (((__int128)1 << n) - 1);
    if (r < lo) r = lo;
    if (r > hi) r = hi;
  }
  return (uint64_t)r;
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
