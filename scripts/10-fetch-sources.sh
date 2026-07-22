#!/usr/bin/env bash
# Stage 10 — download and verify every upstream source tarball.
# Sources are shared across architectures (work/sources/).
source "$(dirname "$0")/lib/common.sh"
mkdirs

step "Fetching sources into $SRC_DIR"

# Each line: fetch_one "<tarball>" "<sha256 var>"
# Tarball names are derived from config/versions.sh.
fetch_one "binutils-${BINUTILS_VERSION}.tar.xz"       "$BINUTILS_SHA256"
fetch_one "gcc-${GCC_VERSION}.tar.xz"                 "$GCC_SHA256"
fetch_one "gmp-${GMP_VERSION}.tar.xz"                 "$GMP_SHA256"
fetch_one "mpfr-${MPFR_VERSION}.tar.xz"               "$MPFR_SHA256"
fetch_one "mpc-${MPC_VERSION}.tar.gz"                 "$MPC_SHA256"
fetch_one "glibc-${GLIBC_VERSION}.tar.xz"             "$GLIBC_SHA256"
fetch_one "linux-${LINUX_VERSION}.tar.xz"             "$LINUX_SHA256"

fetch_one "m4-${M4_VERSION}.tar.xz"                   "$M4_SHA256"
fetch_one "ncurses-${NCURSES_VERSION}.tar.gz"         "$NCURSES_SHA256"
fetch_one "bash-${BASH_VERSION_PKG}.tar.gz"           "$BASH_SHA256"
fetch_one "coreutils-${COREUTILS_VERSION}.tar.xz"     "$COREUTILS_SHA256"
fetch_one "bison-${BISON_VERSION}.tar.xz"             "$BISON_SHA256"
fetch_one "gawk-${GAWK_VERSION}.tar.xz"               "$GAWK_SHA256"
fetch_one "grep-${GREP_VERSION}.tar.xz"               "$GREP_SHA256"
fetch_one "sed-${SED_VERSION}.tar.xz"                 "$SED_SHA256"
fetch_one "tar-${TAR_VERSION}.tar.xz"                 "$TAR_SHA256"
fetch_one "gzip-${GZIP_VERSION}.tar.xz"               "$GZIP_SHA256"
fetch_one "xz-${XZ_VERSION}.tar.xz"                   "$XZ_SHA256"
fetch_one "make-${MAKE_VERSION}.tar.gz"               "$MAKE_SHA256"
fetch_one "findutils-${FINDUTILS_VERSION}.tar.xz"     "$FINDUTILS_SHA256"
fetch_one "diffutils-${DIFFUTILS_VERSION}.tar.xz"     "$DIFFUTILS_SHA256"

# System layer (built in the chroot; downloaded here).
fetch_one "systemd-${SYSTEMD_VERSION}.tar.gz"         "$SYSTEMD_SHA256"

echo
ok "All sources present in $SRC_DIR"
[ -f "$LOCKFILE" ] && warn "Some packages had no pinned hash — see $LOCKFILE and paste values into config/versions.sh"
