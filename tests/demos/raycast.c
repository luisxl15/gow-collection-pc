/*
 * "Guest game": a textured raycaster written for the MIPS guest, standing in
 * for a real console title. It exercises FP-heavy inner loops, integer
 * texture sampling, tables, a framebuffer, input and timing.
 *
 * Platform syscalls (see guest/glibc.h):
 *   - plat_buttons < 0 (qemu, host tests): deterministic scripted camera,
 *     DEMO_FRAMES frames, prints a per-frame FNV hash, exits.
 *   - plat_buttons >= 0 (PS Vita runtime): interactive until START.
 */
#include "../../guest/glibc.h"

#ifndef W
#define W 320
#endif
#ifndef H
#define H 240
#endif
#ifndef DEMO_FRAMES
#define DEMO_FRAMES 48
#endif
#define TEX 64
#define MAP_W 16
#define MAP_H 16

enum { B_UP = 1, B_DOWN = 2, B_LEFT = 4, B_RIGHT = 8, B_CROSS = 16, B_CIRCLE = 32, B_START = 64,
       B_L = 128, B_R = 256 };

static u32 fb[W * H];
static u32 tex[4][TEX * TEX];
static float sin_tab[1024];

static const char map[MAP_H][MAP_W + 1] = {
    "1111111111111111",
    "1000000000000001",
    "1022200000330001",
    "1020000000030001",
    "1020000400030001",
    "1000000400000001",
    "1000000000000001",
    "1003300000222001",
    "1000300000002001",
    "1000000044002001",
    "1040000000000001",
    "1040022200030001",
    "1000000000030001",
    "1000000000000001",
    "1000000000000001",
    "1111111111111111",
};

static float fsin(float a) { /* a in turns */
  int i = (int)(a * 1024.0f) & 1023;
  return sin_tab[i];
}
static float fcos(float a) { return fsin(a + 0.25f); }

static float sine_series(double x) {
  double term = x, sum = x;
  for (int n = 1; n < 10; n++) {
    term *= -x * x / ((2 * n) * (2 * n + 1));
    sum += term;
  }
  return (float)sum;
}

static void init_tables(void) {
  for (int i = 0; i < 1024; i++) {
    double a = (double)i / 1024.0 * 6.283185307179586;
    if (a > 3.141592653589793) a -= 6.283185307179586;
    sin_tab[i] = sine_series(a);
  }
  for (int t = 0; t < 4; t++)
    for (int y = 0; y < TEX; y++)
      for (int x = 0; x < TEX; x++) {
        u32 c;
        switch (t) {
          case 0: { /* bricks */
            int row = y / 8, off = (row & 1) * 8;
            int mortar = (y % 8 == 0) || ((x + off) % 16 == 0);
            c = mortar ? 0xffb0b0b0u : 0xff2030a0u + ((x * 7 + y * 13) & 15) * 0x010101u;
            break;
          }
          case 1: c = ((x ^ y) & 8) ? 0xff30a030u : 0xff206020u; break;          /* checker */
          case 2: c = 0xff000000u | (u32)(x * 4) << 16 | (u32)(y * 4) << 8 | 0x80; break; /* grad */
          default: { int d = (x - 32) * (x - 32) + (y - 32) * (y - 32);           /* rings */
            c = (d / 40) & 1 ? 0xff00c0e0u : 0xff004060u; }
        }
        tex[t][y * TEX + x] = c;
      }
}

static u32 shade(u32 c, int s) { /* s in 0..256 */
  u32 r = ((c & 0xff) * s) >> 8, g = (((c >> 8) & 0xff) * s) >> 8, b = (((c >> 16) & 0xff) * s) >> 8;
  return 0xff000000u | b << 16 | g << 8 | r;
}

static void render(float px, float py, float ang) {
  float dx = fcos(ang), dy = fsin(ang);
  float plx = -dy * 0.66f, ply = dx * 0.66f;
  for (int x = 0; x < W; x++) {
    float cam = 2.0f * x / (float)W - 1.0f;
    float rdx = dx + plx * cam, rdy = dy + ply * cam;
    int mx = (int)px, my = (int)py;
    float ddx = rdx == 0 ? 1e30f : __builtin_fabsf(1.0f / rdx);
    float ddy = rdy == 0 ? 1e30f : __builtin_fabsf(1.0f / rdy);
    int stx, sty;
    float sdx, sdy;
    if (rdx < 0) stx = -1, sdx = (px - mx) * ddx; else stx = 1, sdx = (mx + 1.0f - px) * ddx;
    if (rdy < 0) sty = -1, sdy = (py - my) * ddy; else sty = 1, sdy = (my + 1.0f - py) * ddy;
    int side = 0, cell = 0;
    for (int guard = 0; guard < 64 && !cell; guard++) {
      if (sdx < sdy) sdx += ddx, mx += stx, side = 0;
      else sdy += ddy, my += sty, side = 1;
      if (mx < 0 || my < 0 || mx >= MAP_W || my >= MAP_H) break;
      cell = map[my][mx] - '0';
    }
    float dist = side == 0 ? sdx - ddx : sdy - ddy;
    if (dist < 0.05f) dist = 0.05f;
    int lh = (int)(H / dist);
    int y0 = H / 2 - lh / 2, y1 = H / 2 + lh / 2;
    float wx = side == 0 ? py + dist * rdy : px + dist * rdx;
    wx -= (int)wx;
    int tx = (int)(wx * TEX) & (TEX - 1);
    const u32 *t = tex[(cell > 0 ? cell : 1) - 1];
    int light = 256 - (int)(dist * 18.0f);
    if (light < 40) light = 40;
    if (side) light = light * 3 / 4;
    for (int y = 0; y < H; y++) {
      u32 c;
      if (y < y0) c = 0xff402010u + (u32)(y / 8) * 0x00010100u;            /* sky */
      else if (y >= y1) c = 0xff204040u + (u32)((H - y) / 8) * 0x00000101u; /* floor */
      else c = shade(t[(((y - y0) * TEX / (lh ? lh : 1)) & (TEX - 1)) * TEX + tx], light);
      fb[y * W + x] = c;
    }
  }
}

static int solid(float x, float y) { return map[(int)y][(int)x] != '0'; }

int main(void) {
  init_tables();
  float px = 3.5f, py = 5.5f, ang = 0.0f;
  long probe = g_syscall3(SYS_plat_buttons, 0, 0, 0);
  int interactive = probe >= 0;
  u32 all = 2166136261u;
  long t0 = g_syscall3(SYS_plat_ticks, 0, 0, 0);
  for (int frame = 0; interactive || frame < DEMO_FRAMES; frame++) {
    long b = interactive ? g_syscall3(SYS_plat_buttons, 0, 0, 0) : 0;
    if (interactive && (b & B_START)) break;
    float speed = 0.06f, turn = 0.008f;
    float mvx = 0, mvy = 0;
    if (interactive) {
      if (b & B_LEFT) ang -= turn;
      if (b & B_RIGHT) ang += turn;
      if (b & B_UP) mvx += fcos(ang) * speed, mvy += fsin(ang) * speed;
      if (b & B_DOWN) mvx -= fcos(ang) * speed, mvy -= fsin(ang) * speed;
    } else { /* scripted walk through the level */
      ang += 0.006f;
      mvx = fcos(ang) * 0.05f, mvy = fsin(ang * 0.5f) * 0.05f;
    }
    if (!solid(px + mvx * 3, py)) px += mvx;
    if (!solid(px, py + mvy * 3)) py += mvy;
    if (ang > 1.0f) ang -= 1.0f;
    if (ang < 0.0f) ang += 1.0f;
    render(px, py, ang);
    g_syscall3(SYS_plat_present, (long)fb, W, H);
    if (!interactive) {
      u32 h = g_fnv1a(fb, sizeof(fb), 2166136261u);
      all = g_fnv1a(&h, 4, all);
      if (frame % 8 == 0) g_printf("frame %2d pos=(%.3f,%.3f) hash=%08x\n", frame, (double)px, (double)py, h);
    }
  }
  long t1 = g_syscall3(SYS_plat_ticks, 0, 0, 0);
  if (!interactive) g_printf("raycast demo: %d frames, combined hash=%08x\n", DEMO_FRAMES, all);
  if (t0 >= 0 && t1 >= 0) g_printf("elapsed %ld us\n", t1 - t0);
  return 0;
}
