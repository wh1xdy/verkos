#!/usr/bin/env bash
# Stage 50 — configure and build the Linux kernel for $ARCH.
#
# Cross-compiles the kernel using the stage-20 toolchain (or the host toolchain
# when native). Produces the arch-appropriate image into $OUT_DIR.
source "$(dirname "$0")/lib/common.sh"
mkdirs

export PATH="$TOOLS_DIR/bin:$PATH"

# Use our cross-compiler when foreign; the plain host gcc when native.
if is_native; then
    export CROSS_COMPILE=""
else
    export CROSS_COMPILE="${VERK_TARGET}-"
fi
export ARCH="$LINUX_ARCH"   # NOTE: kernel wants its own ARCH name (x86/arm64)

build_kernel() {
    local d; d="$(extract "linux-${LINUX_VERSION}.tar.xz")"
    cd "$d"
    make mrproper

    # Start from a sane default config for this arch, then ensure the options a
    # minimal VerkOS needs (serial console, initramfs, tmpfs, ext4) are on.
    make "$KERNEL_DEFCONFIG"
    ./scripts/config --enable BLK_DEV_INITRD
    ./scripts/config --enable DEVTMPFS --enable DEVTMPFS_MOUNT
    ./scripts/config --enable TMPFS --enable EXT4_FS
    ./scripts/config --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE
    ./scripts/config --enable SERIAL_AMBA_PL011 --enable SERIAL_AMBA_PL011_CONSOLE
    ./scripts/config --enable PRINTK --enable BINFMT_ELF --enable BINFMT_SCRIPT
    # systemd needs cgroups + a few knobs.
    ./scripts/config --enable CGROUPS --enable INOTIFY_USER --enable SIGNALFD \
                     --enable TIMERFD --enable EPOLL --enable NET \
                     --enable SYSFS --enable PROC_FS --enable FHANDLE \
                     --enable AUTOFS_FS --enable TMPFS_POSIX_ACL
    make olddefconfig

    make -j"$JOBS"

    install -Dm644 "$KERNEL_IMAGE" "$OUT_DIR/vmlinuz-${LINUX_VERSION}"
    ln -sfn "vmlinuz-${LINUX_VERSION}" "$OUT_DIR/vmlinuz"
    # Stash the kernel modules into the rootfs (if it exists).
    if [ -d "$ROOTFS" ]; then
        make INSTALL_MOD_PATH="$ROOTFS" modules_install 2>/dev/null || \
            warn "modules_install skipped (rootfs not ready)"
    fi
    ok "kernel image → $OUT_DIR/vmlinuz-${LINUX_VERSION}"
}

run_stage "kernel-${LINUX_VERSION}" build_kernel
