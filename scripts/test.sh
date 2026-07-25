#!/usr/bin/env bash
# test.sh — everything about VerkOS that can be checked on the build host,
# without building a distro or booting a VM. Run it with `make test`; CI runs
# the same script, so a green CI means these all held.
#
# The point is regression cover for things that have actually broken:
#   - the crypto that decides whether a kernel is allowed to boot,
#   - cpio symlinks (vcpio used to write them with an empty target, which
#     silently broke /bin/sh in the initramfs),
#   - the compression and archive tools round-tripping against the GNU ones,
#   - every C program still compiling, including the bootloader for both arches.
set -uo pipefail
cd "$(dirname "$0")/.."

pass=0; fail=0
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT

say()  { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
ok()   { printf '  \033[32mok\033[0m    %s\n' "$*"; pass=$((pass+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n' "$*"; fail=$((fail+1)); }
check(){ if eval "$2" >/dev/null 2>&1; then ok "$1"; else bad "$1"; fi; }

CC=${CC:-cc}

say "compiles"
for src in pkg/vpk/vpk.c pkg/verkbox/verkbox.c pkg/urverk/urverk.c pkg/vcpio/vcpio.c \
           pkg/vgzip/vgzip.c pkg/vbzip2/vbzip2.c pkg/vtar/vtar.c pkg/vsed/vsed.c \
           pkg/vawk/vawk.c pkg/vpager/vpager.c pkg/vboot/vsign.c \
           pkg/verkmod/verkmod.c pkg/vlogin/vlogin.c pkg/vmkfs/vmkfs.c \
           pkg/vinstall/vinstall.c; do
    name=$(basename "$src" .c)
    extra=""
    [ "$name" = vpk ] && extra="-lcurl -llzma -lz"
    [ "$name" = vawk ] && extra="-lm"      # fmod/pow/sqrt
    [ "$name" = vlogin ] && extra="-lcrypt"
    # vlogin needs libxcrypt (crypt_gensalt); skip where there is none
    if [ "$name" = vlogin ] && ! echo "#include <crypt.h>" | $CC -E - >/dev/null 2>&1; then
        printf '  \033[33mskip\033[0m  %s (no crypt.h here)\n' "$name"; continue
    fi
    # urverk is Linux-only (mount, cgroups); skip it elsewhere rather than fail
    if [ "$name" = urverk ] && [ "$(uname -s)" != Linux ]; then
        printf '  \033[33mskip\033[0m  %s (needs Linux headers)\n' "$name"; continue
    fi
    if $CC -O2 -w -o "$T/$name" "$src" $extra 2>"$T/$name.log"; then ok "$name"
    else bad "$name — $(head -1 "$T/$name.log")"; fi
done

say "ed25519 (the signatures vboot trusts)"
if $CC -O2 -w -o "$T/edtest" pkg/vboot/ed25519-test.c 2>/dev/null; then
    if "$T/edtest" >"$T/ed.log" 2>&1; then ok "RFC 8032 vectors and rejection tests"
    else bad "known-answer tests"; sed 's/^/      /' "$T/ed.log"; fi
else bad "ed25519-test does not compile"; fi

say "vsign (signing what boots)"
if [ -x "$T/vsign" ]; then
    head -c 200000 /dev/urandom > "$T/payload"
    "$T/vsign" keygen "$T/k.key" "$T/k.pub" >/dev/null 2>&1
    "$T/vsign" sign   "$T/k.key" "$T/payload" >/dev/null 2>&1
    check "a signed payload verifies"        "\"$T/vsign\" verify \"$T/k.pub\" \"$T/payload\""
    printf 'x' | dd of="$T/payload" bs=1 seek=1234 conv=notrunc status=none
    if "$T/vsign" verify "$T/k.pub" "$T/payload" >/dev/null 2>&1; then
        bad "a tampered payload is rejected"
    else ok "a tampered payload is rejected"; fi
fi

say "vcpio (symlinks used to come out empty and break /bin/sh)"
if [ -x "$T/vcpio" ]; then
    mkdir -p "$T/cp/d" && echo hello > "$T/cp/d/real"
    ( cd "$T/cp" && ln -sf d/real link && ln -sf /abs/path abslink )
    ( cd "$T/cp" && find . | "$T/vcpio" -o ) > "$T/a.cpio" 2>/dev/null
    mkdir -p "$T/out" && ( cd "$T/out" && "$T/vcpio" -i < "$T/a.cpio" ) >/dev/null 2>&1
    check "relative symlink survives a round trip" "[ \"\$(readlink '$T/out/link')\" = d/real ]"
    check "absolute symlink survives a round trip" "[ \"\$(readlink '$T/out/abslink')\" = /abs/path ]"
    check "file contents survive"                  "[ \"\$(cat '$T/out/d/real')\" = hello ]"
    if command -v cpio >/dev/null; then
        check "GNU cpio can read our archive" "cpio -t < '$T/a.cpio'"
    fi
fi

say "compression interoperates with the GNU tools"
if [ -x "$T/vgzip" ] && command -v gzip >/dev/null; then
    head -c 300000 /dev/urandom > "$T/g.bin"
    "$T/vgzip" -c "$T/g.bin" > "$T/g.gz" 2>/dev/null
    check "gzip reads what vgzip writes" "gzip -dc '$T/g.gz' | cmp -s - '$T/g.bin'"
    gzip -c "$T/g.bin" > "$T/g2.gz"
    check "vgzip reads what gzip writes" "'$T/vgzip' -dc '$T/g2.gz' | cmp -s - '$T/g.bin'"
fi
if [ -x "$T/vbzip2" ] && command -v bzip2 >/dev/null; then
    head -c 200000 /dev/urandom > "$T/b.bin"
    "$T/vbzip2" -c "$T/b.bin" > "$T/b.bz2" 2>/dev/null
    check "bzip2 reads what vbzip2 writes" "bzip2 -dc '$T/b.bz2' | cmp -s - '$T/b.bin'"
    # -dc used to match no flag at all, so the tool stayed in compress mode and
    # replaced the named file with a .bz2 of itself — losing the original
    check "vbzip2 -dc round-trips"        "'$T/vbzip2' -dc '$T/b.bz2' | cmp -s - '$T/b.bin'"
    check "vbzip2 -dc leaves the source"  "[ -f '$T/b.bz2' ]"
    if "$T/vbzip2" -Q "$T/b.bz2" >/dev/null 2>&1; then
        bad "an unknown flag is an error"
    else ok "an unknown flag is an error"; fi
    check "and did not eat the file"      "[ -f '$T/b.bz2' ]"
fi
if [ -x "$T/vgzip" ]; then
    check "vgzip -dc round-trips" "'$T/vgzip' -c '$T/g.bin' > '$T/g3.gz' && '$T/vgzip' -dc '$T/g3.gz' | cmp -s - '$T/g.bin'"
fi

say "vtar"
if [ -x "$T/vtar" ] && command -v tar >/dev/null; then
    mkdir -p "$T/tt" && echo hello > "$T/tt/f.txt"
    ( cd "$T/tt" && "$T/vtar" -cf a.tar f.txt ) 2>/dev/null
    check "GNU tar reads what vtar writes"    "tar -tf '$T/tt/a.tar'"
    # the traditional dashless form: 'tar xzf ...' is how most people write it
    ( cd "$T/tt" && "$T/vtar" cf trad.tar f.txt ) 2>/dev/null
    check "vtar accepts the dashless form"    "tar -tf '$T/tt/trad.tar'"
fi

say "text tools match GNU byte for byte"
if [ -x "$T/vsed" ]; then
    printf 'one\ntwo\nthree\n' > "$T/t.txt"
    for expr in 's/o/0/g' '2d' '$!d' 's/[aeiou]//g'; do
        if diff <("$T/vsed" "$expr" "$T/t.txt" 2>/dev/null) \
                <(sed "$expr" "$T/t.txt" 2>/dev/null) >/dev/null 2>&1
        then ok "sed $expr"; else bad "sed $expr"; fi
    done
fi
if [ -x "$T/vawk" ]; then
    printf '1 a\n2 b\n3 c\n' > "$T/a.txt"
    for prog in '{print $2}' '{s+=$1} END{print s}' '$1>1{print}'; do
        if diff <("$T/vawk" "$prog" "$T/a.txt" 2>/dev/null) \
                <(awk "$prog" "$T/a.txt" 2>/dev/null) >/dev/null 2>&1
        then ok "awk $prog"; else bad "awk $prog"; fi
    done
fi

say "vget TLS (a fetcher that accepts any certificate is a back door)"
if $CC -O2 -w -o "$T/vget" pkg/vget/vget.c -lssl -lcrypto 2>/dev/null; then
    ok "vget compiles"
    # These need the network; skip rather than fail when there is none.
    if timeout 20 "$T/vget" -I https://github.com >/dev/null 2>&1; then
        for bad in self-signed expired wrong.host; do
            if timeout 20 "$T/vget" -I "https://$bad.badssl.com" >/dev/null 2>&1
            then bad "$bad.badssl.com is refused"
            else ok  "$bad.badssl.com is refused"; fi
        done
        check "a valid certificate still works" "timeout 20 '$T/vget' -I https://github.com"
        check "-k still overrides"              "timeout 20 '$T/vget' -k -I https://self-signed.badssl.com"
    else
        printf '  \033[33mskip\033[0m  TLS checks (no network)\n'
    fi
else bad "vget does not compile"; fi

say "vmkfs makes a filesystem the world agrees is FAT32"
# The judge here is deliberately not us: fsck.fat and blkid are the tools that
# would reject a malformed volume, and the firmware has to agree with them.
if [ -x "$T/vmkfs" ]; then
    truncate -s 64M "$T/esp.img" 2>/dev/null || dd if=/dev/zero of="$T/esp.img" bs=1M count=64 status=none
    if "$T/vmkfs" -n VERKESP "$T/esp.img" 2>"$T/vmkfs.log"; then
        if command -v fsck.fat >/dev/null 2>&1; then
            check "fsck.fat accepts what vmkfs wrote" "fsck.fat -n '$T/esp.img'"
        else
            printf '  \033[33mskip\033[0m  fsck.fat check (dosfstools not installed)\n'
        fi
        if command -v blkid >/dev/null 2>&1; then
            check "blkid sees a vfat volume"  "blkid -o value -s TYPE  '$T/esp.img' | grep -qx vfat"
            check "blkid sees the label"      "blkid -o value -s LABEL '$T/esp.img' | grep -qx VERKESP"
        fi
        # A serial taken from the clock alone repeats for volumes made in the
        # same second, and then two disks cannot be told apart.
        truncate -s 64M "$T/esp2.img" 2>/dev/null || dd if=/dev/zero of="$T/esp2.img" bs=1M count=64 status=none
        "$T/vmkfs" -n VERKESP "$T/esp2.img" 2>/dev/null
        check "two filesystems made back to back get different serials" \
              "! cmp -s <(dd if='$T/esp.img' bs=1 skip=67 count=4 2>/dev/null) \
                        <(dd if='$T/esp2.img' bs=1 skip=67 count=4 2>/dev/null)"
        # Below ~33 MB the cluster count puts it outside FAT32 by definition.
        truncate -s 8M "$T/tiny.img" 2>/dev/null || dd if=/dev/zero of="$T/tiny.img" bs=1M count=8 status=none
        check "a volume too small for FAT32 is refused, not silently mangled" \
              "! '$T/vmkfs' '$T/tiny.img' 2>/dev/null"
    else bad "vmkfs failed — $(head -1 "$T/vmkfs.log")"; fi
else bad "vmkfs does not compile"; fi

say "verkmod indexes a module tree"
# Every bug below was real. A fake tree is enough: .modinfo is an ordinary ELF
# section, so a plain executable carrying one parses exactly like a .ko.
if [ -x "$T/verkmod" ]; then
    mk_ko() {  # mk_ko <path> <name> <depends>
        mkdir -p "$(dirname "$1")"
        cat > "$T/fake.c" <<EOF
static const char mi[] __attribute__((section(".modinfo"), used)) =
    "name=$2\0depends=$3\0license=GPL";
int main(void) { return 0; }
EOF
        $CC -O2 -w -o "$1" "$T/fake.c" 2>/dev/null
    }
    M="$T/fakeroot/usr/lib/modules/9.9.9-test"
    mk_ko "$M/kernel/fs/alpha.ko"    alpha    "beta-mod"
    mk_ko "$M/kernel/lib/beta_mod.ko" beta_mod ""
    mk_ko "$M/kernel/fs/gamma.ko"    gamma    "alpha,beta-mod"
    # a build/ symlink into the kernel source tree holds a second copy of every
    # module; indexing it doubled the index and resolved deps to the wrong file
    mkdir -p "$T/faketree/fs" && mk_ko "$T/faketree/fs/alpha.ko" alpha ""
    ln -sf "$T/faketree" "$M/build"

    "$T/verkmod" depmod -b "$T/fakeroot" 9.9.9-test >/dev/null 2>&1
    D="$M/modules.dep"
    if [ -s "$D" ]; then
        check "depmod indexes each module once (skips build/)" \
              "[ \"\$(wc -l < '$D')\" -eq 3 ]"
        check "depmod resolves a dependency written with a hyphen" \
              "grep -q '^kernel/fs/alpha.ko: kernel/lib/beta_mod.ko\$' '$D'"
        check "depmod records every dependency, not just the first" \
              "[ \"\$(grep -c ' ' <<<\"\$(grep '^kernel/fs/gamma.ko:' '$D')\")\" -ge 1 ] && \
               grep '^kernel/fs/gamma.ko:' '$D' | grep -q alpha.ko && \
               grep '^kernel/fs/gamma.ko:' '$D' | grep -q beta_mod.ko"
        check "depmod never points a dependency into the build tree" \
              "! grep -q 'build/' '$D'"
    else bad "depmod wrote no index"; fi
else bad "verkmod does not compile"; fi

say "shell scripts parse"
for f in scripts/*.sh scripts/lib/*.sh boot/*.sh config/*.sh config/targets/*.sh pkg/vboot/build.sh; do
    [ -f "$f" ] || continue
    bash -n "$f" 2>/dev/null || bad "syntax: $f"
done
ok "bash -n over every shell script"

say "the bootloader builds for both architectures"
# only where a toolchain for the target exists; a missing cross-compiler is not
# a test failure, it is a host without that toolchain
for arch in aarch64 x86_64; do
    cc_for_arch=""
    case "$arch-$(uname -m)" in
        aarch64-aarch64|x86_64-x86_64) cc_for_arch=$CC ;;
        *) command -v "$arch-linux-gnu-gcc" >/dev/null && cc_for_arch="$arch-linux-gnu-gcc" ;;
    esac
    if [ -z "$cc_for_arch" ]; then
        printf '  \033[33mskip\033[0m  vboot %s (no toolchain here)\n' "$arch"; continue
    fi
    if bash pkg/vboot/build.sh "$T/vboot-$arch" "$arch" >"$T/vb-$arch.log" 2>&1
    then ok "vboot $arch"; else bad "vboot $arch — $(tail -1 "$T/vb-$arch.log")"; fi
done

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
