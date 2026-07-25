/* vboot.lds — linker script for a from-scratch aarch64 UEFI application.
 * Produces a PIE object that objcopy converts to a PE/COFF efi-app.
 *
 * KEY: the first section must start at RVA 0x1000 (SectionAlignment), leaving
 * the opening page for the PE/COFF headers. A strict EDK2/AAVMF loader rejects
 * an image whose .text sits at RVA 0 (overlapping the headers) with
 * EFI_UNSUPPORTED — this is exactly how the distro's systemd-bootaa64.efi is
 * laid out (.text @ 0x1000, SizeOfHeaders=4096). */
/* No OUTPUT_FORMAT/OUTPUT_ARCH: ld takes them from the input objects, so this
 * one script serves both the aarch64 and the x86_64 build. */
ENTRY(efi_main)
SECTIONS
{
  . = 0;
  ImageBase = .;
  . = 0x1000;                    /* reserve the first page for PE/COFF headers */
  .text : {
    *(.text.head)
    *(.text) *(.text.*) *(.gnu.linkonce.t.*)
    *(.rodata) *(.rodata.*) *(.gnu.linkonce.r.*)   /* fold rodata into R+X text */
  }
  . = ALIGN(0x1000);
  .data : {
    *(.sdata) *(.data) *(.data.*) *(.gnu.linkonce.d.*)
    *(.got.plt) *(.got)
    /* .bss MUST live inside a loaded section. objcopy only emits PROGBITS, so a
     * separate NOBITS .bss would fall outside SizeOfImage and every global would
     * be written to memory the firmware never allocated. Folding it in here
     * makes ld materialise it as zeros in the image. */
    *(.bss) *(.bss.*) *(COMMON)
    . = ALIGN(0x1000);
  }
  . = ALIGN(0x1000);
  .reloc : { *(.reloc) }

  /* Dynamic-linking metadata must be kept OUT of .data. We only link -shared to
   * get position-independent code; the firmware never consults these. Left in
   * .data, ld lays the string/symbol tables over the START of our real data —
   * that silently overwrote the initrd device-path header (its Length field
   * became garbage, so the firmware's path walk never terminated). They live in
   * their own sections here, past .data, and objcopy simply does not copy them. */
  .dynamic : { *(.dynamic) }
  .dynsym  : { *(.dynsym) }
  .dynstr  : { *(.dynstr) }
  .hashtab : { *(.hash) *(.gnu.hash) }
  .relatab : { *(.rela) *(.rela.*) }

  /DISCARD/ : {
    *(.rel.reloc) *(.eh_frame) *(.note.*) *(.comment) *(.interp)
  }
}
