/*
 * PS Vita platform layer for recompiled guests.
 *
 *   guest RAM      -> one 32 MiB USER_RW memory block
 *   write(1/2)     -> framebuffer console + sceClibPrintf (Vita3K log)
 *   plat_present   -> integer-scaled blit of the guest RGBA framebuffer
 *   plat_buttons   -> sceCtrlPeekBufferPositive mapped to the guest bitmask
 *   plat_ticks     -> sceKernelGetProcessTimeWide (microseconds)
 *   exit           -> keep the last screen up until START (max 15 s)
 */
#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>
#include "../recomp.h"
#include "vita_console.h"

#define SCREEN_W 960
#define SCREEN_H 544

void *rc_platform_alloc_guest_ram(uint32_t size) {
  SceUID block = sceKernelAllocMemBlock("rc_guest_ram", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
  if (block < 0) return NULL;
  void *base = NULL;
  if (sceKernelGetMemBlockBase(block, &base) < 0) return NULL;
  return base;
}

long rc_platform_write(int fd, const void *buf, uint32_t len) {
  if (fd != 1 && fd != 2) return -9;
  vcon_write(buf, len);
  return len;
}

void rc_platform_log(const char *msg) { vcon_puts(msg); }

long rc_platform_present(const uint8_t *rgba, uint32_t w, uint32_t h) {
  uint32_t *fb = vcon_framebuffer();
  if (!fb) return -1;
  uint32_t sx = SCREEN_W / w, sy = SCREEN_H / h;
  uint32_t s = sx < sy ? sx : sy;
  if (s == 0) return -1;
  uint32_t ox = (SCREEN_W - w * s) / 2, oy = (SCREEN_H - h * s) / 2;
  const uint32_t *src = (const uint32_t *)rgba; /* guest pixels are A8B8G8R8, like the display */
  for (uint32_t y = 0; y < h; y++) {
    const uint32_t *row = src + y * w;
    for (uint32_t r = 0; r < s; r++) {
      uint32_t *dst = fb + (oy + y * s + r) * SCREEN_W + ox;
      if (s == 2) {
        for (uint32_t x = 0; x < w; x++) dst[2 * x] = dst[2 * x + 1] = row[x];
      } else {
        for (uint32_t x = 0; x < w * s; x++) dst[x] = row[x / s];
      }
    }
  }
  sceDisplayWaitVblankStart();
  return 0;
}

long rc_platform_buttons(void) {
  SceCtrlData pad;
  if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return 0;
  static const struct { uint32_t sce, guest; } map[] = {
      {SCE_CTRL_UP, 1},      {SCE_CTRL_DOWN, 2},   {SCE_CTRL_LEFT, 4},  {SCE_CTRL_RIGHT, 8},
      {SCE_CTRL_CROSS, 16},  {SCE_CTRL_CIRCLE, 32}, {SCE_CTRL_START, 64}, {SCE_CTRL_LTRIGGER, 128},
      {SCE_CTRL_RTRIGGER, 256},
  };
  long b = 0;
  for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
    if (pad.buttons & map[i].sce) b |= map[i].guest;
  /* Left stick acts as a D-pad. */
  if (pad.ly < 64) b |= 1;
  if (pad.ly > 192) b |= 2;
  if (pad.lx < 64) b |= 4;
  if (pad.lx > 192) b |= 8;
  return b;
}

long rc_platform_ticks_us(void) { return (long)(sceKernelGetProcessTimeWide() & 0x7fffffff); }

void rc_platform_exit(int code) {
  char msg[48] = "\n[guest exited with code   ]\nSTART: quit\n";
  unsigned c = (unsigned)code & 0xff;
  msg[25] = '0' + c / 100;
  msg[26] = '0' + (c / 10) % 10;
  msg[27] = '0' + c % 10;
  vcon_puts(msg);
  for (int i = 0; i < 15 * 60; i++) {
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    if (pad.buttons & SCE_CTRL_START) break;
    sceKernelDelayThread(16666);
  }
  sceKernelExitProcess(code);
  for (;;) {}
}

int vita_main(void) {
  if (vcon_init() < 0) return 1;
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
  vcon_puts("vita-recomp-lab: running recompiled MIPS guest\n");
  return rc_run();
}
