/* stam — the context switch.
 *
 * The one thing C cannot express: change the stack pointer out from under the
 * running function, so that the `ret` at the end returns into a different
 * thread than the one that called.
 *
 * Only the callee-saved registers are here — x19-x28, the frame pointer and
 * the link register. The others do not need saving, because AAPCS64 says a
 * function call may destroy them, and cpu_switch is reached by a function
 * call. A thread interrupted by the timer has its caller-saved registers on
 * its own stack already, pushed by the exception vector.
 */

    .section .text
    .global cpu_switch
    .hidden cpu_switch
/* cpu_switch(uint64_t *save_sp_here, uint64_t resume_sp) */
cpu_switch:
    sub     sp, sp, #96
    stp     x19, x20, [sp, #0]
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     x29, x30, [sp, #80]
    mov     x2, sp
    str     x2, [x0]              /* remember where we left off       */
    mov     sp, x1                /* and stand on the other stack     */
    ldp     x19, x20, [sp, #0]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     x29, x30, [sp, #80]
    add     sp, sp, #96
    ret                           /* returns into the other thread    */

/* The runtime address of the trampoline, computed PC-relative.
 *
 * Taking &thread_trampoline from C put the address in a GOT entry instead,
 * even with the symbol hidden and local — and GOT entries hold link-time
 * addresses that nothing in this kernel relocates. A new thread then began
 * executing wherever the image happened to be linked, which under UEFI is
 * somewhere in the first megabyte, mapped as non-executable device memory.
 * The failure was an instruction abort at an address suspiciously close to
 * zero, which is the tell. Computed here, it cannot be wrong. */
    .global thread_entry_point
    .hidden thread_entry_point
thread_entry_point:
    adrp    x0, thread_trampoline
    add     x0, x0, :lo12:thread_trampoline
    ret

    .global thread_trampoline
    /* .hidden, or taking this address in C goes through the GOT, whose
     * entries hold link-time addresses that nothing here relocates. */
    .hidden thread_trampoline
/* Where a thread that has never run before begins. thread_create arranged for
 * cpu_switch to `ret` here, with the function in x19 and its argument in x20. */
thread_trampoline:
    /* The scheduler lock, held for us by whichever processor chose us. Same
     * reason as ret_to_user: a thread that has never run does not resume at
     * the line after cpu_switch, so it does not reach the unlock there. */
    bl      sched_first_run
    /* A new thread is first resumed from inside the timer interrupt, where the
     * hardware masked interrupts on entry. Without this it would run with them
     * masked forever — it would never be preempted, and the machine would look
     * like it had one thread that would not give way. */
    msr     daifclr, #2
    mov     x0, x20
    blr     x19
    mov     x0, #0
    bl      thread_exit
    b       .

/* Move the kernel to its high addresses and continue there.
 *
 *   x0 = the offset between a physical address and its high alias
 *
 * This does not return, and cannot: the stack it was called on is full of
 * return addresses in the low half, and the moment TTBR0 stops describing the
 * low half those become faults. So the old frames are abandoned. The stack
 * pointer is moved to the same memory seen through its high alias, and control
 * goes to kmain_high, which starts a fresh call chain.
 *
 * The address of kmain_high is computed here with adrp/add rather than passed
 * in from C — see userprog.S for why taking a symbol's address in C does not
 * survive in this kernel.
 */
    .global go_high
    .hidden go_high
go_high:
    mov     x2, sp
    add     x2, x2, x0
    mov     sp, x2
    adrp    x3, kmain_high
    add     x3, x3, :lo12:kmain_high
    add     x3, x3, x0
    br      x3
