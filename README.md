# VerkOS

[![CI](https://github.com/wh1xdy/verkos/actions/workflows/ci.yml/badge.svg)](https://github.com/wh1xdy/verkos/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**A Linux distribution built from source, which has been replacing its
third-party parts with its own — and, since 2026-07-26, has a kernel of its own
as well.**

VerkOS ("verk" is Swedish for *work / creation*) started as a Linux From
Scratch–style build: a cross-toolchain bootstraps temporary tools, which build
the final system inside a chroot. What it has become is a system where the
bootloader, the init, the installer, the account tools, the module tools, the
package manager and much of the everyday userland are code in this repository —
and where a second kernel, written here, boots the same machine to its own shell.

The rule the project is built on is that **a claim is earned by running the
thing**. The recurring defect across its whole history is a report that does not
depend on what it reports: a boot menu entry naming a flag nothing implemented, a
kernel printing "time running" with no timer, an installer that "succeeded" every
time while producing an unbootable disk. So this file distinguishes what has been
booted from what has only been written. The reasoning behind every choice is in
[`docs/DECISIONS.md`](docs/DECISIONS.md), D-001 to D-054.

---

## What is ours

Not a plan — these are directories in `pkg/`, built today; all but `stam`,
`viso` and `vbootimg` into the system itself.

| | Replaces | What it is |
|---|---|---|
| **`stam`** | Linux | Our own aarch64 kernel. Boots two ways, runs a shell (D-030…D-050) |
| **`vboot`** | GRUB | A UEFI bootloader in plain C, no gnu-efi. Verifies what it boots with Ed25519 — when a key is configured; the shipped key is all zeroes, so a default build boots unchecked (D-010, D-011) |
| **`urverk`** | systemd | One static binary that is both the initramfs `/init` and PID 1 (D-009) |
| **`vlogin`** | shadow | `login`, `su`, `passwd`, `useradd`… shadow is not built or even fetched (D-013, D-051) |
| **`verkmod`** | kmod | `insmod`, `rmmod`, `lsmod`, `modinfo`, `modprobe`, `depmod` (D-012) |
| **`vinstall`** | — | The installer, in five modes (D-015, D-018, D-019, D-020, D-022) |
| **`vmkfs`** | dosfstools | A FAT32 formatter. ext4 is bought from e2fsprogs, deliberately (D-014) |
| **`viso`** | xorriso | The ISO 9660 writer, with El Torito and a hybrid MBR (D-024, D-026) |
| **`vbootimg`** | mkbootimg | Android boot images, for Qualcomm boards (D-045) |
| **`vpk`** | — | The package manager: fetch, verify, build, install, remove |
| **`verkbox`** | coreutils | 42 applets in one multi-call binary |
| **`vdhcp`** | dhcpcd | The DHCP client |
| **26 `vX` tools** | GNU / iproute2 / … | `sed`, `awk`, `tar`, `gzip`, `ip`, `ping`, `less`, `find`, `ps`, `curl`… |

**At least 97 command names on a booted VerkOS resolve to code in this
repository** — 42 verkbox applet names, plus 55 familiar names symlinked onto 26 `vX` tools
(`scripts/40-final-system.sh`). The `vX` binary is the real file and the everyday
name is the symlink, so `ip a` and `tar xf` work as muscle memory expects.

**What we deliberately do not own.** glibc, because desktop software expects it
(D-002). ext4, because a subtle mistake in a filesystem is silent until someone's
data is gone (D-014). LUKS, for the same reason only sharper (D-020). Password
hashing, which stays libxcrypt's — the one place a mistake is silent, permanent
and found by an attacker (D-013). And every *format*: ELF, cpio, tar, ISO 9660,
the device tree. The principle, stated twice in the log: **adopt the interface
everyone speaks; own the implementation behind it** (D-030, D-040).

---

## What actually boots

Everything in this section was checked by running a system on **aarch64**, not by
reading code. The full list, with what each proves, is in
[`docs/STATUS.md`](docs/STATUS.md).

- **The boot chain.** UEFI → vboot → Linux → initramfs → urverk → services →
  login. No GRUB, no systemd, no `-kernel`.
- **Installing.** `vinstall` onto a bare disk, and the result booting on its own
  with nothing else attached. Root and `/boot` resolved by UUID.
- **Encrypted root.** `--encrypt` produces a LUKS2 container that refuses to
  mount; urverk finds it by UUID before there is a root, prompts, opens it.
- **Beside another system.** `--root-part`/`--esp-part` install into existing
  partitions; the other system's bootloader keeps its checksum, and the firmware
  loads ours from a `Boot####` variable we wrote into efivarfs — confirmed in the
  firmware's own BdsDxe log.
- **Systems that forget.** `--immutable` and `--ramdisk`: a file written on the
  first boot is gone on the second.
- **The live ISO.** `make iso` produces a medium that boots, installs from
  itself, and boots from a simulated USB stick through its hybrid MBR — the
  firmware logs "USB HARDDRIVE", so it was read as a disk and not as a CD. All
  four menu entries were selected and watched, including `rd.toram` and the
  rescue shell.
- **A full userland.** A native GCC built and installed; `libstdc++` has working
  threads. VerkOS builds software from source, on itself, with its own tools.

**What has not been tried: real hardware.** The ISO boots as a simulated USB
stick under QEMU/AAVMF, which exercises the same firmware path, but no physical
machine has been asked to boot it. There is deliberately no GPT on the medium
(D-026), so firmware that reads nothing else will not find it.

**x86_64 is many months behind.** The toolchain and kernel build, and vboot boots
under OVMF, but the current tree — urverk, vlogin, verkmod, vinstall, viso,
e2fsprogs, cryptsetup, the native GCC — has never been built there. `make run
ARCH=x86_64` does not work today: the launcher wants an initramfs filename no
stage produces. **i686** is configured but has no path to a bootable image at
all; stage 70 refuses the architecture.

---

## `stam` — our own kernel

The original plan was a distro first and a kernel later (D-001). The kernel work
began on 2026-07-26. It is about 3 300 lines of C, headers and AArch64 assembly,
against the 36.3 million lines of the Linux kernel it sits beside.

It boots **two ways from one set of sources**: as a raw arm64 `Image` that QEMU's
`-kernel` or any ARM bootloader can start, and as a UEFI application that
firmware — or vboot — loads. Nothing above `bootinfo.h` knows which happened
(D-030, D-034).

What it does today, each verified by running it:

- Finds its own hardware from the **device tree** — console, memory, CPUs, the
  timer's interrupt number. Nothing is hardcoded. On machines whose firmware
  publishes no tree, **vboot builds one** out of ACPI's SPCR, MADT and GTDT and
  installs it under the GUID firmware would have used, so the kernel cannot tell
  the difference (D-035, D-036).
- Exception vectors installed **before** the MMU, so the first thing virtual
  memory can do wrong is reported rather than silent (D-032).
- GICv2 **and** GICv3, which takes the interrupt controller out of the way of
  real hardware (D-044) — stam itself has still only run under QEMU. Verified
  across five configurations: raw Image and UEFI, ACPI and device tree, v2 and v3.
- Preemptive round-robin scheduling driven from the timer interrupt, with the
  kernel in the high half (TTBR1) and every process holding its own low half
  (D-037, D-042, D-043).
- Seven system calls — `read`, `write`, `exit`, `getpid`, `clone`, `execve`,
  `wait4` — using Linux's aarch64 numbers, because the numbering is the cheapest
  part of an ABI to agree with (D-039).
- An **ELF loader** fed from a **cpio initrd**, both formats everyone else
  speaks, both readers ours (D-040, D-047).
- `fork`, `exec`, `wait4`, zombie reaping and pages that come back — a process
  that exits returns every page of its address space (D-046, D-048).
- **A shell**, `vsh`: prompt, read a line, split it, fork, exec with arguments,
  wait. That is the evidence, not the feature — being able to write a shell as
  an ordinary program with no privileges and nothing the kernel knows about it
  is what shows the interface underneath is real (D-049, D-054).
- An interrupt-driven console with a line discipline, and a machine that actually
  idles: sitting at the prompt for ten seconds, QEMU consumed no measurable CPU
  (D-050).

Named rather than hidden: no SMP and **no locking** — system calls run with
interrupts enabled, which is a debt (D-050). No copy-on-write; `fork` copies
outright. No pipes, no redirection, no filesystem beyond the initrd. No
user-pointer validation. No FP/SIMD for EL0. **A burst of input longer than a
few characters is lost** — typed slowly the shell reads a whole line, pasted it
does not (D-054). A **Qualcomm GENI console driver** and `vbootimg` exist for
hardware nothing has run them on, and D-045 says so plainly rather than letting
them sit beside the tested work as equals.

And one that is about the tests rather than the kernel: **nothing here boots
stam.** `make test` builds it and checks its shape — Image magic, vector
ordering, relocation count, no unaligned access before the MMU — but never
starts it. That is how the kernel came to stop booting entirely, for a reason
no source change caused, with every check still green (D-053).

**stam is not in any image.** It is built by `pkg/stam/Makefile` and shape-checked
by `make test`; no numbered build stage ships it. The distribution still boots
Linux 6.12.9.

---

## Quick start

> A full build compiles for **hours** and needs **tens of GB** of free disk. It
> needs a real Linux machine with a case-sensitive filesystem. The walkthrough,
> including the arm64 Linux container used on Apple Silicon, is in
> [`docs/BUILD.md`](docs/BUILD.md).

```sh
make check ARCH=aarch64     # does this host have what it takes?
make fetch ARCH=aarch64     # download every tarball, verify its pinned SHA-256
make       ARCH=aarch64     # stages 00-70: toolchain, system, kernel, image, boot disk
make run   ARCH=aarch64     # UEFI firmware -> vboot -> VerkOS

make iso     ARCH=aarch64   # stage 71: the live medium (needs `make` first)
make run-iso ARCH=aarch64   # boot it
make test                   # the host-side suite; this is what CI runs
```

`ARCH` defaults to `x86_64`, but **aarch64 is the architecture that works today**
— see above. Every stage also runs on its own (`make toolchain`, `make system`,
`make kernel`, `make image`, `make boot`, …).

---

## The build, stage by stage

Numbered scripts in [`scripts/`](scripts/), orchestrated by
[`scripts/build.sh`](scripts/build.sh) and wrapped by the `Makefile`. They read
like the LFS book on purpose: you can open one and see which package is
configured with which flags, in order.

| Stage | `make` | What it does |
|------:|--------|--------------|
| 00 | `check` | Host prerequisites — compiler, build tools, QEMU, free disk |
| 10 | `fetch` | Every upstream tarball, verified against its pinned SHA-256 — except the CA bundle and the pip sdists, which are unpinned |
| 20 | `toolchain` | The cross toolchain: binutils, gcc pass 1, kernel headers, glibc, libstdc++ |
| 30 | `temp` | Cross-build the temporary tools into the target rootfs |
| 40 | `system` | Enter the chroot and build the real system: the GNU userland, a native GCC, **and every VerkOS tool that ships in the rootfs**. glibc is not rebuilt — stage 20's is used |
| 50 | `kernel` | Linux 6.12.9 for the target |
| 60 | `image` | Trim the rootfs, build the ext4 root image |
| 70 | `boot` | vboot, the initramfs, and a GPT disk with an ESP that boots |
| 71 | `iso` | The live medium — written by our own `viso` |

`make` runs 00 through 70. Stage 71 is separate, because the ISO is built from
what stage 70 produced.

Stage 60 refuses to package a rootfs it does not recognise, and both its guards
exist because something got through: nothing may be mounted inside the rootfs, and
the top level must have plain-text names. A leaked bind mount once turned a
cleanup into deleting every source the project had downloaded; vtar, handed a
compressed file it could not read, once created 36 files named after binary
garbage and reported success (D-016).

Versions are pinned in one place, [`config/versions.sh`](config/versions.sh):
GCC 14.2.0, glibc 2.40, binutils 2.43.1, Linux 6.12.9, bash 5.2.37,
OpenSSL 3.4.0, OpenSSH 9.9p1, e2fsprogs 1.47.1, cryptsetup 2.7.5. Per-architecture
settings live in [`config/targets/`](config/targets/).

---

## `vpk`, `verkbox`, and the `vX` tools

**`vpk`** builds packages from recipes and caches the result as a binary `.vpk`.
One C binary: it links libcurl, liblzma and zlib and implements SHA-256, tar
extraction, recipe parsing, dependency resolution and the install database
itself. Extraction is tar-slip hardened — absolute paths and `..` components are
refused, because it unpacks as root.

```sh
vpk update | search | install | reinstall | upgrade | build | remove | list | info
```

A recipe is a few fields and a shell `build()` that installs into `$DESTDIR`;
[`pkg/recipes/`](pkg/recipes/) holds fourteen. The **whole base system is
registered in vpk's database**, so `vpk list` shows the OS, and `[base]` packages
are protected — a protection that was once strippable by `vpk reinstall`, which
on a running system meant `vpk remove zlib` became permitted (D-029).

**`verkbox`** is one multi-call binary with **42 applets**:

```
true false echo cat pwd whoami nproc yes basename dirname head wc seq ls tail
cut rev uniq mkdir rmdir rm cp mv ln touch sort tr nl tac tee fold comm paste
grep chmod du printf uname realpath env sleep which
```

The whole set takes over `/usr/bin` on a built system — GNU coreutils stays
installed, but the names point at verkbox.

**The `vX` tools** are 26 separate programs covering the rest: `vsed`, `vawk`,
`vtar`, `vgzip`, `vbzip2`, `vzip`, `vcpio`, `vip`, `vping`, `vtrace`, `vss`,
`vdig`, `vnetcat`, `vget`, `vmon`, `vsys`, `vfind`, `vxargs`, `vpager`, `ved`,
`vdate`, `vstat`, `vhex`, `vtree`, `vwhois`, and `verkmod`.

They were made honest by real software, not by testing them in isolation.
Building cryptsetup's dependency chain put them in front of autotools, gnulib,
libtool and cmake for the first time, and **defects fell out of four tools at
once** — vtar truncating paths over 100 characters and writing files to the *wrong* place,
vawk treating a regex field separator as a literal, vsed silently discarding
commands, verkbox's `sort` missing `-c`/`-t`/`-k`. A deliberate sweep for silent
failures the same day found eighteen *other* problems and none of these (D-017,
D-021).

> Synthetic probes test what we thought to ask. Real build systems ask things we
> did not think of, in combinations like `sort -nt_ -k5` and `tar xpf -`.

---

## Installing, and the live medium

`vinstall` destroys data on purpose — a whole disk at a time — so it refuses
rather than guesses. It rejects a target that is not a block device, one with
anything mounted on it, and the disk the running system booted from; it checks
`sfdisk`, `blkid`, `mkfs.ext4` and a FAT formatter **before** touching anything
(`cryptsetup` for `--encrypt` is still checked only after repartitioning, and
`mkswap` not at all); `-n` leaves the disk
byte-identical. **Confirmation is the disk's name, not "y"** — the question is
not "are you sure", which nobody misreads carefully, but "is this the disk you
meant". Root is referred to by UUID everywhere, because the name a disk gets
depends on what else is plugged in.

Five modes, each booted to prove it:

```sh
vinstall -t /dev/sda                         # GPT, FAT32 ESP, ext4 root
vinstall -t /dev/sda --encrypt               # root inside a LUKS2 container
vinstall --root-part /dev/sda3 --esp-part /dev/sda1   # beside another system
vinstall -t /dev/sda --immutable             # read-only root under a tmpfs overlay
vinstall -t /dev/sda --ramdisk               # no root partition; one image on the ESP
```

An install never inherits the image's password: `vinstall` prompts, or takes
`--passwd-file`, or `--lock-root` with a named account — and refuses to run
unattended without being told (D-018). Without that, every installed machine kept
the development password that is written down in this repository.

`make iso` writes the live medium with our own `viso`: an ISO 9660 image carrying
a FAT EFI image and the ext4 system image, an El Torito no-emulation entry, and
a 512-byte MBR so a USB stick's firmware finds a FAT partition to boot
(D-024, D-026). The installer runs from the medium and produces a disk that boots
on its own.

---

## Tests and CI

`make test` is [`scripts/test.sh`](scripts/test.sh) — everything about VerkOS
that can be checked on a build host without building a distro or booting a VM. It
is regression cover for things that have actually broken, not a coverage
exercise: the Ed25519 that decides whether a kernel may boot, against the RFC
8032 vectors; `vtar`, `vgzip`, `vbzip2` and `vcpio` round-tripped against their
GNU counterparts; twenty-five of the C programs still compiling, including the
bootloader for both architectures.

Three checks are there because of a specific defect and are worth naming:

- **`stam.efi` must need zero relocations.** The kernel is position-independent
  and nothing applies relocations to it, so anything routed through the GOT holds
  the address the image had when linked. That bug has been found twice — a thread
  starting at `0x193c`, and a user program copied out of empty space — and this
  check is what catches the third (D-037, D-039).
- **Every `rd.*`/`urverk.*` token any boot configuration hands to urverk must be
  a token urverk reads.** The rescue menu entry named a flag nothing implemented,
  for as long as the menu had existed, and selecting it was indistinguishable
  from selecting the first entry (D-028).
- **Exactly two setuid binaries, `su` and `passwd`, each with one `main()`.**
  Setuid and `argv[0]` dispatch are incompatible: put them together and the
  privileged code is selected by a field the caller sets freely. The build strips
  the bit from everything else and fails if any survives (D-051).

CI runs `make test`, compiles vpk and verkbox, diffs verkbox against GNU for
`seq`, `cat`, `head`, `sort` and `wc`, and lints every shell script outside
`tools/` with `bash -n` in a separate job. A green CI means all of that held.

One lesson has its own entry. The suite once gave three different pass counts on
three consecutive runs, because a check compared modification times and the files
landed in the same second often enough to flip. **A check that is only sometimes
right is worse than no check** — a test that always fails gets fixed; one that
fails a third of the time teaches everyone to re-run it, after which a real
regression looks exactly like the noise (D-038).

---

## Repository layout

```
verkos/
├── config/       versions.sh (pins + hashes), os-release, per-arch targets
├── scripts/      the build: stages 00-71, plus test.sh and lib/
├── pkg/
│   ├── stam/     our own kernel — aarch64, boots two ways, runs a shell
│   ├── vboot/    the UEFI bootloader, and the Ed25519 that can gate a boot
│   ├── urverk/   init: the initramfs stage and PID 1, one binary
│   ├── vlogin/   accounts: a library, plus vlogin, su and passwd
│   ├── verkmod/  module tools
│   ├── vinstall/ the installer
│   ├── vmkfs/    the FAT32 formatter
│   ├── viso/     the ISO 9660 writer
│   ├── vbootimg/ Android boot images
│   ├── vpk/      the package manager
│   ├── verkbox/  42 coreutils applets in one binary
│   ├── vdhcp/    the DHCP client — plus vgetty and vfb, the getty and the splash
│   ├── v*/       25 more tools that, with verkmod, take over 55 familiar names
│   └── recipes/  vpk build recipes
├── boot/         QEMU launchers per arch
├── tools/        verkfetch, and dev-access.sh
└── docs/         STATUS and DECISIONS first; also ARCHITECTURE, BUILD, ROADMAP
```

Start with [`docs/STATUS.md`](docs/STATUS.md) — it says what is proved by running
it, what is written but never run, and what is known fragile.
[`docs/DECISIONS.md`](docs/DECISIONS.md) is the long answer to every "why", and
is worth searching rather than reading.

---

## Contributing

Bug fixes found by real builds are the most valuable thing here, and the bar for
a claim is that you ran it.

- **A package:** `pkg/recipes/<name>/recipe` — `name`, `version`, `source`,
  `sha256`, `depends`, `build()`. If it is part of the base build, add the
  version to `config/versions.sh` and the tarball to `PKGS` in
  `scripts/pin-hashes.sh`, after which `make pin` fills in the hash.
- **A verkbox applet or a `vX` tool:** plain C, no dependencies beyond libc —
  vget's TLS through OpenSSL is the one exception — and it must match its GNU
  counterpart where anything depends on the difference.
- **CI must stay green.** `make test` runs the same script CI does; run it before
  you push.

The question this project asks of every output and every return value is: **what
does this print when the thing it describes did not happen?** A tool that fails
without saying so does not merely fail to work — it leaves damage somewhere
nobody is looking.

---

## License

VerkOS' own code is **MIT** — see [`LICENSE`](LICENSE). VerkOS packages upstream
free software; each bundled component keeps its own license.
