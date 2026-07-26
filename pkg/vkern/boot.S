/* vkern — the VerkOS kernel. Entry point, aarch64.
 *
 * We use the standard arm64 Image protocol rather than inventing our own, even
 * though vboot is ours and could hand over any way we liked. Two reasons: QEMU's
 * -kernel loads an Image directly, which makes the edit-build-boot loop seconds
 * instead of minutes, and every ARM bootloader in existence already speaks it.
 * There is no portability to be gained by being different here.
 *
 * On entry the boot protocol guarantees:
 *   x0  = physical address of the device tree blob
 *   MMU off, D-cache off, interrupts masked
 *   we are at EL2 (normal) or EL1 (already dropped)
 *   only one CPU is running; the others are parked
 */

    .section .head.text, "ax"
    .global _start
_start:
    b       entry                   /* code0: must be a valid branch      */
    .long   0                       /* code1                              */
    .quad   0x80000                 /* text_offset: where to load us      */
    .quad   _kernel_end - _start    /* image_size                         */
    .quad   0xa                     /* flags: LE, 4K pages, anywhere      */
    .quad   0
    .quad   0
    .quad   0
    .ascii  "ARM\x64"               /* magic the loader looks for         */
    .long   0

    .section .text
entry:
    mov     x20, x0                 /* the DTB pointer, before anything   */
                                    /* else can clobber x0                */

    /* If the firmware left us at EL2 we drop to EL1. A kernel could stay at
     * EL2 and be a hypervisor, but everything below assumes EL1, and mixing
     * the two silently is how you get faults that make no sense later. */
    mrs     x0, CurrentEL
    lsr     x0, x0, #2
    cmp     x0, #2
    b.ne    in_el1

    /* EL1 will be AArch64, not AArch32. */
    mov     x0, #(1 << 31)
    msr     hcr_el2, x0

    /* SCTLR_EL1: MMU and caches off, but the RES1 bits must be set. Writing
     * a plain zero here is a classic way to produce a machine that faults on
     * its first memory access for reasons the debugger cannot explain. */
    ldr     x0, =0x30d00800
    msr     sctlr_el1, x0

    /* Return into EL1h with all interrupts masked. */
    mov     x0, #0x3c5              /* D,A,I,F masked, M = EL1h           */
    msr     spsr_el2, x0
    adr     x0, in_el1
    msr     elr_el2, x0
    eret

in_el1:
    /* A stack, before any C code. */
    adrp    x0, _stack_top
    add     x0, x0, :lo12:_stack_top
    mov     sp, x0

    /* Zero the bss. C assumes this; nothing else will do it for us. */
    adrp    x0, _bss_start
    add     x0, x0, :lo12:_bss_start
    adrp    x1, _bss_end
    add     x1, x1, :lo12:_bss_end
1:  cmp     x0, x1
    b.hs    2f
    str     xzr, [x0], #8
    b       1b
2:

    mov     x0, x20                 /* hand the DTB to C                  */
    bl      kmain

    /* kmain must not return. If it does, stop cleanly rather than running
     * off into whatever follows us in memory. */
halt:
    wfe
    b       halt
