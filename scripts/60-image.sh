#!/usr/bin/env bash
# Stage 60 — build a bootable root filesystem image from the rootfs.
#
# We boot QEMU with a direct kernel boot (-kernel) plus the rootfs on a virtio
# disk (root=/dev/vda), which scales to a full glibc+systemd system far better
# than stuffing multiple GB into an initramfs loaded entirely into RAM. GRUB +
# a partitioned disk for real hardware comes later (see boot/grub/).
source "$(dirname "$0")/lib/common.sh"
mkdirs

# Trim the rootfs down to what a shipped system actually needs. Runs here, after
# every stage has installed into it, so it catches the kernel modules too. Set
# VERK_NO_TRIM=1 to keep a fat development rootfs.
#
# This is worth a lot: unstripped binaries alone are most of the image (gcc's
# internals in /usr/libexec were 983 MB, 109 MB stripped). Measured effect on
# aarch64: 4.0 GB -> 603 MB of content, and a 5.1 GB -> 1.1 GB image file.
# Refuse to package a rootfs whose top level has entries nobody meant to put
# there. This is not hypothetical: vtar, given a compressed file it could not
# read, treated the compressed bytes as tar headers and created 36 files and
# directories named after that binary garbage — in whatever directory it was
# run from, which during a build is /. The extraction "succeeded" and the
# corruption reached a booted VM before anything enumerated / and noticed.
check_rootfs_sane() {
    local bad
    bad=$(ls -1 "$ROOTFS" | LC_ALL=C grep -cv '^[A-Za-z0-9._+-]*$' || true)
    [ "${bad:-0}" -eq 0 ] && return 0
    ls -1b "$ROOTFS" | LC_ALL=C grep -v '^[A-Za-z0-9._+-]*$' | head -5 | sed 's/^/    /' >&2
    die "$bad entries in $ROOTFS have names that are not plain text — the rootfs is corrupt, not shipping it"
}

# Nothing may be mounted inside the rootfs while it is trimmed. The trim does
# `rm -rf $ROOTFS/sources/*`, and $ROOTFS/sources is a bind mount of the shared,
# SHA-verified tarball cache during a build — so a leaked mount turns a cleanup
# into deleting every source the project has downloaded. It also silently
# packages whatever is mounted: that is how a 600 MB image became 4.0 GB.
check_rootfs_unmounted() {
    local mounted
    mounted=$(cut -d' ' -f2 /proc/mounts 2>/dev/null | grep "^${ROOTFS}/" || true)
    [ -z "$mounted" ] && return 0
    printf '%s\n' "$mounted" | sed 's/^/    /' >&2
    die "something is still mounted inside $ROOTFS — unmount it before building an image"
}

trim_rootfs() {
    [ "${VERK_NO_TRIM:-0}" = "1" ] && { log "trim skipped (VERK_NO_TRIM=1)"; return 0; }
    local SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
    local before after
    before=$($SUDO du -sm "$ROOTFS" | cut -f1)

    # Build-time leftovers and caches — none of this belongs in a shipped image.
    $SUDO rm -rf "$ROOTFS"/var/cache/vpk/* "$ROOTFS"/var/tmp/* "$ROOTFS"/tmp/* \
                 "$ROOTFS"/root/*.c "$ROOTFS"/sources/* 2>/dev/null || true

    # Message translations, info pages and package docs.
    $SUDO rm -rf "$ROOTFS"/usr/share/{locale,i18n,info,doc}

    # Python/meson/ninja are build tooling (dbus is built with meson); the running
    # system has no use for them. pip's wheels go too.
    $SUDO rm -rf "$ROOTFS"/usr/lib/python3.* "$ROOTFS"/usr/include/python3.* \
                 "$ROOTFS"/usr/lib/pkgconfig/python*.pc
    $SUDO rm -f  "$ROOTFS"/usr/lib/libpython3*.so* \
                 "$ROOTFS"/usr/bin/{python3,python3.*,python3-config,python3.*-config} \
                 "$ROOTFS"/usr/bin/{pip3,pip3.*,meson,ninja,wheel,idle3,idle3.*,2to3,2to3-*,pydoc3,pydoc3.*}

    # Strip debug info. The LFS recipe: never touch the dynamic loader, keep
    # symbols in libraries and modules, take everything from executables.
    local STRIP=strip
    command -v "${VERK_TARGET:-}-strip" >/dev/null 2>&1 && STRIP="${VERK_TARGET}-strip"
    $SUDO find "$ROOTFS"/usr/lib -type f -name '*.so*' ! -name 'ld-*' \
        -exec "$STRIP" --strip-unneeded {} ';' 2>/dev/null || true
    $SUDO find "$ROOTFS"/usr/lib -type f -name '*.a' \
        -exec "$STRIP" --strip-debug {} ';' 2>/dev/null || true
    $SUDO find "$ROOTFS"/usr/lib/modules -type f -name '*.ko' \
        -exec "$STRIP" --strip-debug {} + 2>/dev/null || true
    local d
    for d in bin sbin libexec; do
        [ -d "$ROOTFS/usr/$d" ] && $SUDO find "$ROOTFS/usr/$d" -type f ! -name 'ld-*' \
            -exec "$STRIP" --strip-all {} ';' 2>/dev/null || true
    done

    after=$($SUDO du -sm "$ROOTFS" | cut -f1)
    ok "trimmed rootfs: ${before} MB -> ${after} MB"
}

build_ext4_image() {
    [ -d "$ROOTFS" ] || die "no rootfs at $ROOTFS — build stages 30-40 first"
    command -v mkfs.ext4 >/dev/null || die "mkfs.ext4 (e2fsprogs) is required"
    check_rootfs_unmounted
    check_rootfs_sane
    trim_rootfs

    local img="$OUT_DIR/verkos-${ARCH}.ext4"
    # Size the image to the rootfs usage + 25% headroom (min 2 GB), so it also
    # has room to be used/written after boot.
    # Generous free space: rootfs + VERK_IMAGE_HEADROOM_MB (default 4 GiB), so
    # there's room to build/install packages with vpk on the running system.
    # The rootfs has root-owned 0700 dirs (/root, /var/lib/sshd, ...); reading them
    # (du) and preserving ownership (mkfs -d) needs root. When the build runs as a
    # normal user (e.g. on a build server) use sudo; when already root (container)
    # sudo isn't needed.
    local SUDO=; [ "$(id -u)" = 0 ] || SUDO=sudo
    local kb size_mb
    kb=$($SUDO du -sk "$ROOTFS" | cut -f1)
    # Headroom over the actual content. 512 MiB is plenty for a system that boots
    # and installs packages; override with VERK_IMAGE_HEADROOM_MB for a fat dev
    # image. (This used to default to 4 GiB, which made a 1 GiB system a 5 GiB file.)
    size_mb=$(( kb / 1024 + ${VERK_IMAGE_HEADROOM_MB:-512} ))
    [ "$size_mb" -lt 1024 ] && size_mb=1024

    $SUDO rm -f "$img"
    log "creating ${size_mb} MiB ext4 image from $ROOTFS"
    # -d populates the fs from a directory; run as root so ownership is preserved.
    $SUDO mkfs.ext4 -q -F -L VERKROOT -m 1 -d "$ROOTFS" "$img" "${size_mb}M"
    # mkfs ran as root, so the image is root-owned; hand it back to the build user
    # so `make run`/QEMU (unprivileged) can open it read-write to boot.
    [ -n "$SUDO" ] && $SUDO chown "$(id -u):$(id -g)" "$img"
    ok "root image → $img ($(du -h "$img" | cut -f1))"
}

run_stage "image-ext4" build_ext4_image

echo
ok "Boot it with:  make run ARCH=$ARCH   (or boot/qemu-$ARCH.sh)"
