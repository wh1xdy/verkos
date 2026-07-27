# stam — the VerkOS kernel.
#
# Freestanding: no libc, no start files, and -mgeneral-regs-only so the
# compiler cannot quietly emit SIMD instructions into code that runs before
# the FPU has been enabled.
CROSS   ?=
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
# The EFI link goes through ld directly, so it needs the prefix too. Missing
# it means the host linker is handed aarch64 objects, which fails in a way
# that names neither the linker nor the architecture.
LD       = $(CROSS)ld
# The two builds — raw Image and UEFI application — must share their code
# generation flags. They did not, and only the UEFI one had -fno-jump-tables:
# the compiler turned a switch into a table of absolute addresses, which the
# kernel jumped to after moving to the high half, where they point at nothing.
# It faulted in a loop, and only on one of the two paths. Anything that changes
# how addresses are formed belongs in COMMON.
#
# -mstrict-align belongs to the same family and cost a boot to find. The kernel
# runs with the MMU off until it has read the device tree, and before the MMU
# everything is Device memory, where an unaligned access is a fault rather than
# a slower load. Left to itself the compiler merges adjacent fields: GCC 12
# turned `addr_cells = 2; size_cells = 1;` — two 32-bit stores at offsets 0x1c
# and 0x20 of fdt_t — into one 8-byte `stur x3, [x0, #0x1c]`, which is legal
# with the MMU on and an alignment fault before it. It faulted inside
# fdt_init, before exception vectors existed, so the machine branched to 0x200
# and hit an undefined instruction: no output at all, which is the most
# expensive failure this kernel can produce. Whether it happens depends on the
# compiler's mood about store merging, so it built and booted with one GCC and
# died with the next.
COMMON   = -ffreestanding -nostdlib -mgeneral-regs-only -fno-stack-protector \
           -fno-jump-tables -fvisibility=hidden -mstrict-align -O2 -Wall -std=c11
# -fpic here too, not just for the UEFI build. Without it the compiler builds
# tables of absolute pointers — a switch returning string constants becomes one
# — and those are link-time low addresses that stop resolving the moment the
# kernel moves to the high half. The exception decoder then cannot read its own
# messages, and faults while reporting a fault, forever.
CFLAGS   = $(COMMON) -fpic -Wextra -g
LDFLAGS  = -nostdlib -nostartfiles -static -Wl,-T,stam.ld -Wl,--build-id=none

OBJS = boot.o smpentry.o vectors.o exceptions.o klib.o fdt.o console.o handover_dtb.o mmu.o gic.o timer.o switch.o sched.o vm.o elf.o cpio.o file.o smp.o signal.o virtio.o gpt.o fat.o syscall.o userprog.o main.o

all: stam.img

# Every object depends on every header. Coarse on purpose: this kernel has
# fourteen of them and a rebuild costs seconds, while the alternative is what
# happened on 2026-07-28 — thread_t grew two fields, sched.o was rebuilt and
# console.o was not, and the two disagreed about where `slices` lived. It
# presented as a scheduler reporting two slices where there had been fifty and
# a processor index of 101, which is not a number any code here can produce.
HDRS = $(wildcard *.h)

%.o: %.c $(HDRS)
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
EFI_SRCS   = handover_efi.c klib.c fdt.c console.c handover_dtb.c mmu.c gic.c timer.c sched.c vm.c elf.c cpio.c file.c smp.c signal.c virtio.c gpt.c fat.c syscall.c main.c exceptions.c
EFI_ASM    = vectors.S switch.S userprog.S smpentry.S

stam.efi: $(EFI_SRCS) $(EFI_ASM) stam-efi.lds
	@for a in $(EFI_ASM); do $(CC) $(EFI_CFLAGS) -c $$a -o efi-$${a%.S}.o || exit 1; done
	@for s in $(EFI_SRCS); do $(CC) $(EFI_CFLAGS) -c $$s -o efi-$${s%.c}.o || exit 1; done
	$(LD) -nostdlib -znocombreloc -shared -Bsymbolic -T stam-efi.lds \
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
# -mgeneral-regs-only, for the same reason the kernel is built with it: stam
# never enables FP/SIMD for EL0 (CPACR_EL1 is left alone), so any such
# instruction traps as EC 0x7 and the program is killed. The compiler reaches
# for a q register unprompted — initialising a two-element array of pointers was
# enough — so a program can die on a line containing no arithmetic at all.
# Either the kernel enables FP or userspace must not use it; this is the second,
# and it is the honest one while nothing needs floating point.
PROG_CFLAGS = -ffreestanding -nostdlib -static -fno-stack-protector \
              -mgeneral-regs-only -O2 -Wall
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

progs/init.elf: progs/init.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

# Reads standard input and writes it back in capitals: the right-hand side of
# a pipe, and identical whether that is a terminal or another program.
progs/upper.elf: progs/upper.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

# Raises a signal and checks the process died of it.
progs/sigtest.elf: progs/sigtest.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

# Counts forever, so that there is something to interrupt.
progs/spin.elf: progs/spin.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

# Catches a signal in a loop that makes no system calls, and checks that every
# register came back — a handler that runs and loses the frame is worse than no
# handler, and only the exit status can tell the two apart.
progs/catch.elf: progs/catch.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

# Hands the kernel addresses that are not its own and checks it is refused.
progs/badptr.elf: progs/badptr.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

# The initrd as every Linux initramfs is: a newc cpio archive, written by
# VerkOS' own vcpio. Names have no leading slash, which is the convention;
# stam accepts either when a program asks.
progs/vsh.elf: progs/vsh.c
	$(CC) $(PROG_CFLAGS) -Wl,-Ttext=0x400000 -Wl,-e,_start \
	    -Wl,--build-id=none -o $@ $<

# A third archive: the shell plus an /rc it runs and exits from. This is what
# makes the shell testable at all — no fifo, no sleeps, no interactive session
# to synchronise with. The commands are in progs/rc, in the repository, where
# they can be read.
progs/initrd-script.cpio: progs/vsh.elf progs/hello.elf progs/upper.elf \
                          progs/forky.elf progs/twice.elf progs/badptr.elf \
                          progs/catch.elf progs/rc
	@cc -O1 -o /tmp/vcpio-host ../vcpio/vcpio.c 2>/dev/null || \
	    { echo "vcpio did not build"; exit 1; }
	@rm -rf progs/scriptroot && mkdir -p progs/scriptroot
	@cp progs/vsh.elf progs/scriptroot/init
	@cp progs/hello.elf progs/scriptroot/hello
	@cp progs/upper.elf progs/scriptroot/upper
	@cp progs/spin.elf progs/scriptroot/spin
	@cp progs/sigtest.elf progs/scriptroot/sigtest
	@cp progs/forky.elf progs/scriptroot/forky
	@cp progs/twice.elf progs/scriptroot/twice
	@cp progs/badptr.elf progs/scriptroot/badptr
	@cp progs/catch.elf progs/scriptroot/catch
	@cp progs/rc progs/scriptroot/rc
	( cd progs/scriptroot && find . -type f | sed 's|^\./||' | /tmp/vcpio-host -o ) > $@

# A second archive whose /init is the shell. Kept separate so the automatic
# demonstration still ends by itself: a shell waits for a person, and a test
# that waits for a person never finishes.
progs/initrd-shell.cpio: progs/vsh.elf progs/hello.elf progs/forky.elf progs/twice.elf progs/badptr.elf progs/upper.elf progs/spin.elf progs/sigtest.elf
	@cc -O1 -o /tmp/vcpio-host ../vcpio/vcpio.c 2>/dev/null || \
	    { echo "vcpio did not build"; exit 1; }
	@rm -rf progs/sroot && mkdir -p progs/sroot
	@cp progs/vsh.elf progs/sroot/init
	@cp progs/hello.elf progs/sroot/hello
	@cp progs/forky.elf progs/sroot/forky
	@cp progs/twice.elf progs/sroot/twice
	@cp progs/badptr.elf progs/sroot/badptr
	@cp progs/upper.elf progs/sroot/upper
	@cp progs/spin.elf progs/sroot/spin
	@cp progs/sigtest.elf progs/sroot/sigtest
	( cd progs/sroot && find . -type f | sed 's|^\./||' | /tmp/vcpio-host -o ) > $@

progs/initrd.cpio: progs/init.elf progs/hello.elf progs/forky.elf progs/badptr.elf
	@cc -O1 -o /tmp/vcpio-host ../vcpio/vcpio.c 2>/dev/null || \
	    { echo "vcpio did not build"; exit 1; }
	@rm -rf progs/root && mkdir -p progs/root
	@cp progs/init.elf progs/root/init
	@cp progs/hello.elf progs/root/hello
	@cp progs/forky.elf progs/root/forky
	@cp progs/badptr.elf progs/root/badptr
	( cd progs/root && find . -type f | sed 's|^\./||' | /tmp/vcpio-host -o ) > $@

progs: progs/hello.elf progs/twice.elf progs/forky.elf progs/init.elf progs/vsh.elf \
       progs/badptr.elf progs/upper.elf progs/spin.elf progs/sigtest.elf \
       progs/initrd.cpio progs/initrd-shell.cpio progs/initrd-script.cpio

run-prog: stam.img progs/hello.elf
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 -nographic \
	    -kernel stam.img -initrd progs/hello.elf

.PHONY: progs

# An interactive VerkOS: stam, with a shell as its first program.
shell: stam.img progs/initrd-shell.cpio
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 -nographic \
	    -kernel stam.img -initrd progs/initrd-shell.cpio

.PHONY: shell
