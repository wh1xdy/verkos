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
    # Graphics: virtio-gpu KMS + fbdev emulation so /dev/fb0 exists (VerkOS' first
    # GUI draws straight to it). Built-in, not modules, so it's up at boot. Also
    # virtio-input + evdev for keyboard/mouse into a future compositor. This is
    # what the arm64 defconfig was missing (DRM was a module, no virtio-gpu/input).
    # DRM_KMS_HELPER + INPUT must be enabled explicitly, else olddefconfig leaves
    # DRM as a module (=m) and drops the built-in virtio-gpu/input drivers.
    ./scripts/config --enable DRM --enable DRM_KMS_HELPER --enable DRM_VIRTIO_GPU \
                     --enable DRM_FBDEV_EMULATION --enable FB --enable FRAMEBUFFER_CONSOLE \
                     --enable INPUT --enable INPUT_EVDEV --enable VIRTIO_INPUT
    # Root on LUKS: the device mapper and its crypto target have to be in the
    # kernel image, not modules. A module would have to be loaded from inside
    # the initramfs before the root filesystem can be opened, which means
    # carrying the modules and a loader in there for no benefit.
    ./scripts/config --enable BLK_DEV_DM --enable DM_CRYPT \
                     --enable CRYPTO_XTS --enable CRYPTO_AES \
                     --enable CRYPTO_SHA256 --enable CRYPTO_SHA512 \
                     --enable CRYPTO_USER_API_SKCIPHER --enable CRYPTO_USER_API_HASH
    make olddefconfig

    make -j"$JOBS"

    install -Dm644 "$KERNEL_IMAGE" "$OUT_DIR/vmlinuz-${LINUX_VERSION}"
    ln -sfn "vmlinuz-${LINUX_VERSION}" "$OUT_DIR/vmlinuz"
    # Stash the kernel modules into the rootfs (if it exists).
    if [ -d "$ROOTFS" ]; then
        make INSTALL_MOD_PATH="$ROOTFS" modules_install 2>/dev/null || \
            warn "modules_install skipped (rootfs not ready)"
        local md="$ROOTFS/usr/lib/modules/${LINUX_VERSION}"
        # modules_install leaves build/ and source/ symlinks pointing into this
        # build tree — paths that do not exist on the installed system, and a
        # second copy of every .ko for anything that walks the directory.
        rm -f "$md/build" "$md/source"
        # Index the modules with our own depmod so modprobe resolves
        # dependencies on a freshly installed system, before anything runs
        # depmod itself. -b/RELEASE index a kernel other than the running one.
        if cc -O2 -o "$BUILD_DIR/verkmod-host" "$VERK_ROOT/pkg/verkmod/verkmod.c" 2>/dev/null; then
            "$BUILD_DIR/verkmod-host" depmod -v -b "$ROOTFS" "$LINUX_VERSION" ||
                warn "depmod failed — modules.dep not generated"
        else
            warn "verkmod would not build for the host; modules.dep not generated"
        fi
    fi
    ok "kernel image → $OUT_DIR/vmlinuz-${LINUX_VERSION}"
}

run_stage "kernel-${LINUX_VERSION}" build_kernel
