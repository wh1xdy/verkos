/* stam — where a secondary processor begins.
 *
 * PSCI enters here with the MMU off, the caches off, and nothing configured:
 * exactly the state the machine was in at reset, except that the rest of the
 * kernel is already running on another core. x0 holds the context id we passed
 * to CPU_ON, which is the physical address of the configuration the primary
 * left for us.
 *
 * Every value here was read off the primary rather than recomputed. Two paths
 * that must agree about page tables are a defect generator — D-042 is the
 * entry about what happens when they drift.
 */
    .section .text

/* The addresses of these two symbols, computed PC-relative.
 *
 * Never take the address of a symbol from C in this kernel: it is built
 * position-independent and nothing applies relocations, so anything routed
 * through the GOT holds the address the image had when it was linked. D-037
 * found it as a thread starting at 0x193c and D-039 found it again in the
 * first user program — and here it was a second processor that PSCI started
 * at an address in the low half that no longer exists, reported as a CPU that
 * accepted the request and never arrived. Third time. */
    .global secondary_entry_addr
    .hidden secondary_entry_addr
secondary_entry_addr:
    adrp    x0, secondary_entry
    add     x0, x0, :lo12:secondary_entry
    ret

    .global secondary_main_addr
    .hidden secondary_main_addr
secondary_main_addr:
    adrp    x0, secondary_main
    add     x0, x0, :lo12:secondary_main
    ret

    .global secondary_entry
    .hidden secondary_entry
secondary_entry:
    mov     x19, x0                 /* the config, physical */

    /* SCTLR with the RES1 bits, MMU still off. A plain zero here is a machine
     * that faults on its first access for reasons nothing explains. */
    ldr     x0, =0x30d00800
    msr     sctlr_el1, x0
    isb

    /* A stack, before anything else touches memory. */
    ldr     x0, [x19, #0]           /* cfg.sp */
    mov     sp, x0

    ldr     x0, [x19, #8]           /* cfg.mair  */
    msr     mair_el1, x0
    ldr     x0, [x19, #16]          /* cfg.tcr   */
    msr     tcr_el1, x0
    ldr     x0, [x19, #24]          /* cfg.ttbr0 */
    msr     ttbr0_el1, x0
    ldr     x0, [x19, #32]          /* cfg.ttbr1 */
    msr     ttbr1_el1, x0
    isb

    /* Vectors before the MMU, for the reason D-032 gives: the first thing
     * virtual memory can do wrong should be reported rather than silent. */
    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    msr     vbar_el1, x0
    isb

    tlbi    vmalle1
    dsb     ish
    isb

    /* On. The same SCTLR the primary is running with, which has the MMU and
     * caches enabled. */
    ldr     x0, [x19, #40]          /* cfg.sctlr */
    msr     sctlr_el1, x0
    isb

    /* The stack pointer still names the low half, which is about to stop
     * existing for us; move it to the same memory seen through the kernel's
     * window before touching anything (D-042). */
    mov     x0, sp
    mov     x1, #0xffffff8000000000
    add     x0, x0, x1
    mov     sp, x0

    ldr     x0, [x19, #48]          /* cfg.entry_high */
    br      x0
