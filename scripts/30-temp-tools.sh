#!/usr/bin/env bash
# Stage 30 — cross-compile a minimal set of temporary tools into the rootfs.
#
# These are just enough utilities, built WITH the stage-20 cross-toolchain, to
# let us chroot into the rootfs and finish the build natively (stage 40). This
# follows LFS "Cross Compiling Temporary Tools" + "Entering Chroot and Building
# Additional Temporary Tools".
#
# STATUS: driver + the first few packages are implemented as the template.
# The remaining packages are listed in TEMP_TOOLS_SEQUENCE below in the exact
# LFS order; fill them in following the same pattern. The script refuses to
# claim success past the implemented boundary so we never ship a half-rootfs.
source "$(dirname "$0")/lib/common.sh"
mkdirs

export PATH="$TOOLS_DIR/bin:$PATH"
export LC_ALL=POSIX
LFS_TGT="$VERK_TARGET"

# The canonical LFS temporary-tools order. Implemented ones have a tt_* function.
TEMP_TOOLS_SEQUENCE=(
    m4 ncurses bash coreutils diffutils file findutils gawk grep gzip
    make patch sed tar xz binutils_pass2 gcc_pass2
)

# --- Implemented: m4 (the pattern every cross-built package follows) --------
tt_m4() {
    local d; d="$(extract "m4-${M4_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr             \
        --host="$LFS_TGT"                 \
        --build="$(build/config.guess 2>/dev/null || ./build-aux/config.guess)"
    make
    make DESTDIR="$ROOTFS" install
}

# --- Implemented: ncurses --------------------------------------------------
tt_ncurses() {
    local d; d="$(extract "ncurses-${NCURSES_VERSION}.tar.gz")"; cd "$d"
    # Build 'tic' for the host first (LFS trick), then cross-build the libs.
    mkdir build
    pushd build; ../configure AWK=gawk; make -C include; make -C progs tic; popd
    ./configure --prefix=/usr --host="$LFS_TGT" \
        --build="$(./config.guess)"             \
        --mandir=/usr/share/man --with-manpage-format=normal \
        --with-shared --without-normal --with-cxx-shared \
        --without-debug --without-ada --disable-stripping AWK=gawk
    make
    make DESTDIR="$ROOTFS" TIC_PATH="$(pwd)/build/progs/tic" install
    ln -sfv libncursesw.so "$ROOTFS/usr/lib/libncurses.so" 2>/dev/null || true
}

# --- Implemented: bash -----------------------------------------------------
tt_bash() {
    local d; d="$(extract "bash-${BASH_VERSION_PKG}.tar.gz")"; cd "$d"
    ./configure --prefix=/usr --build="$(sh support/config.guess)" \
        --host="$LFS_TGT" --without-bash-malloc
    make
    make DESTDIR="$ROOTFS" install
    ln -sfv bash "$ROOTFS/usr/bin/sh"
}

# --- Implemented: coreutils ------------------------------------------------
tt_coreutils() {
    local d; d="$(extract "coreutils-${COREUTILS_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT" \
        --build="$(build-aux/config.guess)"     \
        --enable-install-program=hostname       \
        --enable-no-install-program=kill,uptime
    make
    make DESTDIR="$ROOTFS" install
}

step "Cross-building temporary tools for $ARCH"

# Run the implemented prefix of the sequence.
run_stage "temp-m4"         tt_m4
run_stage "temp-ncurses"    tt_ncurses
run_stage "temp-bash"       tt_bash
run_stage "temp-coreutils"  tt_coreutils

echo
warn "Temp-tools driver implemented through: coreutils."
warn "Remaining LFS temp-tools still to add (same pattern):"
warn "  diffutils file findutils gawk grep gzip make patch sed tar xz \\"
warn "  binutils(pass2) gcc(pass2)"
warn "See docs/ROADMAP.md — Phase 1. Not marking stage 30 fully complete."
# Intentionally do NOT create a global 'stage 30 done' stamp until the full
# sequence is implemented, so 'make' won't pretend the rootfs is chroot-ready.
exit 0
