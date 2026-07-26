/* stam — link map.
 *
 * 0x40080000 is where QEMU's virt machine puts a kernel: RAM starts at
 * 0x40000000 and the arm64 Image header asks for a 0x80000 offset. This is
 * the one place a physical address is written down, and it is a temporary
 * one: it holds only until the MMU comes up in milestone 2, after which the
 * kernel runs from a virtual address and the load address stops mattering.
 */
ENTRY(_start)
SECTIONS {
    . = 0x40080000;
    _kernel_start = .;
    .head.text : { KEEP(*(.head.text)) }
    .text      : { *(.text .text.*) }
    .rodata    : { *(.rodata .rodata.*) }
    .data      : { *(.data .data.*) }
    . = ALIGN(16);
    _bss_start = .;
    .bss       : { *(.bss .bss.*) *(COMMON) }
    . = ALIGN(16);
    _bss_end = .;
    . = ALIGN(4096);
    . = . + 0x4000;          /* 16 KiB boot stack */
    _stack_top = .;
    _kernel_end = .;
    /DISCARD/  : { *(.comment) *(.note*) *(.eh_frame*) }
}
