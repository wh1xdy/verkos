#!/usr/bin/env bash
# Stage 30 — cross-compile the full set of temporary tools into the rootfs.
#
# Built WITH the stage-20 cross-toolchain, these are just enough utilities to
# chroot into the rootfs (stage 40) and finish natively. Mirrors LFS chapter 6
# "Cross Compiling Temporary Tools", in order. Every package uses the same shape:
#     configure --prefix=/usr --host=$LFS_TGT --build=$HOST_TRIPLE ...
#     make ; make DESTDIR=$ROOTFS install
source "$(dirname "$0")/lib/common.sh"
mkdirs

export PATH="$TOOLS_DIR/bin:$PATH"
export LC_ALL=POSIX
export CONFIG_SITE="$ROOTFS/usr/share/config.site"   # LFS: keeps configure honest
LFS_TGT="$VERK_TARGET"
B="$HOST_TRIPLE"

# --- m4 --------------------------------------------------------------------
tt_m4() {
    local d; d="$(extract "m4-${M4_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B"
    make; make DESTDIR="$ROOTFS" install
}

# --- ncurses (host 'tic' first, then cross the libraries) ------------------
tt_ncurses() {
    local d; d="$(extract "ncurses-${NCURSES_VERSION}.tar.gz")"; cd "$d"
    mkdir build
    pushd build >/dev/null; ../configure AWK=gawk; make -C include; make -C progs tic; popd >/dev/null
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B"      \
        --mandir=/usr/share/man --with-manpage-format=normal      \
        --with-shared --without-normal --with-cxx-shared          \
        --without-debug --without-ada --disable-stripping AWK=gawk
    make
    make DESTDIR="$ROOTFS" TIC_PATH="$(pwd)/build/progs/tic" install
    ln -sfv libncursesw.so "$ROOTFS/usr/lib/libncurses.so"
    # LFS: make -lcurses resolve to the wide-char lib.
    sed -e 's/^#if.*XOPEN.*$/#if 1/' -i "$ROOTFS/usr/include/curses.h" 2>/dev/null || true
}

# --- bash ------------------------------------------------------------------
tt_bash() {
    local d; d="$(extract "bash-${BASH_VERSION_PKG}.tar.gz")"; cd "$d"
    ./configure --prefix=/usr --build="$B" --host="$LFS_TGT" \
        --without-bash-malloc bash_cv_strtold_broken=no
    make; make DESTDIR="$ROOTFS" install
    ln -sfv bash "$ROOTFS/usr/bin/sh"
}

# --- coreutils -------------------------------------------------------------
tt_coreutils() {
    local d; d="$(extract "coreutils-${COREUTILS_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B" \
        --enable-install-program=hostname                    \
        --enable-no-install-program=kill,uptime              \
        gl_cv_macro_MB_CUR_MAX_good=y
    make; make DESTDIR="$ROOTFS" install
    # LFS: move a couple of tools to their FHS-correct locations.
    mv -v "$ROOTFS/usr/bin/chroot" "$ROOTFS/usr/sbin" 2>/dev/null || true
}

# --- diffutils -------------------------------------------------------------
tt_diffutils() {
    local d; d="$(extract "diffutils-${DIFFUTILS_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B"
    make; make DESTDIR="$ROOTFS" install
}

# --- file (needs a host copy of 'file' to cross-build) ---------------------
tt_file() {
    local d; d="$(extract "file-${FILE_VERSION}.tar.gz")"; cd "$d"
    mkdir build
    pushd build >/dev/null
    ../configure --disable-bzlib --disable-libseccomp \
        --disable-xzlib --disable-zlib
    make
    popd >/dev/null
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B"
    make FILE_COMPILE="$(pwd)/build/src/file"
    make DESTDIR="$ROOTFS" install
    rm -v "$ROOTFS"/usr/lib/libmagic.la 2>/dev/null || true
}

# --- findutils -------------------------------------------------------------
tt_findutils() {
    local d; d="$(extract "findutils-${FINDUTILS_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --localstatedir=/var/lib/locate \
        --host="$LFS_TGT" --build="$B"
    make; make DESTDIR="$ROOTFS" install
}

# --- gawk ------------------------------------------------------------------
tt_gawk() {
    local d; d="$(extract "gawk-${GAWK_VERSION}.tar.xz")"; cd "$d"
    sed -i 's/extras//' Makefile.in
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B"
    make; make DESTDIR="$ROOTFS" install
}

# --- grep ------------------------------------------------------------------
tt_grep() {
    local d; d="$(extract "grep-${GREP_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B"
    make; make DESTDIR="$ROOTFS" install
}

# --- gzip ------------------------------------------------------------------
tt_gzip() {
    local d; d="$(extract "gzip-${GZIP_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT"
    make; make DESTDIR="$ROOTFS" install
}

# --- make ------------------------------------------------------------------
tt_make() {
    local d; d="$(extract "make-${MAKE_VERSION}.tar.gz")"; cd "$d"
    ./configure --prefix=/usr --without-guile --host="$LFS_TGT" --build="$B"
    make; make DESTDIR="$ROOTFS" install
}

# --- patch -----------------------------------------------------------------
tt_patch() {
    local d; d="$(extract "patch-${PATCH_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B"
    make; make DESTDIR="$ROOTFS" install
}

# --- sed -------------------------------------------------------------------
tt_sed() {
    local d; d="$(extract "sed-${SED_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B"
    make; make DESTDIR="$ROOTFS" install
}

# --- tar -------------------------------------------------------------------
tt_tar() {
    local d; d="$(extract "tar-${TAR_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B"
    make; make DESTDIR="$ROOTFS" install
}

# --- xz --------------------------------------------------------------------
tt_xz() {
    local d; d="$(extract "xz-${XZ_VERSION}.tar.xz")"; cd "$d"
    ./configure --prefix=/usr --host="$LFS_TGT" --build="$B" \
        --disable-static --docdir="/usr/share/doc/xz-${XZ_VERSION}"
    make; make DESTDIR="$ROOTFS" install
    rm -v "$ROOTFS"/usr/lib/liblzma.la 2>/dev/null || true
}

# --- binutils pass 2 -------------------------------------------------------
tt_binutils_pass2() {
    local d; d="$(extract "binutils-${BINUTILS_VERSION}.tar.xz")"; cd "$d"
    sed '6031s/$add_dir//' -i ltmain.sh 2>/dev/null || true   # LFS libtool fix
    mkdir -p build; cd build
    ../configure --prefix=/usr --build="$B" --host="$LFS_TGT" \
        --disable-nls --enable-shared --enable-gprofng=no     \
        --disable-werror --enable-64-bit-bfd --enable-default-hash-style=gnu
    make; make DESTDIR="$ROOTFS" install
    rm -v "$ROOTFS"/usr/lib/lib{bfd,ctf,ctf-nobfd,opcodes,sframe}.{a,la} 2>/dev/null || true
}

# --- gcc pass 2 (full C/C++ compiler, targeting the rootfs) ----------------
tt_gcc_pass2() {
    local d; d="$(extract "gcc-${GCC_VERSION}.tar.xz")"
    tar -xf "$SRC_DIR/gmp-${GMP_VERSION}.tar.xz"  -C "$d"; mv "$d/gmp-${GMP_VERSION}"  "$d/gmp"
    tar -xf "$SRC_DIR/mpfr-${MPFR_VERSION}.tar.xz" -C "$d"; mv "$d/mpfr-${MPFR_VERSION}" "$d/mpfr"
    tar -xf "$SRC_DIR/mpc-${MPC_VERSION}.tar.gz"   -C "$d"; mv "$d/mpc-${MPC_VERSION}"   "$d/mpc"
    cd "$d"
    case "$VERK_ARCH" in
        x86_64) sed -e '/m64=/s/lib64/lib/' -i.orig gcc/config/i386/t-linux64 ;;
    esac
    mkdir -p build; cd build
    ../configure --build="$B" --host="$LFS_TGT" --target="$LFS_TGT" \
        LDFLAGS_FOR_TARGET=-L"$PWD/$LFS_TGT/libgcc"                 \
        --prefix=/usr --with-build-sysroot="$ROOTFS"               \
        --enable-default-pie --enable-default-ssp                  \
        --disable-nls --disable-multilib --disable-libatomic       \
        --disable-libgomp --disable-libquadmath --disable-libsanitizer \
        --disable-libssp --disable-libvtv --enable-languages=c,c++ \
        ${GCC_CONFIG_FLAGS}
    make; make DESTDIR="$ROOTFS" install
    ln -sfv gcc "$ROOTFS/usr/bin/cc"
}

step "Cross-building temporary tools for $ARCH (LFS chapter 6, full sequence)"

run_stage "temp-m4"            tt_m4
run_stage "temp-ncurses"       tt_ncurses
run_stage "temp-bash"          tt_bash
run_stage "temp-coreutils"     tt_coreutils
run_stage "temp-diffutils"     tt_diffutils
run_stage "temp-file"          tt_file
run_stage "temp-findutils"     tt_findutils
run_stage "temp-gawk"          tt_gawk
run_stage "temp-grep"          tt_grep
run_stage "temp-gzip"          tt_gzip
run_stage "temp-make"          tt_make
run_stage "temp-patch"         tt_patch
run_stage "temp-sed"           tt_sed
run_stage "temp-tar"           tt_tar
run_stage "temp-xz"            tt_xz
run_stage "temp-binutils2"     tt_binutils_pass2
run_stage "temp-gcc2"          tt_gcc_pass2

# All temp tools built — the rootfs is now chroot-ready.
stamp_set "temp-tools-complete"
echo
ok "Temporary tools complete — rootfs at $ROOTFS is ready to chroot into (stage 40)"
