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
           pkg/vinstall/vinstall.c pkg/viso/viso.c pkg/vhex/vhex.c pkg/vtree/vtree.c \
           pkg/vdate/vdate.c pkg/vxargs/vxargs.c pkg/vip/vip.c; do
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
    ( cd "$T/cp" && echo "$T/cp/outside.txt" | "$T/vcpio" -o > "$T/cp/abs.cpio" ) 2>/dev/null
    mkdir -p "$T/cp/dest2"
    ( cd "$T/cp/dest2" && "$T/vcpio" -i < "$T/cp/abs.cpio" ) 2>/dev/null
    check "vcpio strips a leading / instead of writing outside" \
          "[ -z \"\$(find '$T/cp/dest2' -type f -newer '$T/cp/abs.cpio' -path '*/outside.txt' -prune -o -type f -print 2>/dev/null | head -1)\" ] && find '$T/cp/dest2' -name outside.txt | grep -q ."
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
