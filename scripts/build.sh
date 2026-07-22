#!/usr/bin/env bash
# VerkOS build orchestrator.
# Runs the numbered stages in order for a given ARCH, or a single named stage.
#
# Usage:
#   scripts/build.sh [ARCH]                 # full pipeline (default x86_64)
#   scripts/build.sh <stage> [ARCH]         # one stage: check|fetch|toolchain|
#                                           #   temp|system|kernel|image
#   ARCH=aarch64 scripts/build.sh           # arch via env also works
source "$(dirname "$0")/lib/common.sh"

STAGE="all"
case "${1:-}" in
    check|fetch|toolchain|temp|system|kernel|image|all)
        STAGE="$1"; shift ;;
    x86_64|i686|aarch64)
        : ;;                      # first arg is an arch, keep STAGE=all
    "" ) : ;;
    * )  die "unknown stage/arch '$1' (stages: check fetch toolchain temp system kernel image all)" ;;
esac
# Allow an ARCH as the remaining positional arg.
[ -n "${1:-}" ] && export ARCH="$1"

S="$VERK_ROOT/scripts"
run() { step "$1"; bash "$S/$2"; }

case "$STAGE" in
    check)     run "Host check"        00-check-host.sh ;;
    fetch)     run "Fetch sources"     10-fetch-sources.sh ;;
    toolchain) run "Cross-toolchain"   20-cross-toolchain.sh ;;
    temp)      run "Temporary tools"   30-temp-tools.sh ;;
    system)    run "Final system"      40-final-system.sh ;;
    kernel)    run "Kernel"            50-kernel.sh ;;
    image)     run "Image"             60-image.sh ;;
    all)
        run "Host check"      00-check-host.sh
        run "Fetch sources"   10-fetch-sources.sh
        run "Cross-toolchain" 20-cross-toolchain.sh
        run "Temporary tools" 30-temp-tools.sh
        run "Final system"    40-final-system.sh
        run "Kernel"          50-kernel.sh
        run "Image"           60-image.sh
        echo; ok "Full pipeline finished for ARCH=$ARCH"
        ;;
esac
