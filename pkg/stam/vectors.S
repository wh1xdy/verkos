/* stam — the exception vector table.
 *
 * Named vectors.S and not exceptions.S because exceptions.c exists: both would
 * build exceptions.o, make would pick one, and the link fails with an undefined
 * symbol that is plainly defined in a file you are looking at.
 *
 * This is written before the MMU on purpose. Once virtual memory is on, a
 * mistake in a page table is a fault, and a fault with no vector table
 * installed is a machine that resets or hangs with nothing on the console. The
 * order matters: install the vectors, then turn on the thing that can fault.
 *
 * The table has a shape the hardware knows about — sixteen entries, each
 * exactly 128 bytes, at a 2 KiB-aligned address in VBAR_EL1. The four groups
 * are the exception level and stack we came from; within each, the four kinds.
 *
 * THE 128 BYTES ARE A HARD LIMIT, and exceeding it fails quietly. An earlier
 * version put a whole save-and-restore sequence in each slot; those run to
 * about 160 bytes, so every entry after the first long one was pushed 128 bytes
 * down the table. Interrupts still worked, because the IRQ entry sits before
 * the overflow — and then the first system call from EL0 arrived at the handler
 * for an SError, which reported a perfectly real SVC as a fatal exception of
 * the wrong kind. The `.org` directives below turn that into a build error: the
 * assembler will not move its location counter backwards, so an entry that
 * outgrows its slot cannot be assembled at all.
 */

/* Each slot does the least it can: make room, save the two registers the common
 * code needs as scratch, record which entry it was, and jump. */
.macro VECTOR kind, target
    sub     sp, sp, #272
    stp     x0, x1, [sp, #16 * 0]
    mov     x0, #\kind
    b       \target
.endm

/* The rest of the register save, in the common path where there is room. x2
 * and x3 are stored before being reused as scratch for the two system registers
 * that describe where we came from. */
.macro SAVE_REST
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
    mrs     x2, elr_el1
    mrs     x3, spsr_el1
    stp     x30, x2,  [sp, #16 * 15]
    str     x3,       [sp, #16 * 16]
.endm

.macro RESTORE_ALL
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

    .section .text
    .align 11                      /* VBAR_EL1 needs 2 KiB alignment */
    .global vector_table
    .hidden vector_table
vector_table:
    /* From EL1 with SP_EL0 — we never run this way, so any arrival is a bug. */
    .org vector_table + 0x000 ; VECTOR 0,  fatal_common
    .org vector_table + 0x080 ; VECTOR 1,  fatal_common
    .org vector_table + 0x100 ; VECTOR 2,  fatal_common
    .org vector_table + 0x180 ; VECTOR 3,  fatal_common

    /* From EL1 with SP_EL1 — the kernel itself. */
    .org vector_table + 0x200 ; VECTOR 4,  fatal_common      /* fault in kernel code */
    .org vector_table + 0x280 ; VECTOR 5,  irq_common        /* timer, and devices   */
    .org vector_table + 0x300 ; VECTOR 6,  fatal_common      /* FIQ                  */
    .org vector_table + 0x380 ; VECTOR 7,  fatal_common      /* SError               */

    /* From EL0, AArch64 — userspace. System calls arrive at entry 8. */
    .org vector_table + 0x400 ; VECTOR 8,  user_sync_common
    .org vector_table + 0x480 ; VECTOR 9,  irq_common
    .org vector_table + 0x500 ; VECTOR 10, fatal_common
    .org vector_table + 0x580 ; VECTOR 11, fatal_common

    /* From EL0, AArch32 — unsupported; say so rather than crash. */
    .org vector_table + 0x600 ; VECTOR 12, fatal_common
    .org vector_table + 0x680 ; VECTOR 13, fatal_common
    .org vector_table + 0x700 ; VECTOR 14, fatal_common
    .org vector_table + 0x780 ; VECTOR 15, fatal_common
    .org vector_table + 0x800

/* Anything that is neither an IRQ nor a system call ends here: report it and
 * stop. A kernel that carries on after a fault it does not understand corrupts
 * something else and fails later, somewhere unrelated. */
fatal_common:
    SAVE_REST
    mov     x1, sp
    bl      exception_fatal
    b       .

irq_common:
    SAVE_REST
    bl      irq_dispatch
    RESTORE_ALL
    eret

/* Returns, unlike the others: the handler writes the system call's result into
 * the saved x0 and RESTORE_ALL hands it back to the program. */
user_sync_common:
    SAVE_REST
    mov     x0, sp
    bl      handle_user_sync
    RESTORE_ALL
    eret

/* Where a forked child begins. Its kernel stack was built to look exactly like
 * the parent's did in the middle of the system call, with x0 set to 0 instead
 * of the child's id, so restoring it and returning to EL0 resumes the same
 * instruction with a different answer. */
    .global ret_to_user
    .hidden ret_to_user
ret_to_user:
    RESTORE_ALL
    eret

/* Its address, PC-relative, for the same reason as everything else here. */
    .global ret_to_user_addr
    .hidden ret_to_user_addr
ret_to_user_addr:
    adrp    x0, ret_to_user
    add     x0, x0, :lo12:ret_to_user
    ret

    .global exceptions_install
    .hidden exceptions_install
exceptions_install:
    adrp    x0, vector_table
    add     x0, x0, :lo12:vector_table
    msr     vbar_el1, x0
    isb
    ret
