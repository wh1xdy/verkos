#ifndef VKERN_CONSOLE_H
#define VKERN_CONSOLE_H
#include <stdint.h>
#include <stddef.h>
struct fdt_t_fwd;
#include "fdt.h"
int  console_init(const fdt_t *f);
void kputc(char c);
void kputs(const char *s);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *console_kind(void);
uint64_t console_addr(void);
#endif
