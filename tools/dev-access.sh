#!/usr/bin/env bash
# dev-access.sh — put an SSH key into a built rootfs so a test VM can be driven
# from the outside.
#
# THIS IS NOT PART OF THE BUILD, ON PURPOSE. An image that ships someone's
# public key lets that someone in, which is a backdoor no matter how good the
# intentions. Run this by hand when you want to poke at a VM, and never before
# building an image you intend to give to anyone.
#
#   tools/dev-access.sh [ARCH]        default ARCH: aarch64
#
# It generates the key on first use and leaves a marker at /etc/verkos-dev-access
# in the rootfs, so an image built with a key in it can be recognised later.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ARCH="${1:-aarch64}"
ROOTFS="${WORK:-$HERE/../work}/$ARCH/rootfs"
KEY="${VERK_DEV_KEY:-/tmp/verkos_key}"

[ -d "$ROOTFS" ] || { echo "no rootfs at $ROOTFS — build it first" >&2; exit 1; }

if [ ! -f "$KEY" ]; then
    ssh-keygen -t ed25519 -N '' -C "verkos-dev" -f "$KEY" >/dev/null
    echo ":: generated a development key at $KEY"
elif [ ! -f "$KEY.pub" ]; then
    # The private key alone is enough; the public half is derivable from it.
    ssh-keygen -y -f "$KEY" > "$KEY.pub"
    echo ":: recovered the public half of $KEY"
fi

install -d -m 700 "$ROOTFS/root/.ssh"
install -m 600 "$KEY.pub" "$ROOTFS/root/.ssh/authorized_keys"
# Key auth only: the dev root password is well known, so letting it in over the
# network would be worse than the key.
if ! grep -q '^PermitRootLogin' "$ROOTFS/etc/ssh/sshd_config" 2>/dev/null; then
    printf 'PermitRootLogin prohibit-password\n' >> "$ROOTFS/etc/ssh/sshd_config"
fi
date -u +"installed %Y-%m-%dT%H:%M:%SZ" > "$ROOTFS/etc/verkos-dev-access"

cat <<EOF

!! $ROOTFS now trusts $KEY.pub for root over SSH.
!! Any image built from it does too. Rebuild the rootfs before shipping one.

Connect with: ssh -i $KEY -p 2222 root@localhost
EOF
