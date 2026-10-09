/* Loads/stores of every width, unaligned access (lwl/lwr/swl/swr), 64-bit ops. */
#include "../../guest/glibc.h"

struct __attribute__((packed)) rec {
  u8 tag;
  u32 a;
  u16 b;
  s64 c;
  s16 d;
};

static struct rec recs[16];
static u8 buf[256];

__attribute__((noinline)) static u32 read_unaligned(const u8 *p) {
  u32 v;
  memcpy(&v, p, 4);
  return v;
}

__attribute__((noinline)) static u64 mul64(u64 a, u64 b) { return a * b; }
__attribute__((noinline)) static u64 div64(u64 a, u64 b) { return a / b; }
__attribute__((noinline)) static s64 sdiv64(s64 a, s64 b) { return a / b; }
__attribute__((noinline)) static s64 shl64(s64 a, int s) { return a << s; }
__attribute__((noinline)) static s64 sar64(s64 a, int s) { return a >> s; }

int main(void) {
  for (int i = 0; i < 256; i++) buf[i] = (u8)(i * 37 + 11);
  u32 h = 2166136261u;
  for (int i = 0; i < 16; i++) {
    recs[i].tag = (u8)i;
    recs[i].a = 0xdeadbeefu ^ (i * 0x01010101u);
    recs[i].b = (u16)(i * 4099);
    recs[i].c = -(s64)i * 0x123456789LL;
    recs[i].d = (s16)(-i * 1000);
  }
  for (int i = 0; i < 16; i++) {
    u32 v[] = {recs[i].a, recs[i].b, (u32)recs[i].c, (u32)(recs[i].c >> 32), (u32)recs[i].d};
    h = g_fnv1a(v, sizeof(v), h);
  }
  for (int off = 0; off < 8; off++) {
    u32 v = read_unaligned(buf + off);
    h = g_fnv1a(&v, 4, h);
    s16 sh = *(volatile s16 *)(buf + (off & ~1));
    s8 sb = *(volatile s8 *)(buf + off + 100);
    s32 w[2] = {sh, sb};
    h = g_fnv1a(w, 8, h);
  }
  memmove(buf + 3, buf, 100);
  memmove(buf, buf + 7, 100);
  h = g_fnv1a(buf, sizeof(buf), h);
  g_printf("t_mem hash=%08x\n", h);

  u64 x = 0x0123456789abcdefULL, y = 0xfedcba9876543ULL;
  g_printf("mul64=%llx div64=%llx sdiv64=%lld\n", mul64(x, y), div64(x, 12345), sdiv64(-(s64)x, 777));
  g_printf("shl=%llx sar=%lld\n", shl64(-12345, 40), sar64(-0x123456789LL, 13));
  return 0;
}
