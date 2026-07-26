# Where VerkOS stands

Written 2026-07-26, as a handover. The point of this file is to let a new
session start without reading the whole history: what is proved, what is
merely written, and what is known to be wrong.

`make test` — 165 checks, all passing, run in the `verkbuild` container on
2026-07-26. The number is host-dependent: some checks are skipped without a
network or a cross toolchain, so a Mac host reports fewer and fails several on
missing headers. Run it where the build runs.

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
| **Live ISO** | `make iso` → a 731 MB medium that boots: firmware → vboot → kernel → initramfs → urverk mounts the ISO by label, loop-mounts the system image, overlays a tmpfs. Installer present, root writable and ephemeral |
| **All four live entries** | Each ISO menu entry was selected and watched: normal; `rd.toram` (copies 648 MB in, then mounts from `/ram/root.img`); verbose; and the rescue shell, which now exists (D-028) and returns to the boot when left |
| **Install from the live medium** | Booting the ISO, running `vinstall` on it, then detaching the ISO: the installed disk boots on its own with a persistent root, `/boot` mounted, three services and 489 programs |
| **Live ephemerality** | Files written on one live boot — `/root`, `/etc` — are gone on the next |
| **Bootable from USB** | The same ISO attached as a `usb-storage` device boots: the firmware logs "USB HARDDRIVE" and the guest sees `sda1` (vfat), so it was read through the hybrid MBR, not El Torito |

## What exists but has never been booted

Nothing of substance. The ISO now boots (D-024), which settled the one
outstanding question: the firmware does present the embedded FAT image as an
ordinary volume, so vboot works unchanged.

Still untried: **real hardware**. The ISO boots as a simulated USB stick under
QEMU/AAVMF, which exercises the same firmware path, but no physical machine has
been asked to boot it. There is deliberately no GPT (D-026), so firmware that
reads nothing but GPT will not find it.

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
third-party build systems for the first time at scale. **Twelve defects fell out**
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

1. **Boot it on real hardware** — everything else is simulation. Write the ISO
   to a stick and try it.
2. **Sync x86_64** — the second architecture is the real test of whether any of
   this was portable, viso and El Torito included.
3. **Trim further** — about 250 MB of the 731 MB is the compiler. Removing it
   would roughly halve the medium again, at the cost of an ISO install no
   longer providing gcc.
4. **Give `full-userland` inner marks**, so a failure does not restart the
   compiler.
