/* vkern — milestone 1: wake up, find out what machine this is, say so.
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

#define VKERN_VERSION "0.1"

static boot_info_t boot;

static void print_size(uint64_t bytes) {
    if (bytes >= (1ull << 30)) kprintf("%llu GiB", (unsigned long long)(bytes >> 30));
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
    kprintf("vkern %s — the VerkOS kernel\n", VKERN_VERSION);
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
    kprintf("\n");
    kprintf("  milestone 1 reached: the kernel found its own hardware.\n");
    kprintf("  no memory manager, no interrupts, no userspace yet.\n");
    kprintf("\n");

    for (;;) __asm__ volatile("wfe");
}
