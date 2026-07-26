/* stam — milestone 1: wake up, find out what machine this is, say so.
 *
 * There is deliberately no hardware address anywhere in this file, nor in the
 * console driver. Everything below is read out of the device tree the
 * firmware handed us. That is the difference between a kernel that runs on
 * one machine and a kernel that can be made to run on many: it is decided
 * here, in the first hundred lines, not later.
 */
#include "bootinfo.h"
#include "console.h"
#include "fdt.h"
#include "mmu.h"
#include "gic.h"
#include "timer.h"

#define STAM_VERSION "0.1"

static boot_info_t boot;

/* Report in MiB up to 8 GiB: "1 GiB" for 1.9 GiB of free memory is true but
 * useless, and the rounding hides half a gigabyte. */
static void print_size(uint64_t bytes) {
    if (bytes >= (8ull << 30)) kprintf("%llu GiB", (unsigned long long)(bytes >> 30));
    else if (bytes >= (1ull << 20)) kprintf("%llu MiB", (unsigned long long)(bytes >> 20));
    else kprintf("%llu bytes", (unsigned long long)bytes);
}

void kmain(void *dtb) {
    if (handover_dtb(dtb, &boot) != 0) {
        /* No tree means no console either — there is nothing to print to and
         * nothing sensible to do. Stopping is the honest response. */
        for (;;) __asm__ volatile("wfe");
    }
    if (console_init(machine_tree()) != 0) for (;;) __asm__ volatile("wfe");

    kprintf("\n");
    kprintf("stam %s — the VerkOS kernel\n", STAM_VERSION);
    kprintf("\n");
    kprintf("  machine   %s\n", boot.model);
    kprintf("  console   %s at 0x%llx (found in the device tree)\n",
            console_kind(), (unsigned long long)console_addr());
    kprintf("  cpus      %llu\n", (unsigned long long)boot.cpus);
    kprintf("  memory    ");
    print_size(boot.ram_size);
    kprintf(" at 0x%llx\n", (unsigned long long)boot.ram_base);
    if (boot.initrd_end > boot.initrd_start) {
        kprintf("  initrd    ");
        print_size(boot.initrd_end - boot.initrd_start);
        kprintf(" at 0x%llx\n", (unsigned long long)boot.initrd_start);
    } else {
        kprintf("  initrd    none\n");
    }
    kprintf("  cmdline   %s\n", boot.cmdline ? boot.cmdline : "(empty)");
    kprintf("  dtb       0x%llx\n", (unsigned long long)(uintptr_t)boot.dtb);
    /* Vectors before the MMU: a bad page table faults, and a fault with no
     * vector table is a machine that resets with nothing on the console. */
    exceptions_install();
    kprintf("  vectors   installed\n");

    if (boot.free_size)
        pmm_init(boot.free_base, boot.free_size, (uint64_t)(uintptr_t)boot.dtb, 1u << 20);
    else
        pmm_init(boot.ram_base, boot.ram_size, (uint64_t)(uintptr_t)boot.dtb, 1u << 20);
    if (mmu_init(boot.ram_base, boot.ram_size) != 0) {
        kprintf("  mmu       FAILED to allocate a page table\n");
    } else {
        kprintf("  mmu       on, caches on, table at 0x%llx (%llu page)\n",
                (unsigned long long)mmu_table(), (unsigned long long)pmm_pages_used());
        kprintf("  free ram  ");
        print_size(pmm_free_bytes());
        kprintf("\n");
    }

    int ticking = 0;
    if (gic_init(machine_tree()) == 0 && timer_init(machine_tree(), 100) == 0) {
        ticking = 1;
        irq_enable();
        /* Wait for a second's worth of ticks and say so. A timer that is
         * configured but never fires is the commonest silent failure at this
         * stage, and it looks exactly like a timer that works. */
        uint64_t target = timer_hz();
        while (timer_ticks() < target) __asm__ volatile("wfi");
        kprintf("  ticking   %llu interrupts taken — the kernel can preempt\n",
                (unsigned long long)timer_ticks());
    }

    /* Prove the fault path works, on request. A handler that has never run is
     * a handler you find out about at the worst moment. */
    if (boot.cmdline) {
        const char *c = boot.cmdline;
        for (; *c; c++)
            if (c[0]=='s'&&c[1]=='t'&&c[2]=='a'&&c[3]=='m'&&c[4]=='.'&&c[5]=='f'&&c[6]=='a'&&c[7]=='u'&&c[8]=='l'&&c[9]=='t') {
                kprintf("\n  stam.fault: touching an address on purpose...\n");
                /* Above the 39-bit address space the page tables describe,
                 * so this is a translation fault — the shape of an actual
                 * kernel bug. An address inside the mapped range would give
                 * an external abort from the bus instead, which is a
                 * different and less instructive failure. */
                volatile uint64_t *nowhere = (volatile uint64_t *)0x8000000000ul;
                *nowhere = 1;
            }
    }

    kprintf("\n");
    /* Say what actually happened. The first version of this line claimed
     * "time running" on a machine whose interrupt controller we do not
     * support and where the timer never started — a summary that lies is
     * worse than no summary, because it is believed. */
    if (ticking)
        kprintf("  milestone 2: hardware found, faults reported, memory mapped, time running.\n");
    else
        kprintf("  milestone 2 partly: memory mapped and faults reported, but this\n"
                "  machine has no interrupt controller we support, so nothing ticks.\n");
    kprintf("  no scheduler and no userspace yet.\n");
    kprintf("\n");

    for (;;) __asm__ volatile("wfe");
}
