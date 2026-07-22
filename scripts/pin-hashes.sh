#!/usr/bin/env bash
# Pin real SHA-256 checksums into config/versions.sh.
#
# Downloads every source tarball via the same source_url() logic the build uses,
# computes its SHA-256, and writes it back into config/versions.sh in place.
# Tarballs are hashed then deleted, so this needs bandwidth but little disk.
#
# Doubles as a version/URL validator: a 404 means the version or URL template in
# versions.sh is wrong. Failures are collected and reported; the script never
# writes a hash it didn't actually compute.
#
# Portable: runs on Linux or macOS (sha256sum/shasum, GNU/BSD sed).
#
#   scripts/pin-hashes.sh            # pin everything
#   scripts/pin-hashes.sh --dry-run  # download+hash, print, don't edit versions.sh
#   scripts/pin-hashes.sh --only gcc,glibc   # just these packages
source "$(dirname "$0")/lib/common.sh"

DRY=0; ONLY=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1; shift ;;
        --only)    ONLY="$2"; shift 2 ;;
        *) die "unknown option '$1' (use --dry-run, --only <a,b,c>)" ;;
    esac
done

VERSIONS_FILE="$VERK_ROOT/config/versions.sh"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

# --- Portable helpers (Linux + macOS) --------------------------------------
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{print $1}'
    else shasum -a 256 "$1" | awk '{print $1}'; fi
}
# In-place sed that works with both GNU and BSD (macOS) sed.
sed_inplace() {
    if sed --version >/dev/null 2>&1; then sed -i -E "$1" "$2"
    else sed -i '' -E "$1" "$2"; fi
}

# tarball-template  |  SHA-variable-name
PKGS=(
    "binutils-${BINUTILS_VERSION}.tar.xz|BINUTILS_SHA256"
    "gcc-${GCC_VERSION}.tar.xz|GCC_SHA256"
    "gmp-${GMP_VERSION}.tar.xz|GMP_SHA256"
    "mpfr-${MPFR_VERSION}.tar.xz|MPFR_SHA256"
    "mpc-${MPC_VERSION}.tar.gz|MPC_SHA256"
    "glibc-${GLIBC_VERSION}.tar.xz|GLIBC_SHA256"
    "linux-${LINUX_VERSION}.tar.xz|LINUX_SHA256"
    "m4-${M4_VERSION}.tar.xz|M4_SHA256"
    "ncurses-${NCURSES_VERSION}.tar.gz|NCURSES_SHA256"
    "bash-${BASH_VERSION_PKG}.tar.gz|BASH_SHA256"
    "coreutils-${COREUTILS_VERSION}.tar.xz|COREUTILS_SHA256"
    "bison-${BISON_VERSION}.tar.xz|BISON_SHA256"
    "gawk-${GAWK_VERSION}.tar.xz|GAWK_SHA256"
    "grep-${GREP_VERSION}.tar.xz|GREP_SHA256"
    "sed-${SED_VERSION}.tar.xz|SED_SHA256"
    "tar-${TAR_VERSION}.tar.xz|TAR_SHA256"
    "gzip-${GZIP_VERSION}.tar.xz|GZIP_SHA256"
    "xz-${XZ_VERSION}.tar.xz|XZ_SHA256"
    "make-${MAKE_VERSION}.tar.gz|MAKE_SHA256"
    "findutils-${FINDUTILS_VERSION}.tar.xz|FINDUTILS_SHA256"
    "diffutils-${DIFFUTILS_VERSION}.tar.xz|DIFFUTILS_SHA256"
    "patch-${PATCH_VERSION}.tar.xz|PATCH_SHA256"
    "flex-${FLEX_VERSION}.tar.gz|FLEX_SHA256"
    "file-${FILE_VERSION}.tar.gz|FILE_SHA256"
    "bzip2-${BZIP2_VERSION}.tar.gz|BZIP2_SHA256"
    "zlib-${ZLIB_VERSION}.tar.gz|ZLIB_SHA256"
    "gperf-${GPERF_VERSION}.tar.gz|GPERF_SHA256"
    "expat-${EXPAT_VERSION}.tar.xz|EXPAT_SHA256"
    "libcap-${LIBCAP_VERSION}.tar.xz|LIBCAP_SHA256"
    "kmod-${KMOD_VERSION}.tar.xz|KMOD_SHA256"
    "util-linux-${UTIL_LINUX_VERSION}.tar.xz|UTIL_LINUX_SHA256"
    "dbus-${DBUS_VERSION}.tar.xz|DBUS_SHA256"
    "libffi-${LIBFFI_VERSION}.tar.gz|LIBFFI_SHA256"
    "Python-${PYTHON_VERSION}.tar.xz|PYTHON_SHA256"
    "ninja-${NINJA_VERSION}.tar.gz|NINJA_SHA256"
    "systemd-${SYSTEMD_VERSION}.tar.gz|SYSTEMD_SHA256"
)

pinned=0; skipped=0; failed=()
# NB: use an explicit test, not ${DRY:+...} — DRY=0 is a non-empty string and
# would wrongly trigger the suffix.
DRY_LABEL=""; [ "$DRY" -eq 1 ] && DRY_LABEL=" (dry run)"
step "Pinning SHA-256 for ${#PKGS[@]} packages${DRY_LABEL}"

for entry in "${PKGS[@]}"; do
    tarball="${entry%%|*}"; var="${entry##*|}"
    if [ -n "$ONLY" ] && ! echo ",$ONLY," | grep -q ",${tarball%%-*},"; then
        skipped=$((skipped+1)); continue
    fi
    url="$(source_url "$tarball" 2>/dev/null)" || { warn "no URL for $tarball"; failed+=("$tarball"); continue; }
    printf '  %-34s ' "$tarball"
    # Bulk-downloading from one host invites rate limiting (ftp.gnu.org will
    # start refusing partway through a 36-package run), so retry generously with
    # backoff and pause briefly between packages.
    if ! curl -fsSL --retry 5 --retry-delay 3 --retry-all-errors \
              --connect-timeout 20 -o "$TMP/$tarball" "$url" 2>/dev/null; then
        printf '%bFAIL%b  %s\n' "$C_RED" "$C_RST" "$url"; failed+=("$tarball"); continue
    fi
    hash="$(sha256_of "$TMP/$tarball")"
    rm -f "$TMP/$tarball"
    printf '%b%s%b\n' "$C_GRN" "$hash" "$C_RST"
    [ "$DRY" -eq 0 ] && sed_inplace "s|(${var}=)\"[^\"]*\"|\1\"${hash}\"|" "$VERSIONS_FILE"
    pinned=$((pinned+1))
    sleep 1   # be a good citizen to the mirrors
done

echo
[ "$DRY" -eq 1 ] && DRY_LABEL=" (dry run — versions.sh untouched)"
ok "Pinned $pinned hashes${DRY_LABEL}${ONLY:+ (${skipped} skipped by --only)}"
if [ "${#failed[@]}" -gt 0 ]; then
    warn "Failed (${#failed[@]}) — the version or URL in versions.sh is wrong:"
    for f in "${failed[@]}"; do warn "  $f"; done
    exit 1
fi
