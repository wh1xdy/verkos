#ifndef STAM_GIC_H
#define STAM_GIC_H
#include <stdint.h>
#include "fdt.h"
int  gic_init(const fdt_t *f);
void gic_enable_irq(unsigned intid);
int  gic_timer_irq(const fdt_t *f);
void gic_set_timer_irq(int id);
void gic_set_console_irq(int id);
#endif
