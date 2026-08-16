# Where VerkOS stands

Rewritten 2026-07-28. The point of this file is to let a new session start
without reading the whole history: what is proved, what is merely written, and
what is known to be wrong.

`make test` — **253 checks**, all passing, run in the `verkbuild` container.
Run it on an idle machine: several checks boot QEMU and wait for a marker, and
two suites running at once fail each other's timeouts. The count is also
host-dependent — some checks are skipped without a network or a cross toolchain,
so a Mac host reports fewer and fails several on missing headers.

**The scheduler question is closed** (D-099 to D-101). It was never the
scheduler: `execve` set the address space and the table it fills in nineteen
lines apart, and a preemption in that window resumed a thread with TTBR0 on the
old space. The disk driver's sleep merely put a guaranteed blocking point in the
middle of it. The block driver is interrupt-driven now, on one processor and on
two.

---

## The two halves

VerkOS is a Linux distribution built from source, replacing third-party
components with its own — and, since 2026-07-26, **its own kernel** as well.
They are separate systems that share tools:

- **The distribution** boots Linux 6.12.9 through our own bootloader, installs
  itself five ways, and runs a userland where 97 command names are ours.
- **`stam`** is our own aarch64 kernel. It is not in any image and no build
  stage ships it; `pkg/stam/Makefile` builds it and `make test` boots it.

---

## What is verified by booting

Nothing below is inference. Each was checked by running a system.

### The distribution

| | |
|---|---|
| **Boot chain** | UEFI → vboot (ours) → kernel → initramfs → urverk → services → login, on aarch64 — **when the firmware publishes a device tree**. With ACPI-only firmware vboot's synthetic tree is enough for stam and not for Linux (D-072) |
| **Encrypted root** | `vinstall --encrypt` makes a LUKS2 container; urverk finds it by UUID, prompts, opens it, boots on `/dev/mapper/cryptroot` |
| **Dual boot** | `--root-part`/`--esp-part` install beside another system; its bootloader keeps its checksum and the firmware loads ours from a `Boot####` variable we wrote |
| **Immutable install** | `--immutable`: root read-only under a tmpfs overlay; a file written on the first boot is gone on the second |
| **Diskless install** | `--ramdisk`: one ext4 image on the ESP, loop-mounted with an overlay |
| **Accounts** | vlogin replaces shadow entirely; `su`/`passwd` are setuid and drop privilege completely |
| **Modules** | verkmod replaces kmod: `modprobe btrfs` pulls in five dependencies; `rmmod` refuses one in use |
| **Live ISO** | `make iso` → a 731 MB medium that boots, all four menu entries watched, installs from itself, and boots as a simulated USB stick |
| **The disk is ours** | `vpart` writes the GPT, `vmkfs` formats and fills the ESP, `vboot` is the loader in it. `make boot` goes from an empty file to a bootable disk using only code in this repository (D-065, D-066) |

### stam

| | |
|---|---|
| **Two boot paths, one source** | a raw arm64 `Image` for `-kernel`, and a UEFI application firmware or vboot loads. Nothing above `bootinfo.h` knows which (D-030, D-034) |
| **Its own hardware** | console, memory, CPUs and the timer's interrupt out of the device tree; GICv2 **and** v3; on ACPI-only machines vboot builds the tree (D-035, D-036, D-044) |
| **Processes** | preemptive round-robin, kernel in TTBR1, a low half per process, ELF out of a cpio initrd, `fork`/`exec`/`wait4`, copy-on-write, pages returned (D-037…D-064) |
| **System calls** | with Linux's aarch64 numbers. Every pointer checked against the caller's own tables before it is dereferenced (D-056) |
| **A shell** | `vsh`: pipelines, `>`, `>>`, `<`, `&`, Ctrl-C with process groups, and a script mode that makes it testable without driving a terminal (D-063) |
| **Signals** | raised, delivered on syscall *and* interrupt return, and **caught**: the frame goes on the program's own stack and comes back through a restorer it supplies (D-075, D-077) |
| **SMP** | PSCI `CPU_ON`; `current` per processor in `TPIDR_EL1`; a real spinlock; one lock over the scheduler, the pipes and the console. With `-smp 4` the work spreads over four processors, each with its own tick count (D-076, D-078) |
| **A disk** | virtio-blk over **MMIO and PCI**, the GPT `vpart` wrote, the FAT32 `vmkfs` formatted — read *and written*, with mtools and `fsck.fat` judging the result (D-079…D-082) |
| **The circle** | through real firmware: UEFI starts vboot, vboot builds a tree from ACPI with the PCI bridge out of MCFG, and the kernel it starts reads back the very `vboot.conf` vboot parsed to start it (D-081) |
| **No initrd** | with the userland on the medium, `/init` comes off the disk and so does every program the shell runs (D-083) |
| **Interrupt-driven disk** | the device's interrupt is found, enabled, acknowledged and **slept on** — a line printed from the handler says one arrived, and `make test` asserts it (D-086, D-101) |
| **Directories** | `getdents64`, `mkdirat`, `unlinkat`, and `ls`, `mkdir`, `rm` as ordinary programs (D-085) |
| **A network card** | virtio-net over MMIO **and** PCI, sharing the disk's transport. ARP resolves an address the kernel did not know, and an ICMP echo comes back with the payload we chose and both checksums verified — the one thing stam prints that another machine had to produce (D-103) |

---

## What exists but has never been booted

**Real hardware.** The ISO boots as a simulated USB stick under QEMU/AAVMF,
which exercises the same firmware path, but no physical machine has been asked
to boot it. There is deliberately no GPT on the ISO (D-026), so firmware that
reads nothing but GPT will not find it.

A **Qualcomm GENI console driver** and `vbootimg` exist for hardware nothing has
run them on, and D-045 says so rather than letting them sit beside the tested
work as equals.

---

## Known-wrong or fragile

- **A diagnostic reported a variable nobody wrote, for eleven entries.**
  `switches` was declared, read three times and never incremented, so "this core
  has had nothing to run for five seconds" printed on any idle boot, healthy or
  not — and D-088 through D-098 all reasoned from it. Fixed (D-099); the shape
  is the one to watch for.
- **A diagnostic changed what it diagnosed, twice in one day.** An idle-core
  report that counted its own loop passes called a busy machine stopped; a
  version that walked the thread table on every tick pushed a boot past the
  check waiting for it. Both are fixed and both are written down (D-088, D-090),
  because the shape recurs.
- **x86_64 has none of this.** The Proxmox VM is many months behind: no verkmod,
  vlogin, vmkfs, vpart, vinstall, viso, no native GCC, and nothing of stam. A
  clean build there would also prove nothing done here is aarch64-specific.
- **The rootfs is 958 MB**; a live ISO would be ~1.3 GB. About 250 MB of the
  medium is the compiler.
- **An interrupted `make install` of GCC leaves the system unable to rebuild
  itself.** Recovery depended on the cross toolchain still existing, which is
  luck rather than design.
- The build volume is 79 GB; a single failed GCC build can occupy 46 GB.

---

## The two things most worth remembering

**Real software finds what synthetic probes do not.** Building cryptsetup's
dependency chain put our userland in front of real build systems for the first
time at scale, and **twelve defects fell out** (D-021) — nearly all producing *a
wrong answer with a zero exit status*. A deliberate sweep for silent failures
earlier the same day found eighteen *other* problems and none of these.

> Synthetic probes test what we thought to ask. Real build systems ask things we
> did not think of.

**Ask the machine, do not reason about it.** The recurring defect across the
whole log is *a report that does not depend on what it reports*. The recurring
*cure* is making the program say where it got to: D-071 found a device-tree bug
by printing the tree, after four correct-looking fixes had not; D-081 found a
table of function pointers by reading a program counter; D-089 narrowed a day's
suspicion to one function by asking the scheduler what it had chosen — in one
run, after three entries of reasoning had each been plausibly wrong.

---

## Suggested next steps, in order

1. **Give the network to programs** — the kernel can ping and nothing else can.
   Sockets, or something smaller with the same effect, plus DHCP so the address
   is discovered rather than assumed.
2. **Boot it on real hardware** — everything else is simulation.
3. **Sync x86_64** — the second architecture is the real test of whether any of
   this was portable.
4. **Trim the live medium** — about 250 MB of the 731 MB is the compiler.

The scheduler question is no longer on this list: it is closed, see the top of
this file and D-099 to D-101.
