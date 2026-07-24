# VerkOS

[![CI](https://github.com/wh1xdy/verkos/actions/workflows/ci.yml/badge.svg)](https://github.com/wh1xdy/verkos/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

**A from-scratch, multi-architecture Linux distribution — built entirely from source, package by package, with its own package manager and its own coreutils.**

VerkOS ("verk" is Swedish for *work / creation*) is a Linux From Scratch–style
distro assembled the hard way on purpose: a cross-toolchain bootstraps temporary
tools, which build a full **glibc + systemd** system inside a chroot. The result
is a real, self-hosting OS that boots to a login prompt, goes online, and builds
software from source on itself — one we understand top to bottom because we built
every layer.

---

## Why VerkOS is different

- **Its own package manager — `vpk`.** A self-contained, source-based
  (ports/Gentoo-style) package manager written in C. One binary: it links
  libcurl, liblzma and zlib and implements SHA-256, tar extraction, recipe
  parsing, recursive dependency resolution and the install database *itself* — no
  shelling out to `curl`/`tar`/`sha256sum`. Recipes are fetched from this repo on
  demand, every tarball is SHA-256-verified, and extraction is tar-slip hardened
  (it refuses absolute paths and `..` components because it unpacks as root).

- **Its own coreutils — `verkbox`.** A BusyBox-style multi-call binary with **33
  applets**, all plain C with no dependencies. On a running VerkOS, `ls`, `cat`,
  `head`, `tail` and `wc` are already served by verkbox — each one promoted to
  shadow GNU only after it is flag-complete *and* verified byte-exact against GNU
  coreutils by differential tests.

- **One coherent system, laptop to server.** A single glibc + systemd base with a
  real PID 1, working login sessions, DHCP networking (dhcpcd) and SSH (OpenSSH).
  The same userland scales from an interactive desktop-bound machine to a
  headless box you SSH into.

- **Genuinely multi-arch.** The build is a proper cross-toolchain bootstrap for
  **x86_64** (primary), **aarch64** (first boot achieved) and **i686**. x86_64 is
  built at native speed on an x86 build host; aarch64 is built on Apple Silicon
  inside a native arm64 Linux VM (Colima).

---

## Quick start

> A full glibc + systemd build compiles for **hours** and needs **tens of GB** of
> free disk. Run it on a real Linux machine with a **case-sensitive** filesystem,
> not a throwaway container. The full walkthrough is in [`docs/BUILD.md`](docs/BUILD.md).

```sh
make check   ARCH=x86_64     # verify the host has the required tools
make fetch   ARCH=x86_64     # download + SHA-256-verify all upstream sources
make         ARCH=x86_64     # full pipeline: toolchain -> temp -> system -> kernel -> image
make run     ARCH=x86_64     # boot the result in QEMU
```

`ARCH` defaults to `x86_64`; the other targets are `aarch64` and `i686`. Every
stage can also be run on its own (`make toolchain`, `make system`, `make kernel`, …).

Building on macOS? You can't build VerkOS *on* macOS directly — see
[`docs/BUILD.md`](docs/BUILD.md) for the Colima arm64 Linux VM setup used to build
and boot `aarch64`.

---

## The build, stage by stage

The build system lives in [`scripts/`](scripts/) as numbered stages, orchestrated
by [`scripts/build.sh`](scripts/build.sh) and wrapped by a discoverable `Makefile`.

| Stage | Script | `make` target | What it does |
|------:|--------|---------------|--------------|
| 00 | `00-check-host.sh`     | `check`     | Verify host prerequisites (compiler, build tools, QEMU, disk space). |
| 10 | `10-fetch-sources.sh`  | `fetch`     | Download every upstream tarball and verify its pinned SHA-256. |
| 20 | `20-cross-toolchain.sh`| `toolchain` | Build the cross-toolchain: binutils, GCC, glibc, libstdc++ (`<arch>-verk-linux-gnu`). |
| 30 | `30-temp-tools.sh`     | `temp`      | Cross-build the LFS-style temporary tools into the target rootfs. |
| 40 | `40-final-system.sh`   | `system`    | Enter the chroot and build the final system: glibc → util-linux → D-Bus → **systemd**, the full GNU userland, plus vpk, verkbox, networking, SSH and `/etc` config. |
| 50 | `50-kernel.sh`         | `kernel`    | Build the Linux kernel (6.12 LTS) for the target arch. |
| 60 | `60-image.sh`          | `image`     | Assemble a bootable image (ext4 root disk / initramfs). |
|  — | `boot/qemu-<arch>.sh`  | `run`       | Boot the built system in QEMU (direct kernel boot, no bootloader). |

Version numbers and hashes are pinned in one place —
[`config/versions.sh`](config/versions.sh) — and per-arch settings live in
[`config/targets/`](config/targets/). Notable components: GCC 14.2.0, glibc 2.40,
binutils 2.43.1, Linux 6.12.9, systemd 256.7, bash 5.2.37, OpenSSL 3.4.0,
OpenSSH 9.9p1, dhcpcd 10.1.0.

---

## `vpk` — the Verk package manager

`vpk` builds packages from **recipes** and can cache the result as a binary `.vpk`
for instant reinstalls. It is deliberately tiny and readable: the only child
process it ever spawns is a recipe's own `build()`.

```sh
vpk update             # refresh the package index from the repo
vpk search [term]      # list available packages
vpk install <pkg>...   # resolve deps, fetch, verify sha256, build, install (+ cache a .vpk)
vpk build   <pkg>...   # build a .vpk without installing
vpk remove  <pkg>...   # remove installed package(s)
vpk list               # list installed packages ([base] packages are protected)
vpk info    <pkg>      # show recipe metadata + install status
```

A recipe is a handful of fields plus a shell `build()` that installs into
`$DESTDIR` (vpk merges that into `/` and records every file for clean removal):

```sh
name=curl
version=8.11.0
source=https://curl.se/download/curl-8.11.0.tar.xz
sha256=db59cf0d671ca6e7f5c2c5ec177084a33a79e04c97e71cf183a5cdea235054eb
depends="openssl zlib"
build() {
    ./configure --prefix=/usr --with-openssl --with-zlib --disable-static
    make
    make DESTDIR="$DESTDIR" install
}
```

Recipes live under [`pkg/recipes/`](pkg/recipes/) (installed to
`/etc/vpk/recipes/`); if a package isn't local, vpk fetches its recipe from the
repo. The **entire base system is registered in vpk's database**, so `vpk list`
shows the whole OS and vpk refuses to remove `[base]` packages. Adding a package
is as simple as dropping a new directory in `pkg/recipes/`.

---

## `verkbox` — VerkOS's own coreutils

One small multi-call binary ([`pkg/verkbox/verkbox.c`](pkg/verkbox/verkbox.c))
implements 33 classic tools as applets. Symlink an applet name to the binary and
it dispatches on `argv[0]`; it's also callable as `verkbox <applet> …` or listed
with `verkbox --list`:

```
true false echo cat pwd whoami nproc yes basename dirname head wc seq ls tail
cut rev uniq mkdir rmdir rm cp mv ln touch sort tr nl tac tee fold comm paste
```

Takeover is gradual and evidence-based: an applet only replaces its GNU
counterpart once it is flag-complete **and** differentially byte-exact against GNU
coreutils. Today `ls`, `cat`, `head`, `tail` and `wc` are verkbox; the rest ship
alongside GNU and get promoted as they qualify. It's the first piece of VerkOS's
own userland — same spirit as vpk: plain C, no dependencies, readable end to end.

---

## Project status

VerkOS is a **self-hosting distro** today: it builds software from source and
manages its own packages, on itself, with working login sessions.

- **First boot achieved (aarch64).** `make run ARCH=aarch64` boots Linux 6.12.9,
  mounts an ext4 root, runs **systemd 256.7 as PID 1**, reaches the Multi-User
  target and drops to a `root@verkos:~#` shell.
- **Full userland.** GCC, binutils and the complete GNU userland built natively
  into the rootfs; shadow provides real `login`/`passwd`.
- **Online & reachable.** dhcpcd brings up networking with DNS; OpenSSH `sshd`
  serves password and ed25519-key logins — verified end to end over QEMU port
  forwarding.
- **vpk + verkbox live.** Both were built and verified *on VerkOS itself*
  (`vpk install hello`/`htop`/`wget` build from source over HTTPS with our GCC).
- **verkfetch** — a dependency-free neofetch-style tool with VerkOS ASCII art.
- **x86_64 is build-ready.** Toolchain, temp tools and kernel build at native
  speed; the final in-chroot native build should be run on real x86 hardware (a
  cheap cloud VM), where it compiles natively and reliably.

Next up, in order: polish the full userland, run the final **x86_64** build on
real hardware, then GRUB / physical-hardware boot. See
[`docs/ROADMAP.md`](docs/ROADMAP.md) and [`docs/BUILD-PROGRESS.md`](docs/BUILD-PROGRESS.md)
for the detailed, checkbox-tracked state.

---

## Repository layout

```
verkos/
├── config/       # Pinned versions (versions.sh), os-release, per-arch targets
├── scripts/      # The build system — numbered LFS-style stages + shared lib/
├── pkg/
│   ├── vpk/      # The Verk package manager (C)
│   ├── verkbox/  # VerkOS's own coreutils (C)
│   └── recipes/  # vpk build recipes
├── boot/         # QEMU launchers per arch + a GRUB template
├── tools/        # verkfetch and friends
├── docs/         # Architecture, build guide, roadmap, decisions, progress
└── Makefile      # Convenience entry points over scripts/build.sh
```

Deeper reading: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md),
[`docs/DECISIONS.md`](docs/DECISIONS.md) (why glibc, why systemd, why distro-first),
and [`docs/BUILD.md`](docs/BUILD.md).

---

## Contributing

Contributions are welcome — bug fixes from real builds, new vpk recipes, and
verkbox applets that pass the byte-exact bar are all great first PRs.

- **Add a package:** create `pkg/recipes/<name>/recipe` (`name`, `version`,
  `source`, `sha256`, `depends`, `build()`) and add its version + hash to
  `config/versions.sh` if it's part of the base build.
- **Add or improve a verkbox applet:** implement it in
  `pkg/verkbox/verkbox.c`, register it in the applet table, and match GNU
  behaviour. An applet only shadows GNU once it's differentially byte-exact.
- **CI must stay green.** Every push runs hard gates: `bash -n` on all scripts
  (including the embedded in-chroot native builder) and config-sanity checks that
  `versions.sh` and every target parse and define their required variables.
  ShellCheck runs as advisory style feedback. See
  [`.github/workflows/ci.yml`](.github/workflows/ci.yml).

Please keep changes focused and pin real SHA-256 hashes for any new sources
(`make pin` / [`scripts/pin-hashes.sh`](scripts/pin-hashes.sh) helps).

---

## License

VerkOS's own code (build scripts, vpk, verkbox, tooling) is released under the
**MIT License** — see [`LICENSE`](LICENSE). VerkOS packages upstream free
software; each bundled component retains its own license.
