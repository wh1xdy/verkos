# VerkOS target: x86_64 (64-bit PC) — the primary development target.
VERK_ARCH="x86_64"
VERK_TARGET="x86_64-verk-linux-gnu"

# Linux kernel arch name + built image path
LINUX_ARCH="x86"
KERNEL_IMAGE="arch/x86/boot/bzImage"
KERNEL_DEFCONFIG="x86_64_defconfig"

# GCC configuration knobs for this arch
GCC_CONFIG_FLAGS=""

# QEMU: system emulator + user-mode binary (for foreign builds on other hosts)
QEMU_BIN="qemu-system-x86_64"
QEMU_USER="qemu-x86_64"
QEMU_MACHINE="q35"
QEMU_CPU="max"
QEMU_EXTRA="-serial mon:stdio -display none"
QEMU_CONSOLE="ttyS0"

# binfmt_misc magic (only needed when building this arch on a foreign host)
BINFMT_NAME="qemu-x86_64"
