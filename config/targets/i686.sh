# VerkOS target: i686 (32-bit x86) — keeps us portable, revives old machines.
VERK_ARCH="i686"
VERK_TARGET="i686-verk-linux-gnu"

LINUX_ARCH="x86"
KERNEL_IMAGE="arch/x86/boot/bzImage"
KERNEL_DEFCONFIG="i386_defconfig"

# Target a real i686 baseline (not ancient i386) with SSE2 off for compatibility.
GCC_CONFIG_FLAGS="--with-arch=i686 --with-tune=generic"

QEMU_BIN="qemu-system-i386"
QEMU_USER="qemu-i386"
QEMU_MACHINE="pc"
QEMU_CPU="max"
QEMU_EXTRA="-serial mon:stdio -display none"
QEMU_CONSOLE="ttyS0"

BINFMT_NAME="qemu-i386"
