# VerkOS target: aarch64 (64-bit ARM) — real cheap hardware, e.g. Raspberry Pi.
VERK_ARCH="aarch64"
VERK_TARGET="aarch64-verk-linux-gnu"

LINUX_ARCH="arm64"
KERNEL_IMAGE="arch/arm64/boot/Image"
KERNEL_DEFCONFIG="defconfig"

GCC_CONFIG_FLAGS="--with-arch=armv8-a --enable-fix-cortex-a53-835769 --enable-fix-cortex-a53-843419"

# 'virt' is QEMU's clean, well-supported generic ARM board — ideal for dev.
QEMU_BIN="qemu-system-aarch64"
QEMU_USER="qemu-aarch64"
QEMU_MACHINE="virt"
QEMU_CPU="cortex-a72"
QEMU_EXTRA="-serial mon:stdio -display none"
QEMU_CONSOLE="ttyAMA0"

BINFMT_NAME="qemu-aarch64"
