#ifndef STAM_GIC_H
#define STAM_GIC_H
#include <stdint.h>
#include "fdt.h"
int  gic_init(const fdt_t *f);
/* The part of the controller that is one processor's rather than the
 * machine's. Called by every core, on itself, including the boot core. */
void gic_cpu_init(void);
void gic_enable_irq(unsigned intid);
int  gic_timer_irq(const fdt_t *f);
void gic_set_timer_irq(int id);
void gic_set_console_irq(int id);
void gic_set_blk_irq(int id);
void gic_set_net_irq(int id);
#endif
