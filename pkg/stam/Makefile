# stam — the VerkOS kernel.
#
# Freestanding: no libc, no start files, and -mgeneral-regs-only so the
# compiler cannot quietly emit SIMD instructions into code that runs before
# the FPU has been enabled.
CROSS   ?=
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
# The two builds — raw Image and UEFI application — must share their code
# generation flags. They did not, and only the UEFI one had -fno-jump-tables:
# the compiler turned a switch into a table of absolute addresses, which the
# kernel jumped to after moving to the high half, where they point at nothing.
# It faulted in a loop, and only on one of the two paths. Anything that changes
# how addresses are formed belongs in COMMON.
COMMON   = -ffreestanding -nostdlib -mgeneral-regs-only -fno-stack-protector \
           -fno-jump-tables -fvisibility=hidden -O2 -Wall -std=c11
# -fpic here too, not just for the UEFI build. Without it the compiler builds
# tables of absolute pointers — a switch returning string constants becomes one
# — and those are link-time low addresses that stop resolving the moment the
# kernel moves to the high half. The exception decoder then cannot read its own
# messages, and faults while reporting a fault, forever.
CFLAGS   = $(COMMON) -fpic -Wextra -g
LDFLAGS  = -nostdlib -nostartfiles -static -Wl,-T,stam.ld -Wl,--build-id=none

OBJS = boot.o vectors.o exceptions.o klib.o fdt.o console.o handover_dtb.o mmu.o gic.o timer.o switch.o sched.o vm.o elf.o syscall.o userprog.o main.o

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
# -fvisibility=hidden: without it, taking the address of one of our own
# functions goes through the GOT, whose entries hold link-time addresses and
# are never relocated because nothing here processes relocations. The kernel
# then branches to where it used to live. Hidden symbols are addressed
# PC-relative instead, which is correct at any load address.
EFI_CFLAGS = $(COMMON) -fshort-wchar -fpic -I../vboot
EFI_SRCS   = handover_efi.c klib.c fdt.c console.c handover_dtb.c mmu.c gic.c timer.c sched.c vm.c elf.c syscall.c main.c exceptions.c
EFI_ASM    = vectors.S switch.S userprog.S

stam.efi: $(EFI_SRCS) $(EFI_ASM) stam-efi.lds
	@for a in $(EFI_ASM); do $(CC) $(EFI_CFLAGS) -c $$a -o efi-$${a%.S}.o || exit 1; done
	@for s in $(EFI_SRCS); do $(CC) $(EFI_CFLAGS) -c $$s -o efi-$${s%.c}.o || exit 1; done
	ld -nostdlib -znocombreloc -shared -Bsymbolic -T stam-efi.lds \
	    $(patsubst %.S,efi-%.o,$(EFI_ASM)) $(patsubst %.c,efi-%.o,$(EFI_SRCS)) -o stam-efi.so
	$(OBJCOPY) -j .text -j .data -j .reloc --target efi-app-aarch64 \
	    --subsystem=10 --file-alignment 0x1000 --section-alignment 0x1000 \
	    stam-efi.so $@
	python3 ../vboot/pe-fixup.py $@

clean-efi:
	rm -f efi-*.o stam-efi.so stam.efi

.PHONY: clean-efi

# ---- a program, compiled the ordinary way ---------------------------------
# Static, freestanding, linked at a fixed address. The kernel loads it out of
# the initrd, so it is a real ELF produced by the real compiler rather than
# bytes carried inside the kernel.
PROG_CFLAGS = -ffreestanding -nostdlib -static -fno-stack-protector -O2 -Wall
# --build-id=none: the note section is placed before .text and then overlaps
# it once -Ttext moves .text to a fixed address, which the linker refuses.
progs/hello.elf: progs/hello.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

progs/twice.elf: progs/twice.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

progs/forky.elf: progs/forky.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

progs: progs/hello.elf progs/twice.elf progs/forky.elf

run-prog: stam.img progs/hello.elf
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 -nographic \
	    -kernel stam.img -initrd progs/hello.elf

.PHONY: progs
