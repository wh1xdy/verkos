#!/usr/bin/env bash
# Stage 00 — verify the host has what it takes to build VerkOS.
# Prints what's missing; exits non-zero if anything essential is absent.
source "$(dirname "$0")/lib/common.sh"

step "Checking host for ARCH=$ARCH (host is $HOST_ARCH)"

missing=0
need() {
    # need <command> [package-hint]
    if command -v "$1" >/dev/null 2>&1; then
        ok "$1"
    else
        warn "MISSING: $1${2:+  (try installing: $2)}"
        missing=1
    fi
}

log "Core toolchain:"
need gcc;   need g++;   need make;  need ld;   need ar
need bash;  need patch; need tar;   need xz;   need gzip
log "Build helpers:"
need bison; need flex;  need gawk;  need m4;   need perl
need python3; need makeinfo "texinfo"; need pkg-config
log "Fetch + hashing:"
{ command -v curl >/dev/null || command -v wget >/dev/null; } \
    && ok "curl/wget" || { warn "MISSING: curl or wget"; missing=1; }
need sha256sum
log "Emulation / boot:"
need "$QEMU_BIN" "qemu-system"
if ! is_native; then
    need "${QEMU_USER}-static" "qemu-user-static"
fi

# Disk space check (need lots).
avail_gb="$(df -Pk "$VERK_ROOT" | awk 'NR==2{print int($4/1024/1024)}')"
if [ "${avail_gb:-0}" -lt 20 ]; then
    warn "Only ${avail_gb}GB free under $VERK_ROOT — a full build wants ~20GB+ per arch"
else
    ok "${avail_gb}GB free disk"
fi

echo
if [ "$missing" -eq 0 ]; then
    ok "Host looks ready to build VerkOS/$ARCH"
else
    die "Host is missing required tools (see MISSING lines above)"
fi
