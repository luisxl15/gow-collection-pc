/* VCVTB/VCVTT (meia precisão <-> simples) em todas as combinações de FPSCR.AHP/DN/FZ/RMode.
 * Escreve os resultados brutos em stdout: o diferencial compara byte a byte com o oráculo. */
#include "../../guest/glibc.h"

static const u32 f32s[] = {
  0x00000000, 0x80000000, 0x3f800000, 0xbf800000, 0x3f000000, 0x477fe000, 0x477ff000, 0x477fefff,
  0x47800000, 0xc7800000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7f800001, 0xffc12345, 0x7fbfffff,
  0x38800000, 0x38000000, 0x33800000, 0x33000000, 0x33000001, 0x337fffff, 0x387fc000, 0x387fe000,
  0x00000001, 0x807fffff, 0x3dcccccd, 0x40490fdb, 0xc2f6e979, 0x3effffff, 0x3f7fffff, 0x46fffe00,
  0x3a800100, 0x3a800180, 0x3a800080, 0xb3c00000, 0x34000000, 0x337ffffe, 0x7f7fffff, 0xff7fffff,
};
static const u32 f16s[] = {
  0x0000, 0x8000, 0x3c00, 0xbc00, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e00, 0x7d01, 0xfe55, 0x7fff,
  0x0001, 0x03ff, 0x0400, 0x8001, 0x83ff, 0x3555, 0x4248, 0xc248, 0x7c01, 0x1234, 0xabcd, 0x5a5a,
};
#define N(a) (int)(sizeof(a) / sizeof(a[0]))

static u32 out[8192];
static int n;

__attribute__((noinline)) static u32 cvt_b_f16(u32 fpscr, u32 x, u32 keep) {
  u32 r;
  __asm__ volatile(
      "vmsr fpscr, %1\n\t"
      "vmov s0, %2\n\t"
      "vmov s1, %3\n\t"
      "vcvtb.f16.f32 s1, s0\n\t"
      "vmov %0, s1\n\t"
      : "=r"(r) : "r"(fpscr), "r"(x), "r"(keep) : "s0", "s1");
  return r;
}
__attribute__((noinline)) static u32 cvt_t_f16(u32 fpscr, u32 x, u32 keep) {
  u32 r;
  __asm__ volatile(
      "vmsr fpscr, %1\n\t"
      "vmov s0, %2\n\t"
      "vmov s1, %3\n\t"
      "vcvtt.f16.f32 s1, s0\n\t"
      "vmov %0, s1\n\t"
      : "=r"(r) : "r"(fpscr), "r"(x), "r"(keep) : "s0", "s1");
  return r;
}
__attribute__((noinline)) static u32 cvt_b_f32(u32 fpscr, u32 x) {
  u32 r;
  __asm__ volatile(
      "vmsr fpscr, %1\n\t"
      "vmov s0, %2\n\t"
      "vcvtb.f32.f16 s1, s0\n\t"
      "vmov %0, s1\n\t"
      : "=r"(r) : "r"(fpscr), "r"(x) : "s0", "s1");
  return r;
}
__attribute__((noinline)) static u32 cvt_t_f32(u32 fpscr, u32 x) {
  u32 r;
  __asm__ volatile(
      "vmsr fpscr, %1\n\t"
      "vmov s0, %2\n\t"
      "vcvtt.f32.f16 s1, s0\n\t"
      "vmov %0, s1\n\t"
      : "=r"(r) : "r"(fpscr), "r"(x) : "s0", "s1");
  return r;
}

int main(void) {
  for (u32 mode = 0; mode < 32; mode++) {
    /* RMode (2 bits) x FZ x DN x AHP: FPSCR bits 23:22, 24, 25, 26 */
    u32 fpscr = (mode & 31u) << 22;
    for (int i = 0; i < N(f32s); i++) {
      out[n++] = cvt_b_f16(fpscr, f32s[i], 0xa5a5c3c3u);
      out[n++] = cvt_t_f16(fpscr, f32s[i], 0x5a5a3c3cu);
    }
    for (int i = 0; i < N(f16s); i++) {
      out[n++] = cvt_b_f32(fpscr, f16s[i] | 0xdead0000u);
      out[n++] = cvt_t_f32(fpscr, (f16s[i] << 16) | 0xbeefu);
    }
  }
  g_write(1, out, (size_t)n * 4);
  return 0;
}
