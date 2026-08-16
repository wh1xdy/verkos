# Working on VerkOS

VerkOS is a Linux distribution built from source, replacing third-party
components with our own — and, since 2026-07-26, its own kernel as well.

**Read `docs/STATUS.md` first.** It says what is proved by running it, what is
written but never run, and what is known fragile. `docs/DECISIONS.md` says why
things are the way they are; it is long, and worth searching rather than
reading.

## How the user wants to work

**Ask with the AskUserQuestion tool, always.** Never a question in prose — it
will not be seen as a question. This applies at every decision point *and every
time a piece of work is finished*: finishing is itself a reason to ask. Offer
concrete options with real trade-offs, say which you would pick and why, and let
them choose. They make the decisions.

**Answer in Swedish.** The code, comments, commit messages and documentation are
in English.

**They will correct you, and they will be right.** Take it directly, fix the
thing, and do not pad the correction with apology. On 2026-07-26 they caught a
sentence claiming a defect had "been public for weeks" when the repo was four
days old — a plausible detail filled in where a known one belonged. In documents
whose entire value is being true, that is the failure that matters most.

**Do not overstate.** If you cannot verify a number, do not give one. Say what
was run, what it printed, and what remains untested.

## What this project is about

Every claim is earned by running the thing, not by reading it. The recurring
defect across the whole log is **a report that does not depend on what it
reports** — a menu entry naming a flag nothing implemented, refusals exiting
zero, a kernel printing "time running" with no timer, a test that lied every
third run, an exception decoder printing an empty description of itself.

Ask of every output and every return value: **what does this print when the
thing it describes did not happen?**

And prefer proofs the machine can check over assertions a reader must believe.
The best test written here is a program that records its own process id, spins
long enough to be preempted, then checks the value survived and exits non-zero
if it did not.

## Traps that have cost real time

- **`/os` in the build container is a bind mount of the repo.** Editing a script
  while a build runs edits the file bash is executing; bash reads by byte offset
  and resumes mid-word. Never edit repo files during a build.
- **In `pkg/stam`, never take the address of a symbol from C.** The kernel is
  position-independent and nothing applies relocations, so anything routed
  through the GOT holds the address the image had when linked. Use a
  two-instruction assembly accessor (`adrp`/`add`). `make test` asserts
  `stam.efi` needs zero relocations.
- **Both stam builds must share code-generation flags.** They drifted once and a
  defect existed on one path and not the other.
- **The cross toolchain's sysroot *is* the rootfs.** Deleting the rootfs while
  keeping the toolchain stamps leaves stamps claiming a glibc whose files are
  gone.
- **Setuid and `argv[0]` dispatch do not mix** — D-051. Only `/usr/bin/su` and
  `/usr/bin/passwd` may carry the bit, and `make test` enforces the whole set.

## Git

Commits go as `wh1xdy <195197132+wh1xdy@users.noreply.github.com>`, set
per-repo. **Do not change it back to the icloud address** — GitHub attributes a
commit by its author email and that address resolves to a different account.
The global identity is different on purpose; leave it alone.

Commit and push only when asked. The repo (`github.com/wh1xdy/verkos`) is
private — a machine without GitHub credentials cannot clone or fetch it, so the
x86_64 build server is updated by pushing into its checkout over ssh rather than
by pulling from GitHub.

## Care

Confirm before anything destructive or outward-facing, and look at what you are
about to overwrite. **Never ask the keychain for a secret** — `gh auth status`
answers who pushes, and `security find-internet-password` without `-g` gives the
account name. On 2026-07-26 `-g` was used where the account name would have
done; the secret happened to be filtered out by a `grep` in the same pipeline,
which was the shape of the command and not a safeguard.

## Where things are

    pkg/stam/        our own kernel — boots two ways, runs a shell
    pkg/vboot/       our UEFI bootloader; builds the device tree on ACPI-only machines
    pkg/urverk/      init, and the initramfs stage
    pkg/vlogin/      account tools: a library plus vlogin, su, passwd
    pkg/vpk/         the package manager
    pkg/vinstall/    the installer — five modes
    pkg/viso/        the ISO 9660 writer
    pkg/vbootimg/    Android boot images, for Qualcomm boards
    scripts/         the build, stages 00–71; `scripts/test.sh` is `make test`

Build aarch64 in the `verkbuild` container on the Mac; x86_64 on the Proxmox VM.
See the project memory for both.
