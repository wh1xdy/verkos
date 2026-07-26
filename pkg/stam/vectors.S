/* stam — the exception vector table.
 *
 * Named vectors.S and not exceptions.S because exceptions.c exists: both
 * would build exceptions.o, make would pick one, and the link fails with an
 * undefined symbol that is plainly defined in a file you are looking at.
 *
 * This is written before the MMU on purpose. Once virtual memory is on, a
 * mistake in a page table is a fault, and a fault with no vector table
 * installed is a machine that resets or hangs with nothing on the console. The
 * order matters: install the vectors, then turn on the thing that can fault.
 *
 * The table has a fixed shape the hardware knows about — sixteen entries of
 * 128 bytes each, at a 2 KiB-aligned address in VBAR_EL1. The four groups are
 * the exception level and stack we came from; within each, the four kinds.
 */

/* Save the general-purpose registers so a handler can print them, and so an
 * IRQ can return to exactly where it interrupted. 256 bytes: x0-x29, x30,
 * and the two we need to describe where we came from. */
.macro SAVE_REGS
    sub     sp, sp, #272
    stp     x0,  x1,  [sp, #16 * 0]
    stp     x2,  x3,  [sp, #16 * 1]
    stp     x4,  x5,  [sp, #16 * 2]
    stp     x6,  x7,  [sp, #16 * 3]
    stp     x8,  x9,  [sp, #16 * 4]
    stp     x10, x11, [sp, #16 * 5]
    stp     x12, x13, [sp, #16 * 6]
    stp     x14, x15, [sp, #16 * 7]
    stp     x16, x17, [sp, #16 * 8]
    stp     x18, x19, [sp, #16 * 9]
    stp     x20, x21, [sp, #16 * 10]
    stp     x22, x23, [sp, #16 * 11]
    stp     x24, x25, [sp, #16 * 12]
    stp     x26, x27, [sp, #16 * 13]
    stp     x28, x29, [sp, #16 * 14]
    mrs     x0, elr_el1
    mrs     x1, spsr_el1
    stp     x30, x0,  [sp, #16 * 15]
    str     x1,       [sp, #16 * 16]
.endm

.macro RESTORE_REGS
    ldr     x1,       [sp, #16 * 16]
    ldp     x30, x0,  [sp, #16 * 15]
    msr     elr_el1, x0
    msr     spsr_el1, x1
    ldp     x0,  x1,  [sp, #16 * 0]
    ldp     x2,  x3,  [sp, #16 * 1]
    ldp     x4,  x5,  [sp, #16 * 2]
    ldp     x6,  x7,  [sp, #16 * 3]
    ldp     x8,  x9,  [sp, #16 * 4]
    ldp     x10, x11, [sp, #16 * 5]
    ldp     x12, x13, [sp, #16 * 6]
    ldp     x14, x15, [sp, #16 * 7]
    ldp     x16, x17, [sp, #16 * 8]
    ldp     x18, x19, [sp, #16 * 9]
    ldp     x20, x21, [sp, #16 * 10]
    ldp     x22, x23, [sp, #16 * 11]
    ldp     x24, x25, [sp, #16 * 12]
    ldp     x26, x27, [sp, #16 * 13]
    ldp     x28, x29, [sp, #16 * 14]
    add     sp, sp, #272
.endm

/* Anything that is not an IRQ ends up here: report it and stop. A kernel that
 * carries on after a fault it does not understand corrupts something else and
 * fails later, somewhere unrelated. */
.macro FATAL kind
    SAVE_REGS
    mov     x0, #\kind
    mov     x1, sp
    bl      exception_fatal
    b       .
.endm

.macro IRQ_ENTRY
    SAVE_REGS
    bl      irq_dispatch
    RESTORE_REGS
    eret
.endm

    .section .text
    .align 11                      /* VBAR_EL1 needs 2 KiB alignment */
    .global vector_table
    .hidden vector_table
vector_table:
    /* From EL1 with SP_EL0 — we never run this way, so any arrival is a bug. */
    .align 7 ; FATAL 0
    .align 7 ; FATAL 1
    .align 7 ; FATAL 2
    .align 7 ; FATAL 3

    /* From EL1 with SP_EL1 — the kernel itself. */
    .align 7 ; FATAL 4             /* synchronous: a fault in kernel code   */
    .align 7 ; IRQ_ENTRY           /* IRQ: the timer, and later devices     */
    .align 7 ; FATAL 6             /* FIQ                                   */
    .align 7 ; FATAL 7             /* SError                                */

    /* From EL0, AArch64 — userspace. Syscalls will arrive at entry 8. */
    .align 7 ; FATAL 8
    .align 7 ; IRQ_ENTRY
    .align 7 ; FATAL 10
    .align 7 ; FATAL 11

    /* From EL0, AArch32 — we do not support it; say so rather than crash. */
    .align 7 ; FATAL 12
    .align 7 ; FATAL 13
    .align 7 ; FATAL 14
    .align 7 ; FATAL 15

    .global exceptions_install
    .hidden exceptions_install
exceptions_install:
    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    msr     vbar_el1, x0
    isb
    ret
