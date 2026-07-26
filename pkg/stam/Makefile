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

OBJS = boot.o vectors.o exceptions.o klib.o fdt.o console.o handover_dtb.o mmu.o main.o

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
