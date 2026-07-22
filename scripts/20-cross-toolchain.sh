#!/usr/bin/env bash
# Stage 20 — build the cross-compilation toolchain for $ARCH.
#
# This mirrors LFS "Compiling a Cross-Toolchain": a compiler that runs on the
# host but produces $VERK_TARGET binaries, installed into $TOOLS_DIR, isolated
# from the host so nothing leaks in. Everything after this is built *with* this
# toolchain.
#
# Order (each depends on the previous):
#   1. binutils (pass 1)   — the assembler + linker
#   2. gcc      (pass 1)   — a bare C compiler (no libc yet)
#   3. linux headers       — the kernel API glibc compiles against
#   4. glibc               — the C library, built with the pass-1 compiler
#   5. libstdc++           — the C++ stdlib (needs the fresh glibc)
source "$(dirname "$0")/lib/common.sh"
mkdirs

export PATH="$TOOLS_DIR/bin:$PATH"
export LC_ALL=POSIX
LFS_TGT="$VERK_TARGET"

# --- 1. binutils pass 1 ----------------------------------------------------
tc_binutils_pass1() {
    local d; d="$(extract "binutils-${BINUTILS_VERSION}.tar.xz")"
    mkdir -p "$d/build"; cd "$d/build"
    ../configure                    \
        --prefix="$TOOLS_DIR"       \
        --with-sysroot="$ROOTFS"    \
        --target="$LFS_TGT"         \
        --disable-nls               \
        --enable-gprofng=no         \
        --disable-werror
    make
    make install
}

# --- 2. gcc pass 1 ---------------------------------------------------------
tc_gcc_pass1() {
    local d; d="$(extract "gcc-${GCC_VERSION}.tar.xz")"
    # gcc bundles its math libs in-tree.
    tar -xf "$SRC_DIR/gmp-${GMP_VERSION}.tar.xz"  -C "$d"; mv "$d/gmp-${GMP_VERSION}"  "$d/gmp"
    tar -xf "$SRC_DIR/mpfr-${MPFR_VERSION}.tar.xz" -C "$d"; mv "$d/mpfr-${MPFR_VERSION}" "$d/mpfr"
    tar -xf "$SRC_DIR/mpc-${MPC_VERSION}.tar.gz"   -C "$d"; mv "$d/mpc-${MPC_VERSION}"   "$d/mpc"

    mkdir -p "$d/build"; cd "$d/build"
    ../configure                                 \
        --target="$LFS_TGT"                      \
        --prefix="$TOOLS_DIR"                    \
        --with-glibc-version="$GLIBC_VERSION"    \
        --with-sysroot="$ROOTFS"                 \
        --with-newlib                            \
        --without-headers                        \
        --enable-default-pie                     \
        --enable-default-ssp                     \
        --disable-nls --disable-shared           \
        --disable-multilib                       \
        --disable-threads --disable-libatomic    \
        --disable-libgomp --disable-libquadmath  \
        --disable-libssp --disable-libvtv        \
        --disable-libstdcxx                      \
        --enable-languages=c,c++                 \
        ${GCC_CONFIG_FLAGS}
    make
    make install
    # Produce the fixed limits.h the way LFS does.
    cd "$d"
    local libgcc incdir
    libgcc="$("$LFS_TGT-gcc" -print-libgcc-file-name)"
    incdir="$(dirname "$libgcc")/include"
    cat gcc/limitx.h gcc/glimits.h gcc/limity.h > "$incdir/limits.h"
}

# --- 3. Linux API headers --------------------------------------------------
tc_linux_headers() {
    local d; d="$(extract "linux-${LINUX_VERSION}.tar.xz")"
    cd "$d"
    # The kernel needs its OWN arch name (arm64, not aarch64; x86, not x86_64).
    # Without ARCH= it guesses from `uname -m`, which is fatal on arm64 hosts
    # (there is no arch/aarch64). LINUX_ARCH comes from config/targets/<arch>.sh.
    make ARCH="$LINUX_ARCH" mrproper
    make ARCH="$LINUX_ARCH" headers
    find usr/include -type f ! -name '*.h' -delete
    mkdir -p "$ROOTFS/usr"
    cp -rv usr/include "$ROOTFS/usr"
}

# --- 4. glibc --------------------------------------------------------------
tc_glibc() {
    local d; d="$(extract "glibc-${GLIBC_VERSION}.tar.xz")"
    cd "$d"
    # LFS creates arch-specific symlinks/dirs; keep it minimal + correct.
    case "$VERK_ARCH" in
        x86_64) ln -sfv ../lib/ld-linux-x86-64.so.2 "$ROOTFS/lib64" 2>/dev/null || true ;;
    esac
    mkdir -p build; cd build
    echo "rootsbindir=/usr/sbin" > configparms
    ../configure                              \
        --prefix=/usr                         \
        --host="$LFS_TGT"                     \
        --build="$(../scripts/config.guess)"  \
        --enable-kernel=4.19                  \
        --with-headers="$ROOTFS/usr/include"  \
        --disable-nscd                        \
        libc_cv_slibdir=/usr/lib
    make
    make DESTDIR="$ROOTFS" install
    # Fix a hard-coded path in the ldd script, as LFS does.
    sed '/RTLDLIST=/s@/usr@@g' -i "$ROOTFS/usr/bin/ldd" || true
}

# --- 5. libstdc++ (from the gcc pass-1 tree, now that glibc exists) --------
tc_libstdcxx() {
    local d="$BUILD_DIR/gcc-${GCC_VERSION}"
    [ -d "$d" ] || die "expected gcc tree at $d (run gcc pass1 first)"
    mkdir -p "$d/build-libstdcxx"; cd "$d/build-libstdcxx"
    ../libstdc++-v3/configure           \
        --host="$LFS_TGT"               \
        --build="$(../config.guess)"    \
        --prefix=/usr                   \
        --disable-multilib              \
        --disable-nls                   \
        --disable-libstdcxx-pch         \
        --with-gxx-include-dir="/usr/${LFS_TGT}/include/c++/${GCC_VERSION}"
    make
    make DESTDIR="$ROOTFS" install
    rm -v "$ROOTFS"/usr/lib/lib{stdc++{,exp,fs},supc++}.la 2>/dev/null || true
}

run_stage "toolchain-binutils1" tc_binutils_pass1
run_stage "toolchain-gcc1"      tc_gcc_pass1
run_stage "toolchain-headers"   tc_linux_headers
run_stage "toolchain-glibc"     tc_glibc
run_stage "toolchain-libstdcxx" tc_libstdcxx

echo
ok "Cross-toolchain for $ARCH installed in $TOOLS_DIR"
log "Sanity check: '$LFS_TGT-gcc --version' should work and target $VERK_TARGET"
