/* Integer ALU, shifts, multiply/divide, set-less-than, sign/zero extension. */
#include "../../guest/glibc.h"

static volatile s32 vals[] = {0, 1, -1, 2, -2, 7, -7, 0x7fffffff, (s32)0x80000000, 12345, -98765,
                              0x55555555, (s32)0xaaaaaaaa, 31, 32, 65535, -65536};
#define N (int)(sizeof(vals) / sizeof(vals[0]))

static u32 h = 2166136261u;
static void mix(u32 v) { h = g_fnv1a(&v, 4, h); }

__attribute__((noinline)) static s32 clz32(u32 x) {
  int n = 0;
  if (!x) return 32;
  while (!(x & 0x80000000u)) x <<= 1, n++;
  return n;
}

int main(void) {
  for (int i = 0; i < N; i++) {
    for (int j = 0; j < N; j++) {
      s32 a = vals[i], b = vals[j];
      u32 ua = a, ub = b;
      mix(a + b), mix(a - b), mix(a & b), mix(a | b), mix(a ^ b), mix(~(a | b));
      mix(a < b), mix(ua < ub), mix(a < 100), mix(ua < 100u);
      mix(ua << (b & 31)), mix(ua >> (b & 31)), mix((u32)(a >> (b & 31)));
      mix(a * b);
      s64 wide = (s64)a * b;
      u64 uwide = (u64)ua * ub;
      mix((u32)wide), mix((u32)(wide >> 32)), mix((u32)uwide), mix((u32)(uwide >> 32));
      if (b != 0 && !(a == (s32)0x80000000 && b == -1)) {
        mix(a / b), mix(a % b), mix(ua / ub), mix(ua % ub);
      }
      mix((s32)(s8)a), mix((s32)(s16)a), mix((u32)(u8)a), mix((u32)(u16)a);
    }
    mix(vals[i] << 3), mix((u32)vals[i] >> 7), mix(vals[i] >> 9);
    mix(clz32(vals[i])), mix(__builtin_clz(vals[i] | 1));
    mix(__builtin_bswap32(vals[i]));
  }
  g_printf("t_arith hash=%08x\n", h);
  g_printf("signed: %d %d %d unsigned: %u %x\n", vals[8] / 3, -17 % 5, vals[10] >> 4,
           (u32)vals[10] >> 4, (u32)vals[12]);
  return 0;
}
