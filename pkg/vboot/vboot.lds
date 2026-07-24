/* vboot.lds — linker script for a from-scratch aarch64 UEFI application.
 * Produces a PIE shared object (R_AARCH64_RELATIVE relocs) that objcopy then
 * converts to a PE/COFF efi-app. Keep .rela so objcopy can emit PE .reloc. */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(efi_main)
SECTIONS
{
  . = 0;
  ImageBase = .;
  .text : ALIGN(4096) {
    *(.text.head)
    *(.text)
    *(.text.*)
    *(.gnu.linkonce.t.*)
  }
  .rodata : ALIGN(4096) {
    *(.rodata)
    *(.rodata.*)
    *(.gnu.linkonce.r.*)
  }
  .data : ALIGN(4096) {
    *(.sdata)
    *(.data)
    *(.data.*)
    *(.gnu.linkonce.d.*)
    *(.got.plt)
    *(.got)
    *(.dynamic)
  }
  .rela : ALIGN(8) {
    *(.rela.text*) *(.rela.data*) *(.rela.rodata*)
    *(.rela.got) *(.rela.dyn) *(.rela)
  }
  .dynsym : ALIGN(8) { *(.dynsym) }
  .dynstr : ALIGN(8) { *(.dynstr) }
  .hash   : ALIGN(8) { *(.hash) *(.gnu.hash) }
  . = ALIGN(4096);
  .reloc : { *(.reloc) }
  /DISCARD/ : {
    *(.rel.reloc) *(.eh_frame) *(.note.*) *(.comment) *(.interp)
  }
}
