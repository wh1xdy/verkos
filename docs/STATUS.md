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
| **Live ISO** | `make iso` → a medium that boots: firmware → El Torito → vboot → kernel → initramfs → urverk mounts the ISO by label, loop-mounts the system image, overlays a tmpfs. Installer present, root writable and ephemeral |

## What exists but has never been booted

Nothing of substance. The ISO now boots (D-024), which settled the one
outstanding question: the firmware does present the embedded FAT image as an
ordinary volume, so vboot works unchanged.

Still untried: booting the ISO from real hardware or a USB stick. There is no
isohybrid MBR/GPT yet, so the ISO is not `dd`-able to a stick as a hybrid
image — it has only been booted as a virtio-blk device under QEMU/AAVMF.

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

1. **Trim the live system** — the ISO is 1.3 GB because the system carries gcc,
   binutils and cmake. A live medium needs none of them; this makes the ISO and
   every test cycle smaller.
2. **isohybrid** — a protective MBR and GPT so the ISO can be written to a USB
   stick and booted on real hardware.
3. **Sync x86_64** — the second architecture is the real test of whether any of
   this was portable.
4. **Give `full-userland` inner marks**, so a failure does not restart the
   compiler.
