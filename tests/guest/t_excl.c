/* LDREX/STREX (8/16/32/64 bits), CLREX e DMB: semântica de uma thread, contra o oráculo.
 * STREX tem de falhar sem LDREX prévio, depois de CLREX, com endereço ou tamanho diferente. */
#include "../../guest/glibc.h"

static volatile u32 w[4] = {0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u};
static volatile u16 h[2] = {0x5555, 0x6666};
static volatile u8 b[4] = {0x77, 0x88, 0x99, 0xaa};
static volatile u64 q[2] = {0x0123456789abcdefull, 0xfedcba9876543210ull};
static u32 out[64];
static int n;

__attribute__((noinline)) static u32 xadd32(volatile u32 *p, u32 v) {
  u32 old, tmp, fail;
  __asm__ volatile("1: ldrex %0, [%3]\n\t"
                   "add %1, %0, %4\n\t"
                   "strex %2, %1, [%3]\n\t"
                   "cmp %2, #0\n\t"
                   "bne 1b\n\t"
                   "dmb ish"
                   : "=&r"(old), "=&r"(tmp), "=&r"(fail) : "r"(p), "r"(v) : "cc", "memory");
  return old;
}
__attribute__((noinline)) static u32 strex_alone(volatile u32 *p, u32 v) {
  u32 fail;
  __asm__ volatile("clrex\n\tstrex %0, %2, [%1]" : "=&r"(fail) : "r"(p), "r"(v) : "memory");
  return fail;
}
__attribute__((noinline)) static u32 strex_after_clrex(volatile u32 *p, u32 v) {
  u32 fail, t;
  __asm__ volatile("ldrex %1, [%2]\n\tclrex\n\tstrex %0, %3, [%2]" : "=&r"(fail), "=&r"(t) : "r"(p), "r"(v) : "memory");
  return fail;
}
__attribute__((noinline)) static u32 strex_other_addr(volatile u32 *p, volatile u32 *o, u32 v) {
  u32 fail, t;
  __asm__ volatile("ldrex %1, [%2]\n\tstrex %0, %4, [%3]" : "=&r"(fail), "=&r"(t) : "r"(p), "r"(o), "r"(v) : "memory");
  return fail;
}
__attribute__((noinline)) static u32 xchg8(volatile u8 *p, u32 v, u32 *old) {
  u32 fail, t;
  __asm__ volatile("ldrexb %1, [%2]\n\tstrexb %0, %3, [%2]" : "=&r"(fail), "=&r"(t) : "r"(p), "r"(v) : "memory");
  *old = t;
  return fail;
}
__attribute__((noinline)) static u32 xchg16(volatile u16 *p, u32 v, u32 *old) {
  u32 fail, t;
  __asm__ volatile("ldrexh %1, [%2]\n\tstrexh %0, %3, [%2]" : "=&r"(fail), "=&r"(t) : "r"(p), "r"(v) : "memory");
  *old = t;
  return fail;
}
__attribute__((noinline)) static u32 xchg64(volatile u64 *p, u64 v, u64 *old) {
  u32 fail;
  u64 t;
  __asm__ volatile("ldrexd %1, %H1, [%2]\n\tstrexd %0, %3, %H3, [%2]" : "=&r"(fail), "=&r"(t) : "r"(p), "r"(v) : "memory");
  *old = t;
  return fail;
}

int main(void) {
  for (u32 k = 1; k < 6; k++) out[n++] = xadd32(&w[0], k * 0x01010101u);
  out[n++] = w[0];
  out[n++] = strex_alone(&w[1], 0xdeadbeefu);
  out[n++] = w[1];
  out[n++] = strex_after_clrex(&w[2], 0xcafef00du);
  out[n++] = w[2];
  out[n++] = strex_other_addr(&w[3], &w[2], 0x0badf00du);
  out[n++] = w[2];
  out[n++] = w[3];
  u32 o;
  out[n++] = xchg8(&b[1], 0x42, &o); out[n++] = o; out[n++] = b[1];
  out[n++] = xchg16(&h[1], 0x4243, &o); out[n++] = o; out[n++] = h[1];
  u64 o64;
  out[n++] = xchg64(&q[1], 0x1122334455667788ull, &o64);
  out[n++] = (u32)o64; out[n++] = (u32)(o64 >> 32); out[n++] = (u32)q[1]; out[n++] = (u32)(q[1] >> 32);
  g_write(1, out, (size_t)n * 4);
  return 0;
}
