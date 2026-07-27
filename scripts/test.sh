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
# The suite is architecture-independent except where it boots something.
ARCH_FOR_TESTS=${ARCH:-aarch64}

say "compiles"
for src in pkg/vpk/vpk.c pkg/verkbox/verkbox.c pkg/urverk/urverk.c pkg/vcpio/vcpio.c \
           pkg/vgzip/vgzip.c pkg/vbzip2/vbzip2.c pkg/vtar/vtar.c pkg/vsed/vsed.c \
           pkg/vawk/vawk.c pkg/vpager/vpager.c pkg/vboot/vsign.c \
           pkg/verkmod/verkmod.c pkg/vlogin/vlogin.c pkg/vlogin/su.c \
           pkg/vlogin/passwd.c pkg/vmkfs/vmkfs.c \
           pkg/vinstall/vinstall.c pkg/viso/viso.c pkg/vhex/vhex.c pkg/vtree/vtree.c \
           pkg/vdate/vdate.c pkg/vxargs/vxargs.c pkg/vip/vip.c; do
    name=$(basename "$src" .c)
    extra=""
    [ "$name" = vpk ] && extra="-lcurl -llzma -lz"
    [ "$name" = vawk ] && extra="-lm"      # fmod/pow/sqrt
    # vlogin, su and passwd are three programs over one library now (D-051).
    case "$name" in
        vlogin|su|passwd) extra="pkg/vlogin/vlogin_core.c -Ipkg/vlogin -lcrypt" ;;
    esac
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
    # The public half is derived, not stored beside the key. A build that reads
    # a stored .pub can be handed one that does not match, and then the boot
    # chain either refuses every kernel or trusts a key nobody holds.
    "$T/vsign" pubof "$T/k.key" "$T/k2.pub" >/dev/null 2>&1
    check "the public key derived from the private one is the same key" \
          "cmp -s $T/k.pub $T/k2.pub"
    # And the header that goes into vboot has to hold that key rather than the
    # zeroes it ships with, or verification is compiled out and everything
    # above this passes while nothing is checked.
    "$T/vsign" pubhdr "$T/k.pub" > "$T/vboot-key.h" 2>/dev/null
    check "the generated key header is not the all-zero default" \
          "grep -q '0x' $T/vboot-key.h && ! grep -q '\\[32\\] = { 0 }' $T/vboot-key.h"
    # vboot decides whether to check at all by looking at that array, so a build
    # with a real key must not take the unsigned path.
    check "vboot skips verification only when the key is all zeroes" \
          "grep -q 'if (!key_configured()) return 0' pkg/vboot/vboot.c"
    # The key header must be overridable, or signing means editing a file that
    # is committed — which is how a private key ends up in a public repository.
    check "vboot's key header can be overridden at build time" \
          "grep -q 'VBOOT_KEY_DIR' pkg/vboot/build.sh && grep -q '#include <vboot-key.h>' pkg/vboot/vboot.c"
    check "stage 70 signs the kernel and the initramfs when given a key" \
          "grep -q 'VERK_SIGN_KEY' scripts/70-boot-image.sh && grep -q 'sign_payloads' scripts/70-boot-image.sh"
    # Everything above says what the build was asked to do. This says what came
    # out: build vboot against a generated header and look for the key bytes in
    # the binary. A build that silently kept the checked-in zeroes passes every
    # structural check and verifies nothing.
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        mkdir -p "$T/keydir"
        "$T/vsign" pubhdr "$T/k.pub" > "$T/keydir/vboot-key.h" 2>/dev/null
        if VBOOT_KEY_DIR="$T/keydir" CC=aarch64-linux-gnu-gcc \
           OBJCOPY=aarch64-linux-gnu-objcopy LD=aarch64-linux-gnu-ld \
           bash pkg/vboot/build.sh "$T/vbkey" aarch64 >"$T/vbkey.log" 2>&1; then
            # The 32 key bytes, as they would sit in .rodata.
            od -An -tx1 -v "$T/k.pub" | tr -d " \n" > "$T/keyhex"
            od -An -tx1 -v "$T/vbkey/BOOTAA64.EFI" | tr -d " \n" > "$T/binhex"
            check "a vboot built with a key actually contains that key" \
                  "grep -qf $T/keyhex $T/binhex"
            # And the default build must not, or the check above proves nothing.
            if bash pkg/vboot/build.sh "$T/vbzero" aarch64 >/dev/null 2>&1; then
                od -An -tx1 -v "$T/vbzero/BOOTAA64.EFI" | tr -d " \n" > "$T/zerohex"
                check "the default build does not contain that key" \
                      "! grep -qf $T/keyhex $T/zerohex"
            fi
        else
            bad "vboot does not build against a generated key header"
        fi
    fi
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

say "verkbox sort can check order, not just produce it"
# libtool detects duplicate object names with "ar t LIB | sort | sort -uc".
# verkbox's sort had no -c at all and exited 2 on every input, so the check
# always "failed" and libtool declared "object name conflicts in archive" for
# archives that were fine — which is how GCC's libstdc++ refused to link.
if [ -x "$T/verkbox" ]; then
    check "sort -uc accepts sorted, unique input" \
          "printf 'a\nb\nc\n' | '$T/verkbox' sort -uc"
    check "sort -uc rejects a duplicate" \
          "! printf 'a\na\nb\n' | '$T/verkbox' sort -uc 2>/dev/null"
    check "sort -c rejects unsorted input" \
          "! printf 'b\na\n' | '$T/verkbox' sort -c 2>/dev/null"
    check "sort -c accepts a duplicate when -u is not given" \
          "printf 'a\na\n' | '$T/verkbox' sort -c"
    # LVM2 orders its symbol-version files with "sort -nt_ -k5". Without -t and
    # -k the command was rejected outright, the loop around it ran zero times,
    # and the linker version script came out empty — reported by ld as a syntax
    # error on line 0 of a file nobody had looked at.
    printf '.sym.DM_1_02_100\n.sym.DM_1_02_97\n.sym.Base\n.sym.DM_1_02_9\n' > "$T/keys.txt"
    if command -v sort >/dev/null 2>&1; then
        o=$("$T/verkbox" sort -nt. -k4 "$T/keys.txt" 2>&1)
        g=$(sort -nt. -k4 "$T/keys.txt" 2>&1)
        if [ "$o" = "$g" ]; then ok "sort -t/-k matches GNU sort"
        else bad "sort -t/-k: got '$o', sort gives '$g'"; fi
        printf 'b:2\na:10\nc:1\n' > "$T/k2.txt"
        o=$("$T/verkbox" sort -t: -k2 -n "$T/k2.txt" 2>&1); g=$(sort -t: -k2 -n "$T/k2.txt" 2>&1)
        if [ "$o" = "$g" ]; then ok "sort -t: -k2 -n matches GNU sort"
        else bad "sort -t: -k2 -n: got '$o', sort gives '$g'"; fi
    fi
    # An installer measuring the system it is about to image used "du -sm", and
    # verkbox's du had no -m at all, so it returned nothing. -x matters too:
    # measuring / on a live system otherwise counts /proc, /sys and every
    # mounted medium along with it.
    if command -v du >/dev/null 2>&1; then
        mkdir -p "$T/dut/sub"
        dd if=/dev/urandom of="$T/dut/f" bs=1024 count=300 status=none 2>/dev/null
        dd if=/dev/urandom of="$T/dut/sub/g" bs=1024 count=200 status=none 2>/dev/null
        for f in -sm -sk -sxm; do
            o=$("$T/verkbox" du $f "$T/dut" 2>&1 | cut -f1)
            g=$(du $f "$T/dut" 2>&1 | cut -f1)
            if [ "$o" = "$g" ]; then ok "du $f matches GNU du"
            else bad "du $f: got '$o', du gives '$g'"; fi
        done
    fi
    check "sort still sorts" \
          "[ \"\$(printf 'c\na\nb\n' | '$T/verkbox' sort | tr -d '\n')\" = abc ]"
fi

say "vawk splits fields the way awk is specified to"
# POSIX: an FS longer than one character is an extended regular expression.
# vawk used only its first character, so -F'[(, ]+' split on '[' and every
# field after the first came out empty — with a zero exit status. GCC builds
# its aarch64 tune table with exactly that expression and refused to continue
# because the table came out empty.
if [ -x "$T/vawk" ] && command -v awk >/dev/null 2>&1; then
    printf 'AARCH64_CORE("cortex-a34", cortexa34, x, y)\nAARCH64_CORE("cortex-a53", cortexa53, x, y)\n' > "$T/cores.def"
    printf '/^AARCH64_CORE/ { c = c $3 "," }\nEND { print c }\n' > "$T/cores.awk"
    ours=$("$T/vawk" -F'[(, ]+' -f "$T/cores.awk" "$T/cores.def" 2>&1)
    theirs=$(awk -F'[(, ]+' -f "$T/cores.awk" "$T/cores.def" 2>&1)
    if [ "$ours" = "$theirs" ]; then ok "vawk matches awk on a regex field separator"
    else bad "vawk -F regex: got '$ours', awk gives '$theirs'"; fi
    # An awk regex literal carries C-style escapes, and POSIX EREs do not:
    # /[ \t]+/ has to become space-or-tab before regcomp sees it, or it means
    # space-or-the-letter-t. GCC counts parentheses with exactly this and lost
    # 38 macro definitions, which surfaced as "'BINOP' was not declared".
    printf 'BEGIN { s = "a  b\tc"; n = gsub(/[ \t]+/, " ", s); print n, s }\n' > "$T/esc.awk"
    o=$("$T/vawk" -f "$T/esc.awk" </dev/null 2>&1)
    g=$(awk -f "$T/esc.awk" </dev/null 2>&1)
    if [ "$o" = "$g" ]; then ok "vawk understands \\t inside a regex literal"
    else bad "vawk regex escapes: got '$o', awk gives '$g'"; fi

    for fs in ':' '[0-9]+' '::' ' '; do
        o=$(printf 'a1b22c:d::e f\n' | "$T/vawk" -F"$fs" '{print NF ":" $2}' 2>&1)
        g=$(printf 'a1b22c:d::e f\n' | awk -F"$fs" '{print NF ":" $2}' 2>&1)
        if [ "$o" = "$g" ]; then ok "vawk matches awk with FS='$fs'"
        else bad "vawk FS='$fs': got '$o', awk gives '$g'"; fi
    done
fi

say "vsed handles the sed scripts real software writes"
# cmake's bootstrap substitutes its namespace with a multi-line block script.
# vsed entered the block and did nothing: the command written immediately after
# "{" was swallowed by the skip-to-next-separator, while commands on later lines
# still ran — so the header came out half-substituted and the build failed a
# thousand lines later with "not a member of cmSystemTools".
if [ -x "$T/vsed" ] && command -v sed >/dev/null 2>&1; then
    printf 'aaa\n' > "$T/sedin.txt"
    for script in '/./ {s/a/X/}' '/./ { s/a/X/ }' '{s/a/X/}' '/./ {s/a/X/;s/X/Y/}' '/b/ {s/a/X/}'; do
        ours=$("$T/vsed" "$script" "$T/sedin.txt" 2>&1)
        gnu=$(sed "$script" "$T/sedin.txt" 2>&1)
        if [ "$ours" = "$gnu" ]; then ok "vsed matches sed on: $script"
        else bad "vsed on '$script': got '$ours', sed gives '$gnu'"; fi
    done
fi

say "vsed writes the files its scripts ask for"
# gnulib generates its replacement headers with
#   sed -e 1h -e '1s|...|' -e 1G -n -e 'w FILE-t' IN
# vsed parsed the w command and then discarded it: exit 0, no file, and the
# build died on "mv: cannot stat 'lib/selinux/context.h-t'".
if [ -x "$T/vsed" ] && command -v sed >/dev/null 2>&1; then
    printf 'line one\nline two\nline three\n' > "$T/win.txt"
    rm -f "$T/w-ours.txt" "$T/w-gnu.txt"
    "$T/vsed" -e 1h -e '1s|one|ONE|' -e 1G -n -e "w $T/w-ours.txt" "$T/win.txt" >/dev/null 2>&1
    sed        -e 1h -e '1s|one|ONE|' -e 1G -n -e "w $T/w-gnu.txt"  "$T/win.txt" >/dev/null 2>&1
    check "vsed w writes a file at all" "[ -s '$T/w-ours.txt' ]"
    check "vsed w matches sed byte for byte" "cmp -s '$T/w-ours.txt' '$T/w-gnu.txt'"
fi

say "no tool reports success while doing nothing"
# A sweep for the shape that has bitten this project repeatedly: the tool
# notices the problem, prints about it, and exits 0 anyway. Every case below
# was a real defect found by running the tools, not by reading them.
for t in vgzip vhex vtree vdate vawk vcpio vxargs; do
    [ -x "$T/$t" ] || continue
    check "$t rejects an unknown option" "! '$T/$t' --zzz-not-a-real-option </dev/null 2>/dev/null"
done
if [ -x "$T/vsed" ]; then
    check "vsed fails on a file it cannot read" "! '$T/vsed' s/a/b/ /no/such/file 2>/dev/null"
fi
if [ -x "$T/vawk" ]; then
    check "vawk fails on a file it cannot read" "! '$T/vawk' '{print}' /no/such/file 2>/dev/null"
fi
if [ -x "$T/vtree" ]; then
    check "vtree fails on a directory that is not there" "! '$T/vtree' /no/such/dir 2>/dev/null"
fi
if [ -x "$T/vdate" ]; then
    # Silently ignoring -d printed the current time as though it were the asked-for one.
    check "vdate refuses a -d form it cannot parse" "! '$T/vdate' -d 'next tuesday' 2>/dev/null"
    check "vdate fails on -r with a missing file" "! '$T/vdate' -r /no/such/file 2>/dev/null"
fi
if [ -x "$T/vcpio" ]; then
    head -c 2048 /dev/urandom > "$T/notcpio.bin"
    check "vcpio fails on something that is not an archive" \
          "! '$T/vcpio' -i < '$T/notcpio.bin' 2>/dev/null"
    # An archived name must never be able to write outside where it is unpacked.
    mkdir -p "$T/cp/inner"; echo outside > "$T/cp/outside.txt"
    ( cd "$T/cp/inner" && echo ../outside.txt | "$T/vcpio" -o > "$T/cp/trav.cpio" ) 2>/dev/null
    mkdir -p "$T/cp/dest"
    check "vcpio refuses an entry that climbs out with .." \
          "! ( cd '$T/cp/dest' && '$T/vcpio' -i < '$T/cp/trav.cpio' ) 2>/dev/null"
    # Archive a file by its absolute path, then change that file. If vcpio
    # honours the leading / it overwrites the original and the sentinel is
    # gone; if it strips it, the sentinel survives and a copy appears inside.
    #
    # The first version of this check compared modification times with
    # `find -newer`, and timestamps land in the same second often enough that
    # it passed or failed at random. A check that is only sometimes right is
    # worse than none: it teaches you to disregard a red result.
    ( cd "$T/cp" && echo "$T/cp/outside.txt" | "$T/vcpio" -o > "$T/cp/abs.cpio" ) 2>/dev/null
    echo SENTINEL-NOT-OVERWRITTEN > "$T/cp/outside.txt"
    mkdir -p "$T/cp/dest2"
    ( cd "$T/cp/dest2" && "$T/vcpio" -i < "$T/cp/abs.cpio" ) 2>/dev/null
    check "vcpio strips a leading / instead of writing outside" \
          "grep -q SENTINEL-NOT-OVERWRITTEN '$T/cp/outside.txt' && find '$T/cp/dest2' -name outside.txt | grep -q ."
fi

say "vtar refuses what it cannot extract"
# This one bit for real: `tar -xf foo.tar.xz` extracted nothing and exited 0,
# because vtar only knew -z and never checked the ustar signature. A build
# resumed after the verkbox takeover then failed with no useful error.
if [ -x "$T/vtar" ]; then
    mkdir -p "$T/tsrc/sub"
    echo one > "$T/tsrc/a.txt"; echo two > "$T/tsrc/sub/b.txt"
    ( cd "$T" && tar -cf t.tar tsrc && tar -czf t.tar.gz tsrc \
                 && tar -cjf t.tar.bz2 tsrc && tar -cJf t.tar.xz tsrc ) 2>/dev/null
    for a in t.tar t.tar.gz t.tar.bz2 t.tar.xz; do
        [ -f "$T/$a" ] || continue
        rm -rf "$T/tout"; mkdir -p "$T/tout"
        if ( cd "$T/tout" && "$T/vtar" -xf "../$a" ) 2>/dev/null &&
           cmp -s "$T/tout/tsrc/a.txt" "$T/tsrc/a.txt"
        then ok "vtar extracts $a (compression detected from the file)"
        else bad "vtar could not extract $a"; fi
    done
    # Archives older than POSIX have no ustar magic and are still valid — popt
    # still ships one, and requiring the magic rejected it. The header checksum
    # is the portable test, so make sure an old-format archive still works.
    if tar --format=v7 -cf "$T/v7.tar" -C "$T" tsrc 2>/dev/null; then
        rm -rf "$T/v7out"; mkdir -p "$T/v7out"
        if ( cd "$T/v7out" && "$T/vtar" -xf "$T/v7.tar" ) 2>/dev/null &&
           cmp -s "$T/v7out/tsrc/a.txt" "$T/tsrc/a.txt"
        then ok "vtar extracts a pre-POSIX (v7) archive"
        else bad "vtar cannot read an old-format tar"; fi
    fi

    # Timestamps are not cosmetic: make and the autotools decide what to rebuild
    # by comparing them, so stamping everything "now" made an extracted source
    # tree look newer than its own generated files and triggered a regeneration
    # needing tools that are not installed.
    touch -d "2020-01-02 03:04:05" "$T/tsrc/a.txt"
    ( cd "$T" && tar -cf stamp.tar tsrc ) 2>/dev/null
    rm -rf "$T/stampout"; mkdir -p "$T/stampout"
    ( cd "$T/stampout" && "$T/vtar" -xf "$T/stamp.tar" ) 2>/dev/null
    check "vtar restores the archived modification time" \
          "[ \"\$(stat -c%Y '$T/stampout/tsrc/a.txt' 2>/dev/null)\" = \"\$(stat -c%Y '$T/tsrc/a.txt')\" ]"

    # A tar header holds 100 bytes of name; longer paths are carried in a pax
    # header, a GNU LongLink entry, or the ustar prefix field. Handling none of
    # them did not merely lose files — the following header still had a
    # truncated name, so contents landed at the wrong path. Extracting cmake
    # this way lost 465 files and overwrote others.
    deep="$T/tsrc/a_directory_with_quite_a_long_name/and_another_level_that_is_also_long"
    mkdir -p "$deep"
    echo deepfile > "$deep/a_file_whose_full_path_comfortably_exceeds_one_hundred_characters.txt"
    ( cd "$T" && tar -cf long.tar tsrc ) 2>/dev/null
    rm -rf "$T/longout"; mkdir -p "$T/longout"
    ( cd "$T/longout" && "$T/vtar" -xf "$T/long.tar" ) 2>/dev/null
    check "vtar keeps paths longer than 100 characters" \
          "cmp -s '$T/longout/tsrc/a_directory_with_quite_a_long_name/and_another_level_that_is_also_long/a_file_whose_full_path_comfortably_exceeds_one_hundred_characters.txt' \
                  '$deep/a_file_whose_full_path_comfortably_exceeds_one_hundred_characters.txt'"

    # GCC installs its headers with "tar -cf - . | tar xpf -". Two things had to
    # work for that: the p flag (preserve permissions), which vtar rejected as
    # unknown, and "-" meaning standard input/output, which vtar treated as a
    # file name — so the archive was written to a file literally called "-".
    rm -rf "$T/pipesrc" "$T/pipedst"; mkdir -p "$T/pipesrc/sub" "$T/pipedst"
    echo through > "$T/pipesrc/f"; echo nested > "$T/pipesrc/sub/g"
    ( cd "$T/pipesrc" && "$T/vtar" -cf - . ) 2>/dev/null | ( cd "$T/pipedst" && "$T/vtar" xpf - ) 2>/dev/null
    check "vtar pipes through stdin/stdout with -f -" \
          "cmp -s '$T/pipedst/f' '$T/pipesrc/f' && cmp -s '$T/pipedst/sub/g' '$T/pipesrc/sub/g'"
    check "vtar accepts the p flag instead of refusing it" \
          "( cd '$T/pipesrc' && '$T/vtar' -cf '$T/p.tar' . ) && ( cd '$T/pipedst' && '$T/vtar' xpf '$T/p.tar' )"

    # The important half: never succeed having done nothing.
    echo "definitely not a tar archive" > "$T/notatar"
    head -c 4096 /dev/urandom > "$T/rand.bin"
    echo hello | gzip > "$T/plain.gz" 2>/dev/null
    check "vtar rejects a short non-archive"     "! '$T/vtar' -xf '$T/notatar' 2>/dev/null"
    check "vtar rejects random bytes"            "! '$T/vtar' -xf '$T/rand.bin' 2>/dev/null"
    check "vtar rejects a gzip that is not a tar" "! '$T/vtar' -xf '$T/plain.gz' 2>/dev/null"
else bad "vtar does not compile"; fi

say "viso writes an ISO that boots"
# The one field where a mistake is silent is the El Torito validation entry's
# checksum: all sixteen little-endian 16-bit words must sum to zero, and
# firmware that dislikes it simply declines to boot with no diagnostic. Note
# that xorriso's -report_el_torito does NOT check this — it reports a corrupted
# catalogue as if it were fine — so the invariant is asserted directly.
if [ -x "$T/viso" ] && [ -x "$T/vmkfs" ]; then
    truncate -s 40M "$T/esp.iso.img" 2>/dev/null || dd if=/dev/zero of="$T/esp.iso.img" bs=1M count=40 status=none
    "$T/vmkfs" -n VERKESP "$T/esp.iso.img" 2>/dev/null
    printf 'payload\n' > "$T/payload.img"
    if "$T/viso" -V VERKLIVE -e "$T/esp.iso.img" -o "$T/t.iso" "$T/esp.iso.img" "$T/payload.img" 2>/dev/null; then
        check "viso writes something file(1) calls a bootable ISO 9660" \
              "file '$T/t.iso' | grep -q 'ISO 9660'"
        # the checksum invariant, and that the check can fail
        cat > "$T/eltorito.py" <<'PYEOF'
import sys
data = open(sys.argv[1], "rb").read()
cat = data[19*2048:19*2048+64]
total = sum(cat[i] | (cat[i+1] << 8) for i in range(0, 32, 2)) & 0xffff
ok = (cat[0] == 0x01 and cat[1] == 0xEF and cat[30] == 0x55 and cat[31] == 0xAA
      and total == 0 and cat[32] == 0x88 and cat[33] == 0x00)
sys.exit(0 if ok else 1)
PYEOF
        if command -v python3 >/dev/null 2>&1; then
            check "viso's El Torito entry is valid (UEFI, no emulation, checksum sums to zero)" \
                  "python3 '$T/eltorito.py' '$T/t.iso'"
            cp "$T/t.iso" "$T/bad.iso"
            printf '\000\000' | dd of="$T/bad.iso" bs=1 seek=$((19*2048+28)) conv=notrunc 2>/dev/null
            check "and that check actually fails on a corrupted catalogue" \
                  "! python3 '$T/eltorito.py' '$T/bad.iso'"
        fi
        if command -v xorriso >/dev/null 2>&1; then
            check "xorriso recognises the boot record" \
                  "xorriso -indev '$T/t.iso' -report_el_torito plain 2>&1 | grep -q 'El Torito boot img'"
            # The hybrid MBR is what lets the ISO boot from a USB stick: firmware
            # then finds an EFI partition on an ordinary block device rather than
            # an El Torito catalogue on a CD. xorriso resolves the partition back
            # to the file it covers, which is the check worth making.
            check "the hybrid MBR partition resolves to a file on the ISO" \
                  "xorriso -indev '$T/t.iso' -report_system_area plain 2>&1 | grep -qE 'MBR partition path.*:.*/[A-Z0-9_]+\.[A-Z0-9_]+'"
        fi
        if command -v python3 >/dev/null 2>&1; then
            cat > "$T/mbr.py" <<'PYEOF'
import sys
d = open(sys.argv[1], "rb").read(512)
e = d[446:462]
lba = int.from_bytes(e[8:12], "little")
cnt = int.from_bytes(e[12:16], "little")
ok = (d[510] == 0x55 and d[511] == 0xAA and e[4] == 0xEF and lba > 0 and cnt > 0)
sys.exit(0 if ok else 1)
PYEOF
            # ISO 9660 names carry exactly one dot. A file called
            # "esp.iso.img" must not become "ESP.ISO.IMG", which no reader has
            # to accept — and which stopped xorriso naming the file a partition
            # covers.
            cat > "$T/dots.py" <<'PYEOF'
import sys
d = open(sys.argv[1], "rb").read()
root = d[22*2048:23*2048]           # viso puts the root directory at LBA 22
off, bad = 0, 0
while off < 2048 and root[off]:
    ln, nlen = root[off], root[off+32]
    name = root[off+33:off+33+nlen].decode("latin1")
    if nlen > 1 and name.count(".") > 1:
        print("multi-dot name:", name); bad = 1
    off += ln
sys.exit(bad)
PYEOF
            check "viso gives every file a single-dot name" "python3 '$T/dots.py' '$T/t.iso'"
            check "the MBR names one 0xEF partition and is signed 55AA" \
                  "python3 '$T/mbr.py' '$T/t.iso'"
        fi
    else bad "viso could not write an image"; fi
else
    printf '  \033[33mskip\033[0m  viso (needs viso and vmkfs)\n'
fi

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

say "vboot recognises a kernel whatever case the filesystem uses"
# vboot compared "vmlinuz" byte for byte in lower case, so on a volume that
# hands back upper-case names — an ISO 9660 without Rock Ridge gives
# VMLINUZ.;1 — it would find no kernel and offer no entry to boot.
cat > "$T/kn.c" <<'KNEOF'
#include <stdio.h>
typedef unsigned short CHAR16;
#include "kernelname.h"
static CHAR16 *w(const char *a, CHAR16 *b) { int i = 0; for (; a[i]; i++) b[i] = (CHAR16)a[i]; b[i] = 0; return b; }
int main(void) {
    CHAR16 b[64];
    struct { const char *n; int want; } cases[] = {
        {"vmlinuz", 1}, {"VMLINUZ", 1}, {"VMLINUZ.;1", 1}, {"VmLiNuZ-6.12.9", 1},
        {"vmlinuz-6.12.9", 1}, {"vmlinu", 0}, {"xvmlinuz", 0}, {"initramfs.img", 0},
        {"", 0}, {"VMLINU", 0},
    };
    int bad = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++)
        if (name_is_kernel(w(cases[i].n, b)) != cases[i].want) {
            printf("%s: got %d\n", cases[i].n, name_is_kernel(w(cases[i].n, b)));
            bad = 1;
        }
    return bad;
}
KNEOF
if $CC -O2 -w -I pkg/vboot -o "$T/kn" "$T/kn.c" 2>/dev/null; then
    check "vboot matches vmlinuz in any case, and nothing else" "'$T/kn'"
else
    printf '  \033[33mskip\033[0m  vboot kernel-name matching (would not compile)\n'
fi

say "the bootloader builds for both architectures"
# only where a toolchain for the target exists; a missing cross-compiler is not
# a test failure, it is a host without that toolchain
# Finding a cross compiler is not enough: build.sh defaults to the host's own
# gcc and objcopy, and the host's objcopy does not know the efi-app target of a
# foreign architecture. It fails with "invalid bfd target", which reads like a
# broken build script rather than the wrong tool being used. Pass the whole
# triple, not just the compiler.
for arch in aarch64 x86_64; do
    cc_for_arch=""; prefix=""
    case "$arch-$(uname -m)" in
        aarch64-aarch64|x86_64-x86_64) cc_for_arch=$CC ;;
        *) if command -v "$arch-linux-gnu-gcc" >/dev/null; then
               cc_for_arch="$arch-linux-gnu-gcc"; prefix="$arch-linux-gnu-"
           fi ;;
    esac
    if [ -z "$cc_for_arch" ]; then
        printf '  \033[33mskip\033[0m  vboot %s (no toolchain here)\n' "$arch"; continue
    fi
    if CC="$cc_for_arch" OBJCOPY="${prefix}objcopy" LD="${prefix}ld" \
       bash pkg/vboot/build.sh "$T/vboot-$arch" "$arch" >"$T/vb-$arch.log" 2>&1
    then ok "vboot $arch"; else bad "vboot $arch — $(tail -1 "$T/vb-$arch.log")"; fi
done


# --- boot options must exist ------------------------------------------------
# The rescue menu entry named urverk.rescue for as long as vboot has existed
# and nothing implemented it: choosing it booted exactly like the normal entry,
# silently. Every rd.*/urverk.* token a boot configuration hands to urverk must
# be a token urverk actually reads.
say "boot options a menu offers must be options urverk reads"
# A path that no longer exists must fail, not be skipped. This list named
# scripts/70-boot.sh, which has been 70-boot-image.sh for as long as it has
# existed, so one of the four files was silently not checked — a check that
# quietly covers less than it claims is the defect this whole file is about.
for cfg in pkg/vboot/vboot.conf scripts/71-iso.sh scripts/70-boot-image.sh pkg/vinstall/vinstall.c; do
    [ -f "$cfg" ] || { bad "boot-option check names $cfg, which does not exist"; continue; }
    # [a-z_.]+, not [a-z_]+: a token may be dotted twice (rd.luks.uuid), and
    # stopping at the second dot compares a prefix that nothing ever reads.
    for tok in $(grep -oE '\b(rd|urverk)\.[a-z_.]+' "$cfg" 2>/dev/null | sed 's/\.$//' | sort -u); do
        if grep -q "\"$tok\"" pkg/urverk/urverk.c; then
            ok "$tok is read by urverk ($(basename "$cfg"))"
        else
            bad "$(basename "$cfg") offers $tok but urverk never reads it"
        fi
    done
done

# --- the ESP must be FAT32 --------------------------------------------------
# Left to choose, dosfstools picks the FAT type from the cluster count, and it
# chose FAT16 at both sizes VerkOS builds — 48 MiB for the ISO, 160 MiB for the
# boot disk — while every comment and document around it said FAT32. UEFI
# requires FAT32 for an ESP on fixed media; AAVMF and OVMF accept FAT16 anyway,
# which is the only reason this ever booted. Nothing reported it, because
# mkfs.vfat succeeded and the firmware did not object.
say "vmkfs writes files into the volume it made (no mtools, no dosfstools)"
if $CC -O2 -w -o "$T/vmkfs" pkg/vmkfs/vmkfs.c 2>/dev/null; then
    ok "vmkfs compiles"
    V=$T/vmkfs
    dd if=/dev/zero of="$T/esp.img" bs=1M count=48 status=none 2>/dev/null
    "$V" -n VERKESP "$T/esp.img" >/dev/null 2>&1
    before=$(stat -c%s "$T/esp.img" 2>/dev/null || stat -f%z "$T/esp.img")

    # A file bigger than 32 MiB, because every cluster number below that fits in
    # sixteen bits: a writer that forgets DIR_FstClusHI works perfectly until
    # the first file past cluster 65535, which on this build is the kernel.
    head -c 34000000 /dev/urandom > "$T/big.bin" 2>/dev/null
    printf 'hello\n' > "$T/small.txt"
    : > "$T/empty.bin"

    "$V" --mkdir "$T/esp.img" /EFI          >/dev/null 2>&1
    "$V" --mkdir "$T/esp.img" /EFI/BOOT     >/dev/null 2>&1
    "$V" --copy  "$T/esp.img" "$T/big.bin"   /vmlinuz        >/dev/null 2>&1
    "$V" --copy  "$T/esp.img" "$T/small.txt" /initramfs.img  >/dev/null 2>&1
    "$V" --copy  "$T/esp.img" "$T/small.txt" /vboot.conf     >/dev/null 2>&1
    "$V" --copy  "$T/esp.img" "$T/empty.bin" /empty.bin      >/dev/null 2>&1
    "$V" --copy  "$T/esp.img" "$T/small.txt" /EFI/BOOT/BOOTAA64.EFI >/dev/null 2>&1

    # The image must not have grown. The FAT is over-provisioned, so there are
    # entries past the last real cluster that read as free; allocating one
    # writes past the end, a regular file simply extends, and stage 70's
    # `dd ... seek=1 conv=notrunc` then puts those bytes over the backup GPT.
    after=$(stat -c%s "$T/esp.img" 2>/dev/null || stat -f%z "$T/esp.img")
    check "populating the volume does not grow the image past its end" \
          "[ '$before' = '$after' ]"

    # Twenty long names, which forces the root past one cluster. A cluster holds
    # sixteen entries and a long name costs two of them, so this is not an edge
    # case — it is the eighth file. The bug it catches: the free slot left
    # behind when a directory grows stays 0x00, which does not mean "free", it
    # means "the directory ends here" — and every entry in every later cluster
    # becomes invisible to anything that honours the marker.
    for i in $(seq 1 20); do
        "$V" --copy "$T/esp.img" "$T/small.txt" "/a-long-file-name-$i.txt" >/dev/null 2>&1
    done

    # The judges are deliberately not ours.
    if command -v fsck.fat >/dev/null 2>&1; then
        if fsck.fat -n "$T/esp.img" >"$T/fsck.log" 2>&1; then
            ok "fsck.fat accepts the volume vmkfs filled"
        else
            bad "fsck.fat rejects it: $(tail -1 "$T/fsck.log")"
        fi
        check "fsck.fat renames nothing (no duplicate short names)" \
              "! grep -q 'Renamed to' $T/fsck.log"
    else
        printf '  \033[33mskip\033[0m  fsck.fat (not installed)\n'
    fi
    if command -v mcopy >/dev/null 2>&1 && command -v mdir >/dev/null 2>&1; then
        # Byte-for-byte, because a structural check passes on a file whose tail
        # is zeroes and whose size field is right.
        mcopy -i "$T/esp.img" ::/vmlinuz "$T/back.bin" >/dev/null 2>&1
        check "a 34 MB file reads back byte-identical (past cluster 65535)" \
              "cmp -s $T/back.bin $T/big.bin"
        mcopy -i "$T/esp.img" ::/EFI/BOOT/BOOTAA64.EFI "$T/back2.bin" >/dev/null 2>&1
        check "a file in a subdirectory reads back byte-identical" \
              "cmp -s $T/back2.bin $T/small.txt"
        check "mtools sees all twenty long names past the first cluster" \
              "[ \"$(mdir -i $T/esp.img :: 2>/dev/null | grep -c 'a-long-file-name')\" = 20 ]"
    else
        printf '  \033[33mskip\033[0m  mtools round-trip (not installed)\n'
    fi

    # A mode that is present must be obeyed or refused, never ignored: this
    # used to fall through to the formatter, one option away from reformatting
    # the image it was asked to add a file to.
    check "a misplaced --copy is refused, not treated as a format" \
          "! $V -v --copy $T/nosuch.img $T/small.txt /x >/dev/null 2>&1"
else
    bad "vmkfs does not compile"
fi

# --- the distro boots, under firmware, with no help ------------------------
# Five rounds of diagnosis went into a machine that took the kernel and said
# nothing (D-068 through D-072), and not one of them would have been needed if
# something had booted the thing. D-055 closed this gap for stam and left it
# open for the distribution.
#
# Deliberately plain `virt`: ACPI only, no device tree from the firmware. That
# is the configuration that was broken, and the one a real ARM server is.
say "the distribution boots under UEFI firmware"
BOOTIMG=work/$ARCH_FOR_TESTS/out/verkos-boot-$ARCH_FOR_TESTS.img
ROOTIMG=work/$ARCH_FOR_TESTS/out/verkos-$ARCH_FOR_TESTS.ext4
FW=""
for c in /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
         /usr/share/edk2/aarch64/QEMU_EFI.fd; do
    [ -f "$c" ] && { FW="$c"; break; }
done
if [ -f "$BOOTIMG" ] && [ -f "$ROOTIMG" ] && [ -n "$FW" ] && \
   command -v qemu-system-aarch64 >/dev/null 2>&1; then
    # snapshot=on above: the test must not write to the images it is testing,
    # and must not fight another QEMU for a lock on them. A check that mutates
    # its own subject passes once.
    D=$T/distro; mkdir -p "$D"
    dd if=/dev/zero of="$D/code.fd" bs=1M count=64 status=none 2>/dev/null
    dd if="$FW" of="$D/code.fd" conv=notrunc status=none 2>/dev/null
    dd if=/dev/zero of="$D/vars.fd" bs=1M count=64 status=none 2>/dev/null
    ( qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 2048 -smp 2 \
        -display none -no-reboot \
        -drive if=pflash,format=raw,file="$D/code.fd",readonly=on \
        -drive if=pflash,format=raw,file="$D/vars.fd" \
        -drive if=none,id=rootdisk,file="$ROOTIMG",format=raw,snapshot=on \
        -device virtio-blk-pci,drive=rootdisk \
        -drive if=none,id=espdisk,file="$BOOTIMG",format=raw,snapshot=on \
        -device virtio-blk-pci,drive=espdisk,bootindex=0 \
        -serial "file:$D/log" </dev/null >/dev/null 2>&1 & echo $! > "$D/pid" )
    # Waiting for a marker rather than a fixed time, for the reason D-063 spells
    # out: a command that failed is otherwise indistinguishable from one that is
    # slow. Generous, because this is a whole distribution coming up.
    for _ in $(seq 1 150); do
        sleep 1
        grep -aqE 'root@verkos|login:|Kernel panic' "$D/log" 2>/dev/null && break
    done
    kill "$(cat "$D/pid")" 2>/dev/null
    wait "$(cat "$D/pid")" 2>/dev/null

    check "the firmware finds the partition table vpart wrote" \
          "grep -aq 'BdsDxe: starting' $D/log"
    check "vboot runs, out of the filesystem vmkfs wrote" \
          "grep -aq 'vboot: VerkOS' $D/log"
    # The kernel is the file that crosses cluster 65535 — a writer that got
    # DIR_FstClusHI wrong produces this log up to a kernel that is short.
    check "vboot reads the whole kernel back off that filesystem" \
          "grep -aqE 'kernel loaded, [0-9]{8}' $D/log"
    # This is the one the five rounds were about: with ACPI-only firmware the
    # kernel used to take the handover and never speak again.
    check "Linux runs and says which machine it is on" \
          "grep -aq 'Linux version' $D/log"
    check "the root filesystem is found and mounted" \
          "grep -aq 'urverk' $D/log && ! grep -aq 'root device never appeared' $D/log"
    check "the distribution reaches a shell" \
          "grep -aq 'root@verkos' $D/log"
    check "nothing panicked on the way" \
          "! grep -aq 'Kernel panic' $D/log"
else
    printf '  \033[33mskip\033[0m  booting the distribution (needs a built image and UEFI firmware)\n'
fi

say "vpart writes the partition table (no parted)"
if $CC -O2 -w -o "$T/vpart" pkg/vpart/vpart.c 2>/dev/null; then
    ok "vpart compiles"
    dd if=/dev/zero of="$T/disk.img" bs=1M count=64 status=none 2>/dev/null
    "$T/vpart" "$T/disk.img" --esp 32 >/dev/null 2>&1
    # A protective MBR, so a tool that reads only MBRs sees an occupied disk
    # rather than an empty one it might offer to partition.
    check "the protective MBR is type 0xEE with a boot signature" \
          "[ \"$(dd if=$T/disk.img bs=1 skip=450 count=1 2>/dev/null | od -An -tx1 | tr -d ' ')\" = ee ] && \
           [ \"$(dd if=$T/disk.img bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | tr -d ' ')\" = 55aa ]"
    check "the primary header says EFI PART" \
          "[ \"$(dd if=$T/disk.img bs=512 skip=1 count=1 2>/dev/null | head -c8)\" = 'EFI PART' ]"
    # The judge is deliberately not us.
    if command -v sfdisk >/dev/null 2>&1; then
        check "sfdisk verifies the table without errors" \
              "sfdisk --verify $T/disk.img 2>&1 | grep -q 'No errors detected'"
        check "sfdisk sees exactly one EFI System partition" \
              "[ \"$(sfdisk -l $T/disk.img 2>/dev/null | grep -c 'EFI System')\" = 1 ]"
        # And the backup, which is the half that is never read until the one
        # moment it matters. Zero the primary header and the table must still
        # be there — a backup that merely exists proves nothing.
        cp "$T/disk.img" "$T/damaged.img"
        dd if=/dev/zero of="$T/damaged.img" bs=512 seek=1 count=1 conv=notrunc status=none 2>/dev/null
        check "the table survives losing the primary header" \
              "sfdisk -l $T/damaged.img 2>/dev/null | grep -q 'EFI System'"
    else
        printf '  \033[33mskip\033[0m  sfdisk (not installed)\n'
    fi
    check "vpart refuses a disk too small for a GPT" \
          "dd if=/dev/zero of=$T/tiny.img bs=512 count=100 status=none 2>/dev/null; \
           ! $T/vpart $T/tiny.img --esp 1 >/dev/null 2>&1"
    check "stage 70 partitions with vpart and never calls parted" \
          "grep -q vpart scripts/70-boot-image.sh && ! grep -q '^[^#]*parted' scripts/70-boot-image.sh"
    # Four partitions in the order the installer numbers them, each with the
    # type GUID other systems recognise. Root is sized rather than left to take
    # "the rest", because it is not last once /home or swap exist — a
    # partitioner that gives the rest to the wrong one produces a disk that
    # installs and then has no swap, with nothing reported.
    dd if=/dev/zero of="$T/full.img" bs=1M count=2048 status=none 2>/dev/null
    "$T/vpart" "$T/full.img" --esp 512 --root --home 256 --swap 128 >/dev/null 2>&1
    if command -v sfdisk >/dev/null 2>&1; then
        check "vpart lays out esp, root, home and swap in that order" \
              "sfdisk -l $T/full.img 2>/dev/null | grep -A5 '^Device' | \
               awk 'NR==2{a=\$0} NR==3{b=\$0} NR==4{c=\$0} NR==5{d=\$0} \
                    END{exit !(a ~ /EFI System/ && b ~ /Linux filesystem/ && \
                               c ~ /Linux home/ && d ~ /Linux swap/)}'"
        check "the four-partition table verifies too" \
              "sfdisk --verify $T/full.img 2>&1 | grep -q 'No errors detected'"
    fi
    check "vinstall partitions with vpart and never runs sfdisk" \
          "grep -q 'run(\"vpart' pkg/vinstall/vinstall.c && \
           ! grep -qE 'run\\(\"sfdisk|\"sfdisk\"' pkg/vinstall/vinstall.c"
    check "the build installs vpart into the rootfs" \
          "grep -q 'o /usr/bin/vpart' scripts/40-final-system.sh"
else
    bad "vpart does not compile"
fi

say "the EFI system partition is FAT32, and now by construction"
# Left to choose, dosfstools picked the FAT type from the cluster count and
# chose FAT16 at both sizes VerkOS builds, for a year, while every comment and
# document said FAT32 (D-052). The fix then was to pass -F 32 and check the
# output. The fix now is that dosfstools is gone: vmkfs is FAT32-only and
# refuses anything else rather than choosing, so the question cannot be got
# wrong — a better place to be than remembering a flag.
for s in scripts/70-boot-image.sh scripts/71-iso.sh; do
    check "$(basename "$s") builds its ESP with our own vmkfs" \
        "grep -q VMKFS $s"
    check "$(basename "$s") calls no mkfs.vfat, mmd or mcopy" \
        "! grep -qE 'mkfs[.]vfat|[^a-z]mmd |[^a-z]mcopy ' $s"
done
# Structural greps say what the script asks for; this says what comes out.
if [ -x "$T/vmkfs" ] && command -v file >/dev/null 2>&1; then
    for mb in 48 160; do
        dd if=/dev/zero of="$T/esp$mb.img" bs=1M count=$mb status=none 2>/dev/null
        "$T/vmkfs" -n VERKESP "$T/esp$mb.img" >/dev/null 2>&1
        check "a ${mb} MiB volume from vmkfs really comes out FAT32" \
            "file '$T/esp$mb.img' | grep -q 'FAT (32 bit)'"
    done
fi

say "stam (our own kernel) builds, and is a valid arm64 Image"
# Same trap as vboot: the Makefile builds with $(CROSS)gcc, and finding
# aarch64-linux-gnu-gcc on an x86_64 host means nothing unless CROSS is set to
# match. Without it the host compiler is handed aarch64 assembly.
STAM_CROSS=""
if [ "$(uname -m)" != aarch64 ]; then
    if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
        STAM_CROSS="aarch64-linux-gnu-"
    else
        STAM_CROSS="none"
    fi
fi
if [ -f "pkg/stam/Makefile" ] && [ "$STAM_CROSS" != none ]; then
    if ( cd pkg/stam && make CROSS="$STAM_CROSS" clean >/dev/null 2>&1 && \
         make CROSS="$STAM_CROSS" >/dev/null 2>&1 ); then
        ok "stam compiles"
        IMG=pkg/stam/stam.img
        # The arm64 boot protocol puts "ARM\x64" at offset 56 — which reads as
        # the four characters ARMd, because \x64 is the letter d. Comparing
        # against "ARM" fails on a correct image; the first version of this
        # check did exactly that and reported a defect that was not there.
        if [ "$(dd if="$IMG" bs=1 skip=56 count=4 2>/dev/null)" = "ARMd" ]; then
            ok "stam carries the arm64 Image magic"
        else
            bad "stam is missing the arm64 Image magic at offset 56"
        fi
        # Vectors must be installed before the MMU is switched on: a fault
        # with no vector table is a reset with nothing on the console.
        if grep -n "exceptions_install\|mmu_init" pkg/stam/main.c | head -2 | \
           awk 'NR==1{a=$0} NR==2{b=$0} END{exit !(a ~ /exceptions_install/ && b ~ /mmu_init/)}'; then
            ok "stam installs exception vectors before enabling the MMU"
        else
            bad "stam enables the MMU before installing exception vectors"
        fi
        # No hardcoded machine addresses: the whole point of reading the DTB.
        if grep -qE '0x9000000|0x3f201000|0x1c090000' pkg/stam/*.c 2>/dev/null; then
            bad "stam has a hardcoded UART address — it must come from the device tree"
        else
            ok "stam hardcodes no UART address"
        fi
        # The kernel runs with the MMU off until it has read the device tree,
        # and before the MMU everything is Device memory, where an unaligned
        # access faults instead of being merely slow. GCC 12 merged two adjacent
        # 32-bit fields of fdt_t into one 8-byte `stur x3, [x0, #28]` and the
        # machine died inside fdt_init, before there were vectors to report it:
        # no output at all. -mstrict-align is what stops that.
        #
        # This was first written to scan the disassembly for a `stur` of a
        # 64-bit register at an offset that is not a multiple of 8, on the
        # reasoning that a 64-bit `str` can only encode an aligned offset. The
        # reasoning is right about the instruction and wrong about the check:
        # the displacement is measured from whatever the base register holds,
        # and the compiler routinely points the base into the middle of a
        # struct. `ldur x2, [x1, #44]` in thread_wake_pipe was reported as a
        # defect when x1 was already `&threads[i].state`, making the real
        # offset 64. A test that reports a defect that is not there is worse
        # than no test, because the next person deletes it — D-028, learned
        # again.
        #
        # The flag is what can actually be checked. It has to be in COMMON so
        # that both builds get it, which is the trap CLAUDE.md names: the two
        # have drifted before and a defect existed on one path and not the
        # other.
        check "-mstrict-align is in COMMON, so both builds get it" \
              "awk '/^COMMON/,/[^\\\\]\$/' pkg/stam/Makefile | grep -q -- -mstrict-align"
        # The same kernel must also build as a UEFI application, because that
        # is how our own bootloader starts it.
        if ( cd pkg/stam && make CROSS="$STAM_CROSS" stam.efi >/dev/null 2>&1 ) && [ -f pkg/stam/stam.efi ]; then
            if [ "$(dd if=pkg/stam/stam.efi bs=1 count=2 2>/dev/null)" = "MZ" ]; then
                ok "stam builds as a UEFI application"
            else
                bad "stam.efi is not a PE image (no MZ)"
            fi
            # Nothing applies relocations to this image, so it must not need
            # any. One left behind is a pointer holding a link-time address —
            # the kernel then jumps to where it used to live, which under UEFI
            # is unmapped or non-executable. It presents as an instruction
            # abort at a suspiciously small address.
            if [ -f pkg/stam/stam-efi.so ]; then
                nrel=$(readelf -r pkg/stam/stam-efi.so 2>/dev/null | grep -cE '^0000' || true)
                if [ "${nrel:-0}" -eq 0 ]; then
                    ok "stam.efi needs no relocations"
                else
                    bad "stam.efi has $nrel relocation(s) nothing will apply"
                fi
            fi
        else
            bad "stam does not build as a UEFI application"
        fi

        # --- and then actually start it -------------------------------------
        # Everything above checks the kernel's shape. None of it noticed when
        # the kernel stopped booting entirely (D-053), so this starts it and
        # reads what it printed. The strings asserted on are deliberately ones
        # that only exist if the work behind them happened — the scheduler's
        # own count, a program's report of its arguments, the final milestone
        # line. A kernel that faults early prints a prefix and none of these.
        #
        # It does NOT catch D-053, and it is worth saying so here rather than
        # letting the next person assume it does. That defect was an unaligned
        # access with the MMU off, and whether it faults is the emulator's
        # choice: QEMU 7.2 in the build container runs the offending kernel to
        # completion, QEMU 11.0 on the developer's machine kills it in
        # fdt_init. The check above — no unaligned 64-bit access in the
        # disassembly — is the one that catches that, on every host. This one
        # catches the faults every QEMU agrees are faults.
        if command -v qemu-system-aarch64 >/dev/null 2>&1; then
            if ( cd pkg/stam && make CROSS="$STAM_CROSS" progs >/dev/null 2>&1 ) &&
               [ -f pkg/stam/stam.img ] && [ -f pkg/stam/progs/initrd.cpio ]; then
                BL=$T/stam-boot.log
                ( qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 \
                    -display none -serial "file:$BL" -no-reboot \
                    -kernel pkg/stam/stam.img -initrd pkg/stam/progs/initrd.cpio \
                    </dev/null >/dev/null 2>&1 & echo $! > "$T/qpid" )
                # Long enough for the scheduler demonstration, which spends two
                # seconds proving threads are preempted.
                for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 \
                         21 22 23 24 25 26 27 28 29 30 31 32 33 34 35; do
                    sleep 1
                    grep -aq 'milestone 4' "$BL" 2>/dev/null && break
                done
                kill "$(cat "$T/qpid")" 2>/dev/null
                wait "$(cat "$T/qpid")" 2>/dev/null

                check "stam boots and says which machine it found" \
                      "grep -aq 'stam 0.1 — the VerkOS kernel' $BL"
                check "stam reaches userspace and reports the console it found" \
                      "grep -aqE 'console +(PL011|8250|GENI)' $BL"
                # The timer really preempted: a fixed string would say this
                # whether or not any thread ran (D-033).
                check "stam preempts threads that never yield" \
                      "grep -aq 'every thread ran, evenly. yes.' $BL"
                # argv survived the address-space switch (D-054).
                check "a program stam started can read its own arguments" \
                      "grep -aq 'argv ends with NULL, as it must' $BL"
                check "stam runs a program, collects it, and gets its pages back" \
                      "grep -aq 'memory returned to the free list' $BL"
                # A program hands the kernel eight addresses that are not its
                # own — kernel memory, unmapped memory, past the end of the low
                # half, a length that wraps — and every one must come back
                # -EFAULT. With the check removed the first prints kernel
                # memory to the console and the second is a data abort at EL1,
                # which is the machine stopping rather than a program being
                # killed. The program exits non-zero if any case is not
                # refused, so this is its verdict and not our reading of a log.
                check "stam refuses addresses a program does not own" \
                      "grep -aq 'the kernel refused every address that was not the caller.s' $BL"
                check "stam finishes its own demonstration" \
                      "grep -aq 'milestone 4' $BL"

                # --- the second processor -----------------------------------
                # Its own run, and a short one: this waits for the CPU report
                # and nothing further, so a machine where PSCI behaves
                # differently fails one check with a clear name instead of
                # taking every stam check down with it.
                #
                # "2 online" is printed from a counter the second processor
                # increments itself, after it has configured its own MMU and
                # reached C. A CPU_ON that returns success prints nothing here:
                # for a whole afternoon that was exactly the difference between
                # a call that was accepted and a processor that was running
                # (D-076).
                SM=$T/stam-smp.log
                ( qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 \
                    -smp 2 -display none -serial "file:$SM" -no-reboot \
                    -kernel pkg/stam/stam.img -initrd pkg/stam/progs/initrd.cpio \
                    </dev/null >/dev/null 2>&1 & echo $! > "$T/qpid" )
                for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do
                    sleep 1
                    grep -aq 'cpus      2 online' "$SM" 2>/dev/null && break
                    grep -aq 'never arrived' "$SM" 2>/dev/null && break
                done
                kill "$(cat "$T/qpid")" 2>/dev/null
                wait "$(cat "$T/qpid")" 2>/dev/null

                check "stam starts the second processor and it arrives" \
                      "grep -aq 'cpus      2 online' $SM"
                # Written by the secondary from its own MPIDR, so it also says
                # the core knows which one it is.
                check "the second processor reports its own identity" \
                      "grep -aq 'cpu 1      online, mpidr' $SM"

                # --- the shell, from a script -------------------------------
                # No fifo, no sleeps between commands, no interactive session to
                # synchronise with. vsh opens /rc from the initrd onto its own
                # standard input and runs it, so the machine does the whole
                # sequence by itself and this only has to read what it printed.
                #
                # That replaces a harness which was intermittent five separate
                # ways — a fifo closed between writes, waiting on output instead
                # of readiness, counting lines instead of matches, an argument
                # order that sent assertions as commands, and one cause never
                # found (D-062). Every one of them looked like timing. The fix
                # was to remove the timing.
                if [ -f pkg/stam/progs/initrd-script.cpio ]; then
                    SL=$T/script.log
                    ( qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 512 \
                        -display none -serial "file:$SL" -no-reboot \
                        -kernel pkg/stam/stam.img \
                        -initrd pkg/stam/progs/initrd-script.cpio \
                        </dev/null >/dev/null 2>&1 & echo $! > "$T/spid" )
                    # The script ends with `exit`, so the shell says bye. Wait
                    # for that rather than for a fixed time.
                    for _ in $(seq 1 90); do
                        sleep 1
                        grep -aq 'bye' "$SL" 2>/dev/null && break
                    done
                    kill "$(cat "$T/spid")" 2>/dev/null
                    wait "$(cat "$T/spid")" 2>/dev/null

                    check "the shell runs a script and reaches its end" \
                          "grep -aq 'bye' $SL"
                    check "a whole line of arguments reaches the program" \
                          "grep -aq 'argv\[3\] = \"three\"' $SL"
                    # `upper` reads standard input and writes it back in
                    # capitals, knowing nothing about where either is attached.
                    # Capitals are the descriptor having moved, not the program
                    # behaving differently — which is why the unpiped case is
                    # checked too.
                    check "a | b sends one program's output to the other" \
                          "grep -aq 'ARGV\[1\] = \"WORLD\"' $SL"
                    check "the same program unpiped still writes to the console" \
                          "grep -aq 'argv\[1\] = \"one\"' $SL"
                    # Three segments, because a chain is the same calls in a
                    # loop and only a third stage shows the loop is right.
                    check "a | b | c chains all three" \
                          "grep -aq 'ARGV\[1\] = \"CHAIN\"' $SL"
                    # Written to a file and read back by a different program.
                    # `> f` alone would pass on a shell that discarded output.
                    check "a > f keeps the output off the console" \
                          "! grep -aq 'argv\[1\] = \"redir\"' $SL"
                    check "a < f feeds a program what the file holds" \
                          "grep -aq 'ARGV\[1\] = \"REDIR\"' $SL"
                    # >> must keep what > wrote: reading the file back shows
                    # both runs, or append silently truncated.
                    check "a >> f appends instead of truncating" \
                          "grep -aq 'ARGV\[1\] = \"FIRST\"' $SL && grep -aq 'ARGV\[1\] = \"SECOND\"' $SL"
                    # & returned the prompt without waiting, and the job was
                    # collected at a later one — a background job nobody
                    # collects is a zombie holding a process slot.
                    check "a & runs in the background and is collected later" \
                          "grep -aq 'done ' $SL"
                    # A signal reaching a process that never calls into the
                    # kernel is the case Ctrl-C is for: a loop that counts has
                    # no system call to be caught on the way out of, so
                    # delivery has to happen when the timer interrupts it. The
                    # program checks the exit status rather than the absence of
                    # output, because a child that never started and a child
                    # that was killed both stop printing.
                    check "a signal ends a process that is not asking for anything" \
                          "grep -aq 'a signal reached a running process and ended it' $SL"
                    check "the status says which signal it died of" \
                          "grep -aq 'exited with 130' $SL"
                fi
            else
                bad "stam's initrd does not build, so the kernel cannot be booted"
            fi
        else
            printf '  \033[33mskip\033[0m  booting stam (no qemu-system-aarch64)\n'
        fi
        ( cd pkg/stam && make CROSS="$STAM_CROSS" clean clean-efi >/dev/null 2>&1 )
    else
        bad "stam does not compile"
    fi
else
    printf '  \033[33mskip\033[0m  stam (no aarch64 toolchain here)\n'
fi

# --- and stam boots the same way ------------------------------------------
# synthdtb is off by default (D-072), and stam is the one kernel that needs it
# on — so this path had exactly the shape the log keeps finding: something that
# looks fine because nobody asked. It was broken when first asked. The config
# keyword parsed only when followed by a space, and a keyword with no value is
# followed by a newline, so `synthdtb` on a line of its own did nothing at all.
#
# The disk is built here with our own tools, which makes this the whole chain
# in one check: vpart, vmkfs, vboot, the synthetic tree, and stam.
say "stam boots through vboot, on a disk built by our own tools"
SFW=""
for c in /usr/share/AAVMF/AAVMF_CODE.fd /usr/share/qemu-efi-aarch64/QEMU_EFI.fd \
         /usr/share/edk2/aarch64/QEMU_EFI.fd; do
    [ -f "$c" ] && { SFW="$c"; break; }
done
# STAM_CROSS is empty on an aarch64 host, which is correct and not "missing" —
# requiring it to be non-empty skipped this check on the machine that builds
# the kernel. "none" is the value that means there is no toolchain.
if [ "$STAM_CROSS" != none ] && [ -n "$SFW" ] && \
   [ -x "$T/vmkfs" ] && [ -x "$T/vpart" ] && command -v qemu-system-aarch64 >/dev/null 2>&1; then
    S=$T/stamboot; mkdir -p "$S"
    if ( cd pkg/stam && make CROSS="$STAM_CROSS" stam.efi progs >/dev/null 2>&1 ) &&
       CC="${STAM_CROSS}gcc" OBJCOPY="${STAM_CROSS}objcopy" LD="${STAM_CROSS}ld" \
       bash pkg/vboot/build.sh "$S/vb" aarch64 >/dev/null 2>&1; then
        printf 'timeout 0\ndefault 0\n\nentry stam\n  kernel  \\stam.efi\n  initrd  \\initramfs.img\n  cmdline stam.hello\n  synthdtb\n' > "$S/vboot.conf"
        dd if=/dev/zero of="$S/esp.img" bs=1M count=64 status=none 2>/dev/null
        "$T/vmkfs" -n VERKESP "$S/esp.img"                                  >/dev/null 2>&1
        "$T/vmkfs" --mkdir "$S/esp.img" /EFI                                >/dev/null 2>&1
        "$T/vmkfs" --mkdir "$S/esp.img" /EFI/BOOT                           >/dev/null 2>&1
        "$T/vmkfs" --copy "$S/esp.img" "$S/vb/BOOTAA64.EFI" /EFI/BOOT/BOOTAA64.EFI >/dev/null 2>&1
        "$T/vmkfs" --copy "$S/esp.img" pkg/stam/stam.efi          /stam.efi       >/dev/null 2>&1
        "$T/vmkfs" --copy "$S/esp.img" pkg/stam/progs/initrd.cpio /initramfs.img  >/dev/null 2>&1
        "$T/vmkfs" --copy "$S/esp.img" "$S/vboot.conf"            /vboot.conf     >/dev/null 2>&1
        dd if=/dev/zero of="$S/disk.img" bs=1M count=68 status=none 2>/dev/null
        "$T/vpart" "$S/disk.img" --esp 64 >/dev/null 2>&1
        dd if="$S/esp.img" of="$S/disk.img" bs=1M seek=1 conv=notrunc status=none 2>/dev/null

        dd if=/dev/zero of="$S/code.fd" bs=1M count=64 status=none 2>/dev/null
        dd if="$SFW" of="$S/code.fd" conv=notrunc status=none 2>/dev/null
        dd if=/dev/zero of="$S/vars.fd" bs=1M count=64 status=none 2>/dev/null
        ( qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 1024 \
            -display none -no-reboot \
            -drive if=pflash,format=raw,file="$S/code.fd",readonly=on \
            -drive if=pflash,format=raw,file="$S/vars.fd" \
            -drive if=none,id=hd,file="$S/disk.img",format=raw,snapshot=on \
            -device virtio-blk-pci,drive=hd,bootindex=0 \
            -serial "file:$S/log" </dev/null >/dev/null 2>&1 & echo $! > "$S/pid" )
        for _ in $(seq 1 90); do
            sleep 1
            grep -aqE 'milestone|Refusing to continue|halted' "$S/log" 2>/dev/null && break
        done
        kill "$(cat "$S/pid")" 2>/dev/null
        wait "$(cat "$S/pid")" 2>/dev/null

        check "vboot starts stam as an EFI application" \
              "grep -aq 'started as an EFI application' $S/log"
        # The keyword that did nothing. Without it stam refuses and says so,
        # which is the correct behaviour and the wrong outcome.
        check "synthdtb reaches vboot and a tree is installed" \
              "grep -aq 'device tree found' $S/log && ! grep -aq 'Refusing to continue' $S/log"
        check "stam finds its console through that tree" \
              "grep -aqE 'console +(PL011|8250)' $S/log"
        check "stam finds the interrupt controller and the timer" \
              "grep -aq 'gic *v' $S/log && grep -aq 'timer' $S/log"
        # A fixed string would say this whether or not any interrupt arrived.
        check "stam takes interrupts on the tree vboot built" \
              "grep -aq 'the kernel can preempt' $S/log"
        check "the initrd vboot published reaches stam" \
              "grep -aq 'initrd collected from the loader' $S/log"
    else
        bad "stam.efi or vboot did not build for the boot test"
    fi
else
    printf '  \033[33mskip\033[0m  booting stam through vboot (needs a cross toolchain, qemu and UEFI firmware)\n'
fi

# --- the device tree we write must be the device tree we read ---------------
# vboot builds a tree (fdtgen.h) and stam parses one (pkg/stam/fdt.c). They are
# written independently, they never meet on the build host, and a disagreement
# about byte order or a block offset shows up only as a kernel that boots on
# some machines. Build a tree with one and read it back with the other.
say "the tree vboot writes is the tree stam reads"
cat > "$T/fdt_roundtrip.c" <<'RT'
#include <stdio.h>
#include <string.h>
typedef unsigned char UINT8; typedef unsigned int UINT32;
typedef unsigned long long UINT64; typedef unsigned long UINTN;
#include "../pkg/vboot/fdtgen.h"
#include "../pkg/stam/fdt.h"
static unsigned char blob[8192];
int main(void) {
    fdtgen_t g; fg_init(&g, blob, sizeof blob);
    fg_node(&g, "");
      fg_prop_u32(&g, "#address-cells", 2);
      fg_prop_u32(&g, "#size-cells", 2);
      fg_prop_str(&g, "model", "roundtrip");
      fg_node(&g, "chosen");
        fg_prop_str(&g, "stdout-path", "/serial@9000000");
      fg_end_node(&g);
      fg_node(&g, "memory@0");
        fg_prop_str(&g, "device_type", "memory");
        fg_prop_reg(&g, 0x40000000ULL, 0x40000000ULL);
      fg_end_node(&g);
      fg_node(&g, "serial@9000000");
        fg_prop_str(&g, "compatible", "arm,pl011");
        fg_prop_reg(&g, 0x9000000ULL, 0x1000ULL);
      fg_end_node(&g);
    fg_end_node(&g);
    if (!fg_finish(&g)) { puts("BUILD-FAILED"); return 1; }

    fdt_t f;
    if (fdt_init(&f, blob)) { puts("PARSE-FAILED"); return 1; }
    if (f.addr_cells != 2 || f.size_cells != 2) { puts("CELLS-WRONG"); return 1; }
    unsigned len = 0;
    const char *m = fdt_getprop(&f, fdt_root(&f), "model", &len);
    if (!m || strcmp(m, "roundtrip")) { puts("MODEL-WRONG"); return 1; }
    int ch = fdt_find_path(&f, "/chosen");
    const char *sp = ch == FDT_NONE ? 0 : fdt_getprop(&f, ch, "stdout-path", &len);
    if (!sp || strcmp(sp, "/serial@9000000")) { puts("CHOSEN-WRONG"); return 1; }
    int u = fdt_find_compatible(&f, "arm,pl011");
    unsigned long long a = 0, sz = 0;
    if (u == FDT_NONE || fdt_reg(&f, u, &a, &sz) || a != 0x9000000ULL || sz != 0x1000ULL) {
        puts("UART-WRONG"); return 1; }
    int depth = 0, mem = FDT_NONE;
    for (int n = fdt_root(&f); n != FDT_NONE; n = fdt_next_node(&f, n, &depth)) {
        const char *dt = fdt_getprop(&f, n, "device_type", &len);
        if (dt && !strcmp(dt, "memory")) { mem = n; break; }
    }
    if (mem == FDT_NONE || fdt_reg(&f, mem, &a, &sz) || a != 0x40000000ULL || sz != 0x40000000ULL) {
        puts("MEMORY-WRONG"); return 1; }
    puts("OK");
    return 0;
}
RT
if cc -O1 -I scripts -o "$T/fdt_roundtrip" "$T/fdt_roundtrip.c" pkg/stam/fdt.c pkg/stam/klib.c \
       -DNDEBUG 2>/dev/null; then
    out=$("$T/fdt_roundtrip" 2>&1)
    case "$out" in
        OK) ok "a tree built by vboot parses correctly in stam" ;;
        *)  bad "vboot's tree and stam's parser disagree: $out" ;;
    esac
else
    bad "the fdt round-trip test does not compile"
fi

# --- the Android boot image we write must be one we can read ---------------
say "vbootimg writes an image its reader agrees with"
if cc -O1 -o "$T/vbootimg" pkg/vbootimg/vbootimg.c 2>/dev/null; then
    ok "vbootimg compiles"
    head -c 40000 /dev/urandom > "$T/fakekernel"
    head -c  9000 /dev/urandom > "$T/fakeramdisk"
    if "$T/vbootimg" create -o "$T/boot.img" --kernel "$T/fakekernel" \
           --ramdisk "$T/fakeramdisk" --cmdline "hello=1" >/dev/null 2>&1; then
        ok "vbootimg writes an image"
    else
        bad "vbootimg could not write an image"
    fi
    out=$("$T/vbootimg" show "$T/boot.img" 2>&1)
    case "$out" in *"40000 bytes"*) ok "the kernel size survives the round trip" ;;
                   *) bad "kernel size wrong: $out" ;; esac
    case "$out" in *"9000 bytes"*)  ok "the ramdisk size survives the round trip" ;;
                   *) bad "ramdisk size wrong" ;; esac
    case "$out" in *"hello=1"*)     ok "the command line survives the round trip" ;;
                   *) bad "command line lost" ;; esac
    # The header must be the first thing a bootloader checks, so we must write it.
    if [ "$(dd if="$T/boot.img" bs=1 count=8 2>/dev/null)" = "ANDROID!" ]; then
        ok "the image starts with the magic a bootloader looks for"
    else
        bad "no ANDROID! magic at the start of the image"
    fi
    # A truncated image must be refused, not half-read.
    head -c 5000 "$T/boot.img" > "$T/short.img"
    if "$T/vbootimg" show "$T/short.img" >/dev/null 2>&1; then
        bad "vbootimg accepted an image shorter than its own header claims"
    else
        ok "vbootimg refuses a truncated image"
    fi
    if "$T/vbootimg" show "$T/fakekernel" >/dev/null 2>&1; then
        bad "vbootimg accepted something that is not a boot image"
    else
        ok "vbootimg refuses a file that is not a boot image"
    fi
else
    bad "vbootimg does not compile"
fi

# --- the setuid set is exactly two files ------------------------------------
# A setuid binary hands its caller privileges they did not have. The list of
# them is therefore the shortest and most important inventory in the system,
# and it must be checked rather than remembered.
say "only su and passwd may be setuid"
# One place in the build may set the bit, and it must be the loop over su and
# passwd. Counting `chmod 4xxx` lines is the whole check; earlier attempts
# matched ordinary modes and reported four places where there is one.
setters=$(grep -c "chmod 4[0-7][0-7][0-7]" scripts/40-final-system.sh || true)
if [ "${setters:-0}" -eq 1 ]; then
    ok "the build sets the setuid bit in exactly one place"
else
    bad "the setuid bit is set in ${setters} places in the build; it belongs in one"
fi

if grep -q 'cp /usr/bin/vlogin "/usr/bin/\$t"' scripts/40-final-system.sh; then
    bad "su/passwd are copies of the multi-call binary again — see D-051"
else
    ok "su and passwd are not copies of the multi-call binary"
fi

# The rule from D-051, as something a machine can check: a setuid program has
# one main() and cannot become anything else. A program that compares strings
# to decide what to do is dispatching, whatever it calls the variable.
for f in su passwd; do
    if [ ! -f "pkg/vlogin/$f.c" ]; then
        bad "$f is not its own program"
    elif ! grep -q "int main" "pkg/vlogin/$f.c"; then
        bad "$f has no main of its own"
    elif grep -q "strcmp" "pkg/vlogin/$f.c"; then
        bad "$f compares strings to choose behaviour — that is dispatch"
    else
        ok "$f has one main and cannot become another tool"
    fi
done

# Hardening is part of the contract for these two, not a preference.
for flag in _FORTIFY_SOURCE=3 -fstack-protector-strong -fPIE relro; do
    if grep -q -- "$flag" scripts/40-final-system.sh; then
        ok "the account tools are built with $flag"
    else
        bad "the account tools are not built with $flag"
    fi
done

# If a rootfs exists, check the real thing rather than the recipe for it.
if [ -d "${ROOTFS:-work/aarch64/rootfs}" ]; then
    rfs="${ROOTFS:-work/aarch64/rootfs}"
    actual=$(find "$rfs" -xdev -perm -4000 -type f 2>/dev/null |
             sed "s|^$rfs||" | sort | tr '\n' ' ')
    case "$actual" in
        ""|"/usr/bin/passwd /usr/bin/su "|"/usr/bin/su /usr/bin/passwd ")
            ok "the built rootfs has no unexpected setuid binaries" ;;
        *)  bad "unexpected setuid binaries in the rootfs: $actual" ;;
    esac
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
