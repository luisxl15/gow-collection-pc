/*
 * Hosted (Linux) platform layer: used for differential testing on x86-64 and
 * on ARMv7 Linux under qemu-arm, which shares the Vita's CPU ISA and ABI.
 *
 * RC_PRESENT_PPM=<dir> writes every presented frame as a PPM for inspection.
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "../recomp.h"

void *rc_platform_alloc_guest_ram(uint32_t size) { return malloc(size); }

long rc_platform_write(int fd, const void *buf, uint32_t len) {
  ssize_t r = write(fd, buf, len);
  return r < 0 ? -9 /* EBADF */ : r;
}

void rc_platform_exit(int code) {
  fflush(stdout);
  _exit(code);
}

long rc_platform_present(const uint8_t *rgba, uint32_t w, uint32_t h) {
  static unsigned frame;
  const char *dir = getenv("RC_PRESENT_PPM");
  if (dir) {
    char path[512];
    snprintf(path, sizeof(path), "%s/frame_%04u.ppm", dir, frame);
    FILE *f = fopen(path, "wb");
    if (f) {
      fprintf(f, "P6\n%u %u\n255\n", w, h);
      for (uint32_t i = 0; i < w * h; i++) fwrite(rgba + i * 4, 1, 3, f);
      fclose(f);
    }
  }
  frame++;
  /* Report "unavailable" so guest control flow matches the qemu reference run. */
  return -1;
}

long rc_platform_buttons(void) { return -1; }
long rc_platform_ticks_us(void) { return -1; }

void rc_platform_log(const char *msg) { fputs(msg, stderr); }

int main(void) { return rc_run(); }
