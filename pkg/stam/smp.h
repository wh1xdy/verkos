#ifndef STAM_SMP_H
#define STAM_SMP_H
#include <stdint.h>
#include "fdt.h"

/* Which processor this is, as an index rather than as an MPIDR. The affinity
 * register is a hierarchy of fields with gaps in it, and every place that
 * wants to say "this CPU" wants a small number instead. */
unsigned cpu_index(void);
uint64_t cpu_mpidr(void);

/* Start the other processors, using whatever the device tree says the
 * firmware's conduit is. Returns how many answered. */
int smp_start_others(const fdt_t *f);
unsigned smp_cpus_online(void);
#endif
