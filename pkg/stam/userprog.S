/* stam — the first program to run outside the kernel.
 *
 * Written in assembly and carried inside the kernel image as bytes, because an
 * ELF loader is a separate problem from getting to EL0 and back, and doing both
 * at once means not knowing which one is broken.
 *
 * It is position-independent: `adr` is PC-relative, so it runs correctly at
 * whatever virtual address the kernel maps it to. It uses Linux's aarch64
 * syscall numbers — 64 for write, 93 for exit — because that is the ABI worth
 * being compatible with, and choosing our own would be a decision we would only
 * have to undo.
 */
    .section .rodata
    .balign 4096
    .global user_prog_start
    .hidden user_prog_start
user_prog_start:
    /* write(1, msg, msg_len) */
    mov     x8, #64
    mov     x0, #1
    adr     x1, 1f
    mov     x2, #(2f - 1f)
    svc     #0

    /* getpid() — proves a syscall can return a value, not just have an effect */
    mov     x8, #172
    svc     #0
    mov     x19, x0

    /* write again, so the second syscall's return value is visible */
    mov     x8, #64
    mov     x0, #1
    adr     x1, 3f
    mov     x2, #(4f - 3f)
    svc     #0

    /* exit(with whatever getpid returned) */
    mov     x8, #93
    mov     x0, x19
    svc     #0

    /* exit must not return. If it does, stop here rather than fall into data. */
5:  b       5b

1:  .ascii  "  [user] hello from EL0, through svc\n"
2:
3:  .ascii  "  [user] and a second syscall came back with a value\n"
4:
    .balign 4
    .global user_prog_end
    .hidden user_prog_end
user_prog_end:

/* A program that goes looking where it is not allowed. The kernel is mapped
 * AP=00 — reachable from EL1, not from EL0 — so this load must fault, and the
 * fault must kill only this program. If it ever succeeds, the separation
 * between kernel and program is not real and nothing above it can be trusted.
 */
    .balign 4096
    .global user_bad_start
    .hidden user_bad_start
user_bad_start:
    mov     x8, #64
    mov     x0, #1
    adr     x1, 6f
    mov     x2, #(7f - 6f)
    svc     #0

    movz    x0, #0x4000, lsl #16        /* 0x40000000: kernel memory */
    ldr     x1, [x0]                    /* must not be permitted     */

    /* If we are still here the boundary did not hold. Say so loudly rather
     * than exiting quietly, which would look like success. */
    mov     x8, #64
    mov     x0, #1
    adr     x1, 8f
    mov     x2, #(9f - 8f)
    svc     #0
    mov     x8, #93
    mov     x0, #1
    svc     #0
0:  b       0b

6:  .ascii  "  [user] now reaching into kernel memory on purpose\n"
7:
8:  .ascii  "  [user] I READ KERNEL MEMORY - the boundary is not real\n"
9:
    .balign 4
    .global user_bad_end
    .hidden user_bad_end
user_bad_end:

/* Accessors, because this kernel has a rule: never take the address of a
 * symbol from C.
 *
 * It is built position-independent and nothing applies relocations to it, so
 * anything the compiler routes through the GOT — which is what it does for
 * `&symbol`, even when the symbol is hidden and local — holds the address the
 * image had when it was linked. Under UEFI that is somewhere in the first
 * megabyte, where nothing of ours lives. This was found twice: once as a
 * thread starting at 0x193c, and once as the first user program being copied
 * out of empty space and then faulting.
 *
 * Computed here with adrp/add, they are right wherever the image lands.
 */
    .global user_prog_addr
    .hidden user_prog_addr
user_prog_addr:
    adrp    x0, user_prog_start
    add     x0, x0, :lo12:user_prog_start
    ret

    .global user_prog_size
    .hidden user_prog_size
user_prog_size:
    adrp    x0, user_prog_start
    add     x0, x0, :lo12:user_prog_start
    adrp    x1, user_prog_end
    add     x1, x1, :lo12:user_prog_end
    sub     x0, x1, x0
    ret

    .global user_bad_addr
    .hidden user_bad_addr
user_bad_addr:
    adrp    x0, user_bad_start
    add     x0, x0, :lo12:user_bad_start
    ret

    .global user_bad_size
    .hidden user_bad_size
user_bad_size:
    adrp    x0, user_bad_start
    add     x0, x0, :lo12:user_bad_start
    adrp    x1, user_bad_end
    add     x1, x1, :lo12:user_bad_end
    sub     x0, x1, x0
    ret
