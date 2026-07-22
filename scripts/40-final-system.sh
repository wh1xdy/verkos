#!/usr/bin/env bash
# Stage 40 — build the final system natively inside the rootfs.
#
# Precondition: stage 30 produced enough temporary tools to chroot in. This
# stage enters the rootfs (native chroot, or qemu-user chroot for a foreign
# arch) and rebuilds the real system cleanly: glibc, the full toolchain, core
# userland, then the system layer (util-linux, D-Bus, systemd).
#
# This is the largest stage and maps to LFS chapters 8-9 plus the systemd parts
# of BLFS. The FINAL_SYSTEM_SEQUENCE below is the authoritative build order.
#
# STATUS: chroot entry + branding + the ordering are in place. Per-package
# native builds are being filled in (see docs/ROADMAP.md, Phase 1). The stage
# guards against pretending a partial system is complete.
source "$(dirname "$0")/lib/common.sh"
mkdirs

# The real system, in dependency order. glibc first (rebuilt natively), then
# the toolchain, then userland, then the init/system layer ending in systemd.
FINAL_SYSTEM_SEQUENCE=(
    # --- libc + toolchain (rebuilt natively, no longer cross) ---
    glibc zlib bzip2 xz binutils gcc
    # --- core userland ---
    ncurses bash coreutils diffutils file findutils gawk grep gzip
    make patch sed tar m4 bison flex
    # --- system libraries ---
    expat libcap kmod util-linux dbus
    # --- init / PID 1 ---
    systemd
)

prepare_rootfs_skeleton() {
    step "Preparing rootfs skeleton + branding"
    sudo install -d "$ROOTFS"/{boot,etc,var,root,tmp}
    sudo install -d "$ROOTFS"/usr/{bin,lib,sbin}
    sudo install -d "$ROOTFS"/{bin,lib,sbin}  # will be symlinks on merged-/usr
    # Merged-/usr symlinks (LFS default).
    for d in bin lib sbin; do
        [ -L "$ROOTFS/$d" ] || sudo ln -sfn "usr/$d" "$ROOTFS/$d" 2>/dev/null || true
    done
    # Branding.
    sudo cp "$VERK_ROOT/config/os-release" "$ROOTFS/etc/os-release"
    echo "VerkOS ${VERSION_ID:-0.1}" | sudo tee "$ROOTFS/etc/verk-release" >/dev/null
    printf 'verkos\n' | sudo tee "$ROOTFS/etc/hostname" >/dev/null
    ok "rootfs skeleton + /etc/os-release in place"
}

enter_and_verify() {
    step "Entering rootfs to sanity-check the chroot environment"
    if is_native; then
        log "native chroot into $ROOTFS"
    else
        ensure_binfmt
        log "foreign chroot into $ROOTFS via ${QEMU_USER}-static"
    fi
    # A tiny smoke test: can we run bash and see our branding?
    enter_rootfs 'echo "chroot OK: $(cat /etc/verk-release 2>/dev/null)"; \
                  /usr/bin/bash --version | head -1' \
        || die "could not enter rootfs — is stage 30 complete enough to chroot?"
}

prepare_rootfs_skeleton

# Guard: we only try the chroot if temp-tools left us a bash.
if [ ! -x "$ROOTFS/usr/bin/bash" ] && [ ! -x "$ROOTFS/bin/bash" ]; then
    warn "No bash in the rootfs yet — stage 30 (temp-tools) must complete first."
    warn "Skipping chroot. Nothing to build here until then."
    exit 0
fi

enter_and_verify

echo
warn "Final-system driver: chroot verified, branding installed."
warn "Native package builds (glibc→…→systemd) are being filled in — see"
warn "FINAL_SYSTEM_SEQUENCE in this file and docs/ROADMAP.md (Phase 1)."
warn "Not marking stage 40 complete until systemd builds and installs."
exit 0
