/*
 * Freestanding PS Vita entry point and minimal libc subset.
 *
 * Used instead of VitaSDK's newlib because the prebuilt VitaSDK release is
 * not reachable from this build environment. The entry receives the
 * (args, argp) pair that the Vita loader passes to an eboot's ELF entry.
 */
#include <stddef.h>
#include <stdint.h>
#include <psp2/kernel/processmgr.h>

int vita_main(void);

void *memset(void *dst, int c, size_t n) {
  unsigned char *d = dst;
  while (n--) *d++ = (unsigned char)c;
  return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  while (n--) *d++ = *s++;
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = dst;
  const unsigned char *s = src;
  if (d < s) {
    while (n--) *d++ = *s++;
  } else {
    d += n;
    s += n;
    while (n--) *--d = *--s;
  }
  return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y) return *x - *y;
  return 0;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

void _start(unsigned int args, void *argp) __attribute__((used, noreturn));
void _start(unsigned int args, void *argp) {
  (void)args;
  (void)argp;
  sceKernelExitProcess(vita_main());
  for (;;) {}
}
