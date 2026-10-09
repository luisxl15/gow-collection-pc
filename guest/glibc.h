/*
 * Minimal freestanding guest C library.
 *
 * System interface = Linux o32 syscalls, so the same guest binary runs under
 * qemu-mipsel (reference) and under the recompiled runtime (HLE).
 * Two extra "platform" syscalls model what a console game needs; Linux
 * returns -ENOSYS for them and the guest falls back to a deterministic path.
 */
#ifndef GUEST_LIBC_H
#define GUEST_LIBC_H

typedef unsigned int size_t;
typedef int ssize_t;
typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef signed char s8;
typedef short s16;
typedef int s32;
typedef long long s64;

#define NULL ((void *)0)

#define SYS_exit 4001
#define SYS_write 4004
/* Platform syscalls (not Linux; -ENOSYS under qemu). */
#define SYS_plat_present 4900 /* a0 = framebuffer (RGBA8888), a1 = width, a2 = height */
#define SYS_plat_buttons 4901 /* returns button bitmask, or -ENOSYS */
#define SYS_plat_ticks 4902   /* returns microseconds, or -ENOSYS */

long g_syscall3(long n, long a, long b, long c);
void g_exit(int code) __attribute__((noreturn));
ssize_t g_write(int fd, const void *buf, size_t len);

void *memset(void *d, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);

int g_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int g_snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void g_puts(const char *s);

/* FNV-1a over a byte range, used by tests to compare large state cheaply. */
u32 g_fnv1a(const void *p, size_t n, u32 h);

#endif
