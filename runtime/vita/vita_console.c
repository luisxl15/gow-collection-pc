/*
 * Framebuffer text console for PS Vita homebrew (960x544, A8B8G8R8).
 * Output is mirrored to sceClibPrintf so it also shows up in Vita3K logs.
 */
#include <stdint.h>
#include <psp2/display.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/sysmem.h>
#include "vita_console.h"
#include "../../third_party/font8x8/font8x8_basic.h"

#define FB_W 960
#define FB_H 544
#define FB_PITCH 960
#define FB_SIZE (256 * 1024 * 9) /* 2 MiB+ rounded to the 256 KiB CDRAM granularity */
#define SCALE 2
#define COLS (FB_W / (8 * SCALE))
#define ROWS (FB_H / (8 * SCALE))

static uint32_t *fb;
static int cur_x, cur_y;
static uint32_t fg = 0xFFFFFFFF, bg = 0xFF201810;

int vcon_init(void) {
  SceUID block = sceKernelAllocMemBlock("vcon_fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                        FB_SIZE, NULL);
  if (block < 0) return block;
  void *base;
  int rc = sceKernelGetMemBlockBase(block, &base);
  if (rc < 0) return rc;
  fb = base;
  vcon_clear();

  SceDisplayFrameBuf param = {0};
  param.size = sizeof(param);
  param.base = fb;
  param.pitch = FB_PITCH;
  param.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
  param.width = FB_W;
  param.height = FB_H;
  return sceDisplaySetFrameBuf(&param, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

void vcon_set_color(uint32_t abgr) { fg = abgr; }

uint32_t *vcon_framebuffer(void) { return fb; }

void vcon_clear(void) {
  for (int i = 0; i < FB_PITCH * FB_H; i++) fb[i] = bg;
  cur_x = cur_y = 0;
}

static void scroll(void) {
  const int line = FB_PITCH * 8 * SCALE;
  for (int i = 0; i < FB_PITCH * FB_H - line; i++) fb[i] = fb[i + line];
  for (int i = FB_PITCH * FB_H - line; i < FB_PITCH * FB_H; i++) fb[i] = bg;
  cur_y = ROWS - 1;
}

static void draw_glyph(int cx, int cy, unsigned char c) {
  const char *g = font8x8_basic[c & 0x7F];
  for (int y = 0; y < 8 * SCALE; y++) {
    uint32_t *row = fb + (cy * 8 * SCALE + y) * FB_PITCH + cx * 8 * SCALE;
    unsigned bits = (unsigned char)g[y / SCALE];
    for (int x = 0; x < 8 * SCALE; x++) row[x] = (bits >> (x / SCALE)) & 1 ? fg : bg;
  }
}

void vcon_putc(char c) {
  if (!fb) return;
  if (c == '\n') {
    cur_x = 0;
    if (++cur_y >= ROWS) scroll();
    return;
  }
  draw_glyph(cur_x, cur_y, (unsigned char)c);
  if (++cur_x >= COLS) {
    cur_x = 0;
    if (++cur_y >= ROWS) scroll();
  }
}

void vcon_write(const char *s, unsigned len) {
  char buf[256];
  while (len) {
    unsigned n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    for (unsigned i = 0; i < n; i++) {
      buf[i] = s[i];
      vcon_putc(s[i]);
    }
    buf[n] = 0;
    sceClibPrintf("%s", buf);
    s += n;
    len -= n;
  }
}

void vcon_puts(const char *s) {
  unsigned n = 0;
  while (s[n]) n++;
  vcon_write(s, n);
}
