#ifndef VITA_CONSOLE_H
#define VITA_CONSOLE_H
#include <stdint.h>

int vcon_init(void);
void vcon_clear(void);
void vcon_set_color(uint32_t abgr);
void vcon_putc(char c);
void vcon_write(const char *s, unsigned len);
void vcon_puts(const char *s);

#endif
