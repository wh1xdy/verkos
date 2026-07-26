# vkern — the VerkOS kernel.
#
# Freestanding: no libc, no start files, and -mgeneral-regs-only so the
# compiler cannot quietly emit SIMD instructions into code that runs before
# the FPU has been enabled.
CROSS   ?=
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
CFLAGS   = -ffreestanding -nostdlib -mgeneral-regs-only -fno-stack-protector \
           -fno-pic -fno-pie -O2 -Wall -Wextra -std=c11 -g
LDFLAGS  = -nostdlib -nostartfiles -static -Wl,-T,vkern.ld -Wl,--build-id=none

OBJS = boot.o klib.o fdt.o console.o handover_dtb.o main.o

all: vkern.img

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<
%.o: %.S
	$(CC) $(CFLAGS) -c -o $@ $<

vkern.elf: $(OBJS) vkern.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

# The raw Image a bootloader expects: the ELF stripped down to bytes, with
# our arm64 header at offset 0.
vkern.img: vkern.elf
	$(OBJCOPY) -O binary $< $@

run: vkern.img
	qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 -nographic \
	    -kernel vkern.img

clean:
	rm -f $(OBJS) vkern.elf vkern.img

.PHONY: all run clean
