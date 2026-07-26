#ifndef STAM_CONSOLE_H
#define STAM_CONSOLE_H
#include <stdint.h>
#include <stddef.h>
struct fdt_t_fwd;
#include "fdt.h"
int  console_init(const fdt_t *f);
void kputc(char c);
void kputs(const char *s);
int  kgetc(void);
void console_interrupt(void);
void console_enable_interrupt(void);
int  console_is_interrupt_driven(void);
/* Which interrupt the console is on, according to the device tree. */
int  console_irq(const fdt_t *f);
void kquiet(int on);
int  kchatty(void);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void console_rebase(uint64_t offset);
const char *console_kind(void);
uint64_t console_addr(void);
#endif
