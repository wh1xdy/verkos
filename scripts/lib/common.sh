#!/usr/bin/env bash
# VerkOS build system — shared library.
# Sourced by every stage script. Provides logging, environment setup, source
# fetching/verification, stamp tracking, and chroot helpers.
#
# Contract: callers set ARCH (default x86_64) before sourcing, or pass it via
# the environment. This file sets up all VERK_* / directory variables.

set -euo pipefail

# --- Locate the repo root regardless of where we're called from ------------
VERK_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERK_ROOT="$(cd "$VERK_LIB_DIR/../.." && pwd)"

# --- Colours / logging -----------------------------------------------------
if [ -t 1 ]; then
    C_RST='\033[0m'; C_BLU='\033[0;36m'; C_GRN='\033[0;32m'
    C_YLW='\033[0;33m'; C_RED='\033[0;31m'; C_BLD='\033[1m'
else
    C_RST=''; C_BLU=''; C_GRN=''; C_YLW=''; C_RED=''; C_BLD=''
fi

log()   { printf "${C_BLU}::${C_RST} %s\n" "$*"; }
step()  { printf "\n${C_BLD}${C_BLU}==>${C_RST} ${C_BLD}%s${C_RST}\n" "$*"; }
ok()    { printf "${C_GRN}✓${C_RST} %s\n" "$*"; }
warn()  { printf "${C_YLW}!${C_RST} %s\n" "$*" >&2; }
die()   { printf "${C_RED}✗ %s${C_RST}\n" "$*" >&2; exit 1; }

# --- Load config -----------------------------------------------------------
ARCH="${ARCH:-x86_64}"
[ -f "$VERK_ROOT/config/versions.sh" ] || die "config/versions.sh missing"
# shellcheck disable=SC1091
source "$VERK_ROOT/config/versions.sh"

TARGET_CONF="$VERK_ROOT/config/targets/${ARCH}.sh"
[ -f "$TARGET_CONF" ] || die "unknown ARCH '$ARCH' (no config/targets/${ARCH}.sh)"
# shellcheck disable=SC1090
source "$TARGET_CONF"

# --- Work directories (per-arch, git-ignored) ------------------------------
WORK="${WORK:-$VERK_ROOT/work}"
SRC_DIR="$WORK/sources"                 # shared across arches
ARCH_DIR="$WORK/$ARCH"
TOOLS_DIR="$ARCH_DIR/tools"             # the cross-toolchain
ROOTFS="$ARCH_DIR/rootfs"               # the target system
BUILD_DIR="$ARCH_DIR/build"             # scratch
OUT_DIR="$ARCH_DIR/out"                 # final artifacts
STAMP_DIR="$ARCH_DIR/.stamps"
LOCKFILE="$VERK_ROOT/config/versions.lock"

# How many parallel jobs to compile with.
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

mkdirs() { mkdir -p "$SRC_DIR" "$TOOLS_DIR" "$ROOTFS" "$BUILD_DIR" "$OUT_DIR" "$STAMP_DIR"; }

# --- Host vs target architecture -------------------------------------------
HOST_ARCH="$(uname -m)"
# The host's own target triple (for configure --build=...). gcc -dumpmachine is
# the same value LFS derives from config.guess, without needing each package's copy.
HOST_TRIPLE="$(cc -dumpmachine 2>/dev/null || gcc -dumpmachine 2>/dev/null || echo "$HOST_ARCH-pc-linux-gnu")"
is_native() {
    case "$ARCH:$HOST_ARCH" in
        x86_64:x86_64|i686:x86_64|i686:i686|aarch64:aarch64) return 0 ;;
        *) return 1 ;;
    esac
}

# --- Stamp tracking (skip completed stages) --------------------------------
stamp_done()  { [ -f "$STAMP_DIR/$1" ] && [ "${FORCE:-0}" != "1" ]; }
stamp_set()   { mkdir -p "$STAMP_DIR"; touch "$STAMP_DIR/$1"; }
run_stage() {
    # run_stage <name> <function>
    local name="$1" fn="$2"
    if stamp_done "$name"; then ok "stage '$name' already done (FORCE=1 to redo)"; return 0; fi
    step "stage: $name"
    "$fn"
    stamp_set "$name"
    ok "stage '$name' complete"
}

# --- Source URL resolution -------------------------------------------------
# Maps "<name>-<version>.tar.<ext>" style packages to a download URL.
source_url() {
    local pkg="$1"
    case "$pkg" in
        binutils-*)   echo "$URL_GNU/binutils/$pkg" ;;
        gcc-*)        echo "$URL_GNU/gcc/${pkg%.tar.*}/$pkg" ;;
        glibc-*)      echo "$URL_GNU/glibc/$pkg" ;;
        gmp-*)        echo "$URL_GNU/gmp/$pkg" ;;
        mpfr-*)       echo "$URL_GNU/mpfr/$pkg" ;;
        mpc-*)        echo "$URL_GNU/mpc/$pkg" ;;
        bash-*|coreutils-*|m4-*|bison-*|gawk-*|grep-*|sed-*|tar-*|gzip-*|make-*|findutils-*|diffutils-*|patch-*|gperf-*)
                      echo "$URL_GNU/${pkg%%-*}/$pkg" ;;
        linux-*)      echo "$URL_KERNEL/v${LINUX_VERSION%%.*}.x/$pkg" ;;
        ncurses-*)    echo "$URL_NCURSES/$pkg" ;;
        xz-*)         echo "$URL_XZ/v${XZ_VERSION}/$pkg" ;;
        systemd-*)    echo "$URL_SYSTEMD/v${SYSTEMD_VERSION}.tar.gz" ;;
        bzip2-*)      echo "$URL_SOURCEWARE/bzip2/$pkg" ;;
        dbus-*)       echo "$URL_DBUS/$pkg" ;;
        kmod-*)       echo "$URL_KMOD/$pkg" ;;
        util-linux-*) echo "$URL_UTILLINUX/v${UTIL_LINUX_VERSION%.*}/$pkg" ;;
        libcap-*)     echo "$URL_LIBCAP/$pkg" ;;
        expat-*)      echo "$URL_EXPAT/R_${EXPAT_VERSION//./_}/$pkg" ;;
        zlib-*)       echo "$URL_ZLIB/$pkg" ;;
        Python-*)     echo "$URL_PYTHON/${PYTHON_VERSION}/$pkg" ;;
        libffi-*)     echo "$URL_LIBFFI/v${LIBFFI_VERSION}/$pkg" ;;
        ninja-*)      echo "$URL_NINJA/v${NINJA_VERSION}.tar.gz" ;;
        *)            die "no download URL known for '$pkg' (add it to source_url in common.sh)" ;;
    esac
}

# --- Download + verify one tarball -----------------------------------------
fetch_one() {
    # fetch_one <tarball-name> <expected-sha256-or-empty>
    local pkg="$1" want="$2" url dest computed
    dest="$SRC_DIR/$pkg"
    url="$(source_url "$pkg")"

    if [ ! -f "$dest" ]; then
        log "downloading $pkg"
        if command -v curl >/dev/null; then
            curl -fL# -o "$dest.part" "$url" || die "download failed: $url"
        else
            wget -O "$dest.part" "$url" || die "download failed: $url"
        fi
        mv "$dest.part" "$dest"
    fi

    computed="$(sha256sum "$dest" | awk '{print $1}')"
    if [ -n "$want" ]; then
        [ "$computed" = "$want" ] || die "SHA-256 mismatch for $pkg:
    want: $want
    got:  $computed"
        ok "$pkg verified"
    else
        warn "$pkg has no pinned SHA-256 — recording $computed to versions.lock"
        printf '%s  %s\n' "$computed" "$pkg" >> "$LOCKFILE"
    fi
}

# --- Extract a tarball into BUILD_DIR and echo the resulting dir ------------
extract() {
    local pkg="$1" dir
    dir="$BUILD_DIR/$(tar tf "$SRC_DIR/$pkg" 2>/dev/null | head -1 | cut -d/ -f1)"
    rm -rf "$dir"
    tar -xf "$SRC_DIR/$pkg" -C "$BUILD_DIR"
    echo "$dir"
}

# --- Enter the target rootfs (native chroot or qemu-user chroot) -----------
enter_rootfs() {
    # enter_rootfs <command...>
    if is_native; then
        sudo chroot "$ROOTFS" /usr/bin/env -i \
            HOME=/root TERM="$TERM" PATH=/usr/bin:/usr/sbin \
            /bin/bash --login -c "$*"
    else
        # Foreign: rely on a statically-linked qemu-user copied into the rootfs
        # plus a registered binfmt_misc handler (set up by ensure_binfmt).
        local q="/usr/bin/${QEMU_USER}-static"
        [ -x "$ROOTFS$q" ] || die "foreign chroot needs $ROOTFS$q (run ensure_binfmt first)"
        sudo chroot "$ROOTFS" /usr/bin/env -i \
            HOME=/root TERM="$TERM" PATH=/usr/bin:/usr/sbin \
            /bin/bash --login -c "$*"
    fi
}

ensure_binfmt() {
    is_native && return 0
    local host_static="/usr/bin/${QEMU_USER}-static"
    [ -x "$host_static" ] || die "install qemu-user-static ($host_static not found)"
    sudo mkdir -p "$ROOTFS/usr/bin"
    sudo cp "$host_static" "$ROOTFS/usr/bin/"
    # Most distros auto-register binfmt via systemd-binfmt / update-binfmts.
    if [ ! -e "/proc/sys/fs/binfmt_misc/${BINFMT_NAME}" ]; then
        warn "binfmt handler ${BINFMT_NAME} not registered; install qemu-user-static's binfmt support"
    fi
}

export MAKEFLAGS="-j$JOBS"
