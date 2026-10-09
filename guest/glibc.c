#include <stdarg.h>
#include "glibc.h"

long g_syscall3(long n, long a, long b, long c) {
  register long v0 __asm__("$2") = n;
  register long a0 __asm__("$4") = a;
  register long a1 __asm__("$5") = b;
  register long a2 __asm__("$6") = c;
  register long a3 __asm__("$7");
  __asm__ volatile("syscall"
                   : "+r"(v0), "=r"(a3)
                   : "r"(a0), "r"(a1), "r"(a2)
                   : "$1", "$3", "$8", "$9", "$10", "$11", "$12", "$13", "$14", "$15", "$24",
                     "$25", "hi", "lo", "memory");
  return a3 ? -v0 : v0;
}

void g_exit(int code) {
  g_syscall3(SYS_exit, code, 0, 0);
  for (;;) {}
}

ssize_t g_write(int fd, const void *buf, size_t len) {
  return g_syscall3(SYS_write, fd, (long)buf, len);
}

void *memset(void *d, int c, size_t n) {
  u8 *p = d;
  while (n--) *p++ = (u8)c;
  return d;
}

void *memcpy(void *d, const void *s, size_t n) {
  u8 *p = d;
  const u8 *q = s;
  while (n--) *p++ = *q++;
  return d;
}

void *memmove(void *d, const void *s, size_t n) {
  u8 *p = d;
  const u8 *q = s;
  if (p < q) {
    while (n--) *p++ = *q++;
  } else {
    p += n;
    q += n;
    while (n--) *--p = *--q;
  }
  return d;
}

int memcmp(const void *a, const void *b, size_t n) {
  const u8 *x = a, *y = b;
  for (; n; n--, x++, y++)
    if (*x != *y) return *x - *y;
  return 0;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n]) n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a && *a == *b) a++, b++;
  return (u8)*a - (u8)*b;
}

u32 g_fnv1a(const void *p, size_t n, u32 h) {
  const u8 *b = p;
  while (n--) {
    h ^= *b++;
    h *= 16777619u;
  }
  return h;
}

/* ---- formatted output ---- */

struct out {
  char *buf;
  size_t cap, len;
};

static void emit(struct out *o, char c) {
  if (o->len + 1 < o->cap) o->buf[o->len] = c;
  o->len++;
}

static void emit_num(struct out *o, u64 v, int base, int upper, int width, char pad, int neg) {
  char tmp[24];
  const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  int n = 0;
  do {
    tmp[n++] = digits[v % base];
    v /= base;
  } while (v);
  if (neg) {
    if (pad == '0') {
      emit(o, '-');
      width--;
    } else {
      tmp[n++] = '-';
    }
  }
  for (int i = n; i < width; i++) emit(o, pad);
  while (n) emit(o, tmp[--n]);
}

static void emit_double(struct out *o, double v, int prec) {
  if (v != v) {
    emit(o, 'n'), emit(o, 'a'), emit(o, 'n');
    return;
  }
  if (v < 0) {
    emit(o, '-');
    v = -v;
  }
  if (v > 1e18) {
    emit(o, 'i'), emit(o, 'n'), emit(o, 'f');
    return;
  }
  double scale = 1;
  for (int i = 0; i < prec; i++) scale *= 10;
  u64 whole = (u64)v;
  u64 frac = (u64)((v - (double)whole) * scale + 0.5);
  if (frac >= (u64)scale) {
    whole++;
    frac -= (u64)scale;
  }
  emit_num(o, whole, 10, 0, 0, ' ', 0);
  if (prec) {
    emit(o, '.');
    emit_num(o, frac, 10, 0, prec, '0', 0);
  }
}

static int vformat(char *buf, size_t cap, const char *fmt, va_list ap) {
  struct out o = {buf, cap, 0};
  for (; *fmt; fmt++) {
    if (*fmt != '%') {
      emit(&o, *fmt);
      continue;
    }
    fmt++;
    char pad = ' ';
    int width = 0, prec = 6, longs = 0;
    if (*fmt == '0') pad = '0', fmt++;
    while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    if (*fmt == '.') {
      fmt++;
      prec = 0;
      while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
    }
    while (*fmt == 'l') longs++, fmt++;
    switch (*fmt) {
      case 'd':
      case 'i': {
        s64 v = longs >= 2 ? va_arg(ap, s64) : va_arg(ap, int);
        emit_num(&o, v < 0 ? (u64)-v : (u64)v, 10, 0, width, pad, v < 0);
        break;
      }
      case 'u':
      case 'x':
      case 'X': {
        u64 v = longs >= 2 ? va_arg(ap, u64) : va_arg(ap, unsigned);
        emit_num(&o, v, *fmt == 'u' ? 10 : 16, *fmt == 'X', width, pad, 0);
        break;
      }
      case 'p':
        emit(&o, '0'), emit(&o, 'x');
        emit_num(&o, (u32)va_arg(ap, void *), 16, 0, 8, '0', 0);
        break;
      case 'c':
        emit(&o, (char)va_arg(ap, int));
        break;
      case 's': {
        const char *s = va_arg(ap, const char *);
        if (!s) s = "(null)";
        int len = strlen(s);
        for (int i = len; i < width; i++) emit(&o, ' ');
        while (*s) emit(&o, *s++);
        break;
      }
      case 'f':
        emit_double(&o, va_arg(ap, double), prec);
        break;
      case '%':
        emit(&o, '%');
        break;
      default:
        emit(&o, '?');
        break;
    }
  }
  if (cap) buf[o.len < cap ? o.len : cap - 1] = 0;
  return o.len;
}

int g_snprintf(char *buf, size_t n, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int r = vformat(buf, n, fmt, ap);
  va_end(ap);
  return r;
}

int g_printf(const char *fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  int r = vformat(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  g_write(1, buf, r < (int)sizeof(buf) ? r : (int)sizeof(buf) - 1);
  return r;
}

void g_puts(const char *s) {
  g_write(1, s, strlen(s));
  g_write(1, "\n", 1);
}
