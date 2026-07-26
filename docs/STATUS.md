# Where VerkOS stands

Written 2026-07-26, as a handover. The point of this file is to let a new
session start without reading the whole history: what is proved, what is
merely written, and what is known to be wrong.

`make test` — 118 checks, all passing.

---

## What is verified by booting

Nothing below is inference. Each was checked by running a system, not by
reading code.

| | |
|---|---|
| **Boot chain** | UEFI → vboot (ours) → kernel → initramfs → urverk → services → login, on aarch64 |
| **Encrypted root** | `vinstall --encrypt` makes a LUKS2 container; the partition reports `crypto_LUKS`/`aes-xts-plain64` and refuses to mount; urverk finds it by UUID, prompts, opens it, boots with root on `/dev/mapper/cryptroot` |
| **Dual boot** | `--root-part`/`--esp-part` install beside another system; its bootloader keeps its checksum, its config survives, and the firmware loads ours from a `Boot####` variable we wrote into efivarfs (confirmed in the firmware's own BdsDxe log) |
| **Immutable install** | `--immutable`: root read-only under a tmpfs overlay. A file written on the first boot is gone on the second |
| **Diskless install** | `--ramdisk`: no root partition; the system is one ext4 image on the ESP, loop-mounted with an overlay. Boots, 496 programs, three services; nothing persists |
| **Passwords** | After install, the image's build password is rejected and the one given at install time is accepted |
| **Accounts** | vlogin replaces shadow entirely: interactive login, wrong password, unknown user, locked account, autologin, `su`, `passwd`, `useradd`/`usermod`/`userdel`. `su`/`passwd` are setuid and drop privilege completely |
| **Modules** | verkmod replaces kmod: `modprobe btrfs` pulls in five dependencies, the kernel reports the refcounts, `rmmod` refuses one in use |
| **Full userland** | Native GCC built and installed; `libstdc++` has working threads (self-checked during the build) |

## What exists but has never been booted

- **The live ISO.** `pkg/viso` writes an ISO that xorriso accepts, the kernel
  mounts, and whose files read back byte-identical. The kernel has
  `ISO9660_FS=y` and `OVERLAY_FS=y`. urverk has the three options the design
  needs. **But `scripts/71-iso.sh` does not exist**, no ISO has been assembled
  from real artefacts, and nothing has booted from one.
- The riskiest unknown is what the firmware hands vboot when it boots from an
  El Torito image: the plan assumes it presents the embedded FAT volume, and
  that is inferred from standard EDK2 behaviour, not observed. vboot was
  hardened against the two ways that assumption could bite (see D-023), so a
  failure now should be attributable to the ISO itself.

## Known-wrong or fragile

- **x86_64 has none of this.** The Proxmox VM is many months of work behind:
  no verkmod, vlogin, vmkfs, vinstall, viso, e2fsprogs, cryptsetup, no native
  GCC, and none of the sixteen tool fixes. A clean build there would also prove
  nothing done here is aarch64-specific.
- **The rootfs is 958 MB** now that the full userland is built. A live ISO would
  be ~1.3 GB. A live medium does not need gcc, binutils or cmake.
- **`full-userland` is one block with no inner marks**, so any failure inside it
  rebuilds bzip2, file, m4, binutils and gcc from the start. This cost several
  hours during the LUKS work.
- **An interrupted `make install` of GCC leaves the system unable to rebuild
  itself** — the new driver is installed but its internal headers are not, so
  `gcc` cannot find `stddef.h` and `g++` segfaults. Recovery depended on the
  cross toolchain's `aarch64-verk-linux-gnu-*` drivers still existing, which is
  luck, not design.
- **Static C++ linking does not work** until a native GCC has been built: the
  cross toolchain's `libstdc++.a` is a stub with neither sized delete nor
  `basic_string`.
- The build volume is 79 GB. Unpacked sources are now reclaimed on success, but
  a single failed GCC build can still occupy 46 GB.

## The thing most worth remembering

Building cryptsetup's dependency chain put VerkOS' own userland in front of real
third-party build systems for the first time at scale. **Eight defects fell out**
(D-021), and nearly all produced *a wrong answer with a zero exit status*:
`vtar` truncating paths over 100 characters and writing files to the wrong
place, `vawk` treating a regex field separator as a literal, `vsed` silently
discarding commands, `verkbox`'s `sort` missing `-c`/`-t`/`-k`.

A deliberate sweep for silent failures earlier the same day — every tool through
unknown flags, wrong formats, missing files — found eighteen *other* problems
and none of these.

> Synthetic probes test what we thought to ask. Real build systems ask things we
> did not think of, in combinations like `sort -nt_ -k5`, `tar xpf -` and
> `sed -n -e 'w $@-t'`.

Every one was found by a build failing somewhere far from the cause. That is the
argument for building real software with our tools rather than testing them in
isolation.

## Suggested next steps, in order

1. **Finish the ISO** — write `scripts/71-iso.sh`, assemble, boot it. Everything
   it needs is in place.
2. **Trim the live system** — a live image does not need a compiler; this makes
   the ISO and every test cycle smaller.
3. **Sync x86_64** — the second architecture is the real test of whether any of
   this was portable.
4. **Give `full-userland` inner marks**, so a failure does not restart the
   compiler.
