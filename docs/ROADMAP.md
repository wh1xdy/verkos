# VerkOS roadmap

Where we are and where we're going. Checkboxes track real progress.

## Phase 0 — Foundations  ✅ (scaffolding done)

- [x] Project structure, docs, and decision log
- [x] Version-pinning config (`config/versions.sh`)
- [x] Per-architecture target config (x86_64, i686, aarch64)
- [x] Branding (`config/os-release`)
- [x] Build orchestration skeleton (`Makefile`, `scripts/build.sh`, `common.sh`)
- [x] QEMU boot launchers + GRUB template
- [x] Checksum-pinning tool (`scripts/pin-hashes.sh`), Linux/macOS-portable
- [x] All 35 source tarballs pinned with verified SHA-256 (meson is pip-only)

## Phase 1 — First boot (x86_64)

The milestone: `make run ARCH=x86_64` drops us at a VerkOS shell.

- [x] Stage 0: source fetch + SHA-256 verify — *script done* (needs real hashes pinned)
- [x] Stage 1: cross-toolchain (binutils, gcc, glibc, libstdc++) — *script done*
- [x] Stage 2: temporary tools cross-built into the rootfs — *full LFS ch.6 sequence written*
- [x] Stage 3: chroot entry (native + qemu-user foreign) — *script done*
- [x] Stage 4: final system driver — glibc→util-linux→dbus→**systemd** critical path *written*
- [x] Stage 4b: full core userland (bzip2, file, m4, final binutils+gcc, bison,
      flex, sed, grep, gawk, diffutils, findutils, tar, gzip, make, patch) +
      system config (fstab, resolv.conf, machine-id, default target) *written*
- [x] Stage 5: Linux kernel build — *script done*
- [x] Stage 6: image — trim the rootfs and build the ext4
- [x] Stage 7: boot image — vboot, the initramfs, and a GPT+ESP disk
- [x] ▶ **Actually compile it** and fix what breaks
- [x] 🎯 **Boots to a login prompt in QEMU** (aarch64)

> Legend: *script done* = the build logic is written and syntax-clean, but the
> heavy compile hasn't been run/validated on real hardware yet. The remaining
> unchecked boxes are about *running* the build and fixing real-world breakage.

## Phase 2 — Multi-arch

- [x] aarch64 → boots in QEMU
- [x] x86_64 → builds; `vboot` boots on it under OVMF
- [ ] x86_64 rebuilt against the current tree (urverk, no systemd, stage 70)
- [ ] i686 → boots in QEMU
- [ ] Shared CI that builds all three

## Phase 3 — A real system

- [x] Networking — our own `vdhcp` client, DNS, and the vX network tools
- [x] SSH, package fetching tools
- [x] A package format and build recipes — `vpk`, with binary `.vpk` packages
- [x] A bootloader and disk images — `vboot` + a GPT/ESP boot disk, UEFI
- [ ] Boot it on real hardware (a physical PC, a Raspberry Pi)
- [ ] A live ISO that can be written to a USB stick
- [ ] An installer (`vinstall`) — needs e2fsprogs, which is not built yet

## Phase 4 — Desktop

- [ ] Wayland + a compositor
- [ ] Graphics stack (Mesa, libinput, fonts)
- [ ] A minimal desktop session
- [ ] Audio (PipeWire)

## Phase 5 — Make it ours

The long game — replace stack components with our own:

- [x] Custom initramfs init / early userspace — `urverk` stage 1
- [x] **Our own init**: `urverk` is PID 1; systemd is gone entirely
- [x] **Our own bootloader**: `vboot`, UEFI, aarch64 + x86_64
- [x] **Signed boot**: our own Ed25519, verified against the RFC 8032 vectors
- [x] Our own coreutils-equivalents — `verkbox` plus ~35 vX tools, which took
      over `sed`, `awk`, `gzip`, `tar`, `ip`, `ping`, `less` and more
- [ ] Our own shell (`vsh`) — bash is the last large interactive component
- [ ] Our own `mkfs.ext4` and partitioner (the installer needs both anyway)
- [ ] `vpk verify` — re-hash installed files, and sign packages with the same
      Ed25519 the boot chain uses
- [ ] Eventually: **a custom kernel** (the original dream), starting as a bootable
      toy kernel on x86_64 and growing from there

## Parked ideas

Things we want to do but not yet:

- [ ] **Integrate Alex's own software** into VerkOS in some fun way — e.g.
      preinstall his apps in the image, ship them as systemd services, or write
      package recipes for them. (Details TBD — Alex to describe the software.)

---

*Milestones are deliberately front-loaded on "get something booting" — everything
after Phase 1 is more fun when there's a running system to test against.*
