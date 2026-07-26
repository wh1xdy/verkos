# stam — the VerkOS kernel.
#
# Freestanding: no libc, no start files, and -mgeneral-regs-only so the
# compiler cannot quietly emit SIMD instructions into code that runs before
# the FPU has been enabled.
CROSS   ?=
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
CFLAGS   = -ffreestanding -nostdlib -mgeneral-regs-only -fno-stack-protector \
           -fno-pic -fno-pie -O2 -Wall -Wextra -std=c11 -g
LDFLAGS  = -nostdlib -nostartfiles -static -Wl,-T,stam.ld -Wl,--build-id=none

OBJS = boot.o vectors.o exceptions.o klib.o fdt.o console.o handover_dtb.o mmu.o gic.o timer.o main.o

all: stam.img

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<
%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

stam.elf: $(OBJS) stam.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

# The raw Image a bootloader expects: the ELF stripped down to bytes, with
# our arm64 header at offset 0.
stam.img: stam.elf
	$(OBJCOPY) -O binary $< $@

run: stam.img
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 -nographic \
	    -kernel stam.img

clean:
	rm -f $(OBJS) stam.elf stam.img

.PHONY: all run clean

# ---- the same kernel, started by UEFI -------------------------------------
# A second link of the same objects, as a PE/COFF EFI application. -fpic and
# ImageBase 0, so the firmware may place it anywhere; pe-fixup.py clears
# IMAGE_FILE_RELOCS_STRIPPED, which objcopy sets and which no loader can
# honour at ImageBase 0. The recipe is vboot's, proved against AAVMF.
# -fshort-wchar: UEFI strings are 16-bit; without it L"..." is 32-bit on Linux
# and every message the firmware prints is garbage.
# -Bsymbolic at link time: without it an ADRP against a global in our own
# assembly is treated as possibly-external and the link fails.
EFI_CFLAGS = -ffreestanding -nostdlib -mgeneral-regs-only -fno-stack-protector \
             -fshort-wchar -fpic -fno-jump-tables -O2 -Wall -std=c11 -I../vboot
EFI_SRCS   = handover_efi.c klib.c fdt.c console.c handover_dtb.c mmu.c gic.c timer.c main.c exceptions.c
EFI_ASM    = vectors.S

stam.efi: $(EFI_SRCS) $(EFI_ASM) stam-efi.lds
	$(CC) $(EFI_CFLAGS) -c $(EFI_ASM) -o efi-vectors.o
	@for s in $(EFI_SRCS); do $(CC) $(EFI_CFLAGS) -c $$s -o efi-$${s%.c}.o || exit 1; done
	ld -nostdlib -znocombreloc -shared -Bsymbolic -T stam-efi.lds \
	    efi-vectors.o $(patsubst %.c,efi-%.o,$(EFI_SRCS)) -o stam-efi.so
	$(OBJCOPY) -j .text -j .data -j .reloc --target efi-app-aarch64 \
	    --subsystem=10 --file-alignment 0x1000 --section-alignment 0x1000 \
	    stam-efi.so $@
	python3 ../vboot/pe-fixup.py $@

clean-efi:
	rm -f efi-*.o stam-efi.so stam.efi

.PHONY: clean-efi
