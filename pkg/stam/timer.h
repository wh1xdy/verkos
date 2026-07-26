#ifndef STAM_TIMER_H
#define STAM_TIMER_H
#include <stdint.h>
#include "fdt.h"
int      timer_init(const fdt_t *f, unsigned want_hz);
void     timer_tick(void);
uint64_t timer_ticks(void);
unsigned timer_hz(void);
void     irq_enable(void);
void     irq_disable(void);
#endif
