# VerkOS — convenience entry points over scripts/build.sh.
# The real logic lives in scripts/; this is a thin, discoverable wrapper.
#
#   make check   ARCH=x86_64     verify host prerequisites
#   make pin                     download every source, pin real SHA-256s
#   make fetch                   download + verify all sources
#   make toolchain ARCH=aarch64  build the cross-toolchain
#   make temp    ARCH=x86_64     temporary tools
#   make system  ARCH=x86_64     final native system (incl. systemd)
#   make kernel  ARCH=x86_64     build the Linux kernel
#   make image   ARCH=x86_64     assemble the bootable initramfs
#   make         ARCH=x86_64     full pipeline
#   make run     ARCH=x86_64     boot the result in QEMU
#   make clean   ARCH=x86_64     remove build scratch (keep sources+toolchain)
#   make distclean               remove everything under work/

ARCH   ?= x86_64
SHELL  := /bin/bash
BUILD  := scripts/build.sh

.PHONY: all check pin fetch toolchain temp system kernel image run clean distclean help

all:        ; @bash $(BUILD) all       $(ARCH)
check:      ; @bash $(BUILD) check     $(ARCH)
pin:        ; @bash scripts/pin-hashes.sh
fetch:      ; @bash $(BUILD) fetch     $(ARCH)
toolchain:  ; @bash $(BUILD) toolchain $(ARCH)
temp:       ; @bash $(BUILD) temp      $(ARCH)
system:     ; @bash $(BUILD) system    $(ARCH)
kernel:     ; @bash $(BUILD) kernel    $(ARCH)
image:      ; @bash $(BUILD) image     $(ARCH)

run:
	@bash boot/qemu-$(ARCH).sh

clean:
	@rm -rf work/$(ARCH)/build work/$(ARCH)/.stamps
	@echo "cleaned build scratch for $(ARCH) (sources + toolchain kept)"

distclean:
	@rm -rf work
	@echo "removed work/ entirely"

help:
	@sed -n '2,20p' Makefile
