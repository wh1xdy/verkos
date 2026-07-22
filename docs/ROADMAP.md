# VerkOS roadmap

Where we are and where we're going. Checkboxes track real progress.

## Phase 0 — Foundations  ✅ (scaffolding done)

- [x] Project structure, docs, and decision log
- [x] Version-pinning config (`config/versions.sh`)
- [x] Per-architecture target config (x86_64, i686, aarch64)
- [x] Branding (`config/os-release`)
- [x] Build orchestration skeleton (`Makefile`, `scripts/build.sh`, `common.sh`)
- [x] QEMU boot launchers + GRUB template

## Phase 1 — First boot (x86_64)

The milestone: `make run ARCH=x86_64` drops us at a VerkOS shell.

- [ ] Stage 0: source fetch + SHA-256 verification working end to end
- [ ] Stage 1: cross-toolchain (binutils, gcc, glibc) builds clean
- [ ] Stage 2: temporary tools cross-built into the rootfs
- [ ] Stage 3: chroot entry (native)
- [ ] Stage 4: final system — glibc, coreutils, bash, util-linux
- [ ] Stage 4b: **systemd** as PID 1
- [ ] Stage 5: Linux kernel builds and boots
- [ ] Stage 6: initramfs assembly
- [ ] 🎯 **Boots to a login prompt in QEMU**

## Phase 2 — Multi-arch

- [ ] aarch64 via qemu-user native builds → boots in QEMU
- [ ] i686 → boots in QEMU
- [ ] Shared CI that builds all three

## Phase 3 — A real system

- [ ] Networking (systemd-networkd, DNS, DHCP)
- [ ] SSH, package fetching tools
- [ ] A minimal package format / build recipes (Beyond-LFS style)
- [ ] GRUB + disk images → boots on real hardware (a physical PC, a Raspberry Pi)

## Phase 4 — Desktop

- [ ] Wayland + a compositor
- [ ] Graphics stack (Mesa, libinput, fonts)
- [ ] A minimal desktop session
- [ ] Audio (PipeWire)

## Phase 5 — Make it ours

The long game — replace stack components with our own:

- [ ] Custom initramfs init / early userspace
- [ ] Our own coreutils-equivalents where it's instructive
- [ ] Eventually: **a custom kernel** (the original dream), starting as a bootable
      toy kernel on x86_64 and growing from there

---

*Milestones are deliberately front-loaded on "get something booting" — everything
after Phase 1 is more fun when there's a running system to test against.*
