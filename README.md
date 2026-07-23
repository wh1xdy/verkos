# VerkOS

[![CI](https://github.com/wh1xdy/verkos/actions/workflows/ci.yml/badge.svg)](https://github.com/wh1xdy/verkos/actions/workflows/ci.yml)

> A from-scratch, multi-architecture Linux distribution — built the hard way, on purpose.

**VerkOS** ("verk" = Swedish for *work / creation*) is a Linux From Scratch–style
distribution built from source, package by package. The goal is a real, usable,
eventually desktop-capable system that we understand top to bottom because we
built every layer of it ourselves.

Phase 1 is the distro (this repo). Phase 2, further down the road, is replacing
pieces of the stack with our own — up to and including our own kernel.

## Design at a glance

| Decision            | Choice                          | Why |
|---------------------|---------------------------------|-----|
| Approach            | Distro first (LFS-style), custom kernel later | Understand the whole stack before rewriting it |
| C library           | **glibc**                       | Maximum compatibility with off-the-shelf software; smooth path to desktop |
| Init / PID 1        | **systemd**                     | The real desktop standard; service management done properly |
| Architectures       | **x86_64**, **i686** (x86), **aarch64** (ARM) | Emulator + real hardware, 32/64-bit |
| Boot (dev)          | **QEMU direct kernel boot**     | Fast iteration, no bootloader needed |
| Boot (later)        | **GRUB**                        | Boot on real hardware |
| Build model         | Cross-toolchain bootstrap → native build in chroot/QEMU | The sane way to do glibc+systemd multi-arch |

See [`docs/DECISIONS.md`](docs/DECISIONS.md) for the full reasoning behind each choice.

## Repository layout

```
VerkOS/
├── config/            # Version pins, per-target settings, branding
│   ├── versions.sh    #   Pinned upstream package versions (single source of truth)
│   ├── os-release     #   /etc/os-release branding
│   └── targets/       #   Per-architecture build settings
├── scripts/           # The build system (LFS-style, numbered stages)
│   ├── lib/common.sh  #   Shared helpers: logging, download, verify, chroot
│   └── NN-*.sh        #   Ordered build stages
├── boot/              # QEMU launchers per arch + GRUB template
├── docs/              # Architecture, build guide, roadmap, decisions
└── Makefile           # Convenience entry points
```

## Quick start

> ⚠️ A full glibc+systemd build compiles for **hours** and needs **many GB** of
> disk. Run it on a real machine (your laptop), not a throwaway container.
> The full walkthrough is in [`docs/BUILD.md`](docs/BUILD.md).

```sh
# 1. Check your host has the required tools
make check

# 2. Fetch and verify all upstream sources
make fetch

# 3. Build everything for x86_64 (default target)
make ARCH=x86_64

# 4. Boot it in QEMU
make run ARCH=x86_64
```

Targets: `x86_64` (default), `i686`, `aarch64`.

## Status

🚧 **Early scaffolding.** The structure, build orchestration, docs and config are
in place. Individual build stages are being fleshed out — see
[`docs/ROADMAP.md`](docs/ROADMAP.md) for what's done and what's next.

## License

See [`LICENSE`](LICENSE). VerkOS packages upstream free software; each component
keeps its own license.
