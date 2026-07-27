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
/* A line, with the editing a person expects: characters echoed as they arrive,
 * backspace removing one, the line delivered on Return. This belongs in the
 * kernel because it is what a terminal driver is for — a program should not
 * have to know that undoing a backspace on screen takes three bytes. Blocks
 * until a whole line is there. */
long console_read_line(char *out, uint64_t len);
/* Which interrupt the console is on, according to the device tree. */
int  console_irq(const fdt_t *f);
void kquiet(int on);
int  kchatty(void);
void kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void console_rebase(uint64_t offset);
const char *console_kind(void);
uint64_t console_addr(void);
#endif
