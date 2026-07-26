/* stam — what the kernel knows about the machine it woke up on.
 *
 * This struct is the whole point of the boot-protocol decision. The kernel
 * reads *this* and nothing else: it never learns whether it was started by
 * vboot, by U-Boot, by QEMU's -kernel, or one day by something speaking
 * Multiboot2 or the Limine protocol. Each of those is a shim that fills this
 * in — handover_dtb.c is the first — so adding a protocol is a new file, not
 * a change to the kernel.
 *
 * Keep it to facts about the machine. Anything a driver can discover for
 * itself belongs in the driver, not here.
 */
#ifndef STAM_BOOTINFO_H
#define STAM_BOOTINFO_H
#include <stdint.h>

typedef struct {
    const char *cmdline;        /* what the boot menu asked for            */
    const char *model;          /* human-readable machine name             */
    uint64_t    ram_base;       /* where usable memory starts              */
    uint64_t    ram_size;
    uint64_t    initrd_start;   /* 0 if none was loaded                    */
    uint64_t    initrd_end;
    unsigned    cpus;           /* how many the firmware told us about     */
    const void *dtb;            /* kept: drivers still need the full tree  */
    uint64_t    acpi_rsdp;      /* 0 on machines that describe with a DTB  */
} boot_info_t;

/* The shims. Each returns 0 on success. */
int  handover_dtb(const void *dtb, boot_info_t *bi);
void handover_set_cmdline(const char *s);

#endif
