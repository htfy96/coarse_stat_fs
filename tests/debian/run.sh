#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
suite=${1:?usage: $0 SUITE [tests...]}
shift
tests=${*:-cpu thread life stress tools}
base=${suite%-backports}
mirror=${DEBIAN_MIRROR:-http://deb.debian.org/debian}
out=$R/build/debian/$suite
mkdir -p "$out"
SUDO=
[ "$(id -u)" = 0 ] || SUDO=sudo

work=$(mktemp -d)
cleanup() { $SUDO rm -rf --one-file-system "${work:?}"; }
trap cleanup EXIT
root=$work/root

kpkgs="linux-image-amd64,linux-headers-amd64"
sources=("deb $mirror $base main")
if [ "$base" != "$suite" ]; then
	kpkgs="linux-image-amd64/$suite,linux-headers-amd64/$suite"
	sources+=("deb $mirror $suite main")
fi

# The host's debian-archive-keyring (e.g. Ubuntu's) can lag behind the keys
# that sign the archive, so use the newest one from the archive, over HTTPS.
echo "== fetching debian-archive-keyring"
pool=https://deb.debian.org/debian/pool/main/d/debian-archive-keyring
deb=$(curl -sfL "$pool/" | grep -oE 'debian-archive-keyring_[^"]+_all\.deb' | sort -uV | tail -1)
curl -sfL -o "$work/keyring.deb" "$pool/$deb"
dpkg-deb -x "$work/keyring.deb" "$work/keyring"
keyring=
for k in "$work"/keyring/usr/share/keyrings/debian-archive-keyring.{pgp,gpg}; do
	[ -e "$k" ] && { keyring=$k; break; }
done
[ -n "$keyring" ] || { echo "!! no keyring in $deb"; exit 1; }
echo "$deb: $keyring"

echo "== building $suite root filesystem"
copy="tar -C '$R' -c --exclude=./build --exclude=./.git --exclude=./.cache"
for x in '*.o' '*.ko' '*.mod' '*.mod.c' '.*.cmd' Module.symvers modules.order \
	compile_commands.json ./tools/csfcat; do
	copy+=" --exclude='$x'"
done
$SUDO mmdebstrap --mode=root --variant=apt --format=directory --keyring="$keyring" \
	--include="build-essential,kmod,mount,$kpkgs" \
	--customize-hook="mkdir -p \"\$1/opt/coarse_stat_fs\" && $copy . | tar -C \"\$1/opt/coarse_stat_fs\" -x" \
	--customize-hook="echo '$tests' > \"\$1/opt/coarse_stat_fs-tests\"" \
	"$base" "$root" "${sources[@]}"

kver=
for d in "$root"/lib/modules/*/; do kver=$(basename "$d"); done
echo "== kernel $kver"
echo "$kver" >"$out/kver"
$SUDO cp "$root/boot/vmlinuz-$kver" "$root/boot/initrd.img-$kver" "$work/"
$SUDO chmod a+r "$work/vmlinuz-$kver" "$work/initrd.img-$kver"
$SUDO mke2fs -q -t ext4 -L csf-root -d "$root" "$work/root.img" 6G
$SUDO chown "$(id -u)" "$work/root.img"
dd if=/dev/urandom of="$work/scratch.img" bs=1M count=64 status=none

accel=(-accel kvm -cpu "host,+invtsc")
if [ ! -w /dev/kvm ]; then
	echo "!! /dev/kvm not usable; falling back to TCG (slow; timing tests may fail)"
	accel=(-accel tcg -cpu max)
fi
DEBUG="slub_debug=FZPU init_on_free=1 kfence.sample_interval=1"
[ "${VM_DEBUG:-1}" = 0 ] && DEBUG=

echo "== booting $kver"
set +e
timeout "${VM_TIMEOUT:-1200}" qemu-system-x86_64 "${accel[@]}" -smp 4 -m 3G \
	-kernel "$work/vmlinuz-$kver" -initrd "$work/initrd.img-$kver" \
	-drive "file=$work/root.img,if=virtio,format=raw" \
	-drive "file=$work/scratch.img,if=virtio,format=raw" \
	-append "root=/dev/vda rw init=/opt/coarse_stat_fs/tests/debian/guest.sh console=ttyS0 loglevel=4 panic_on_warn=1 softlockup_panic=1 hung_task_panic=1 hung_task_timeout_secs=60 panic=-1 $DEBUG" \
	-nographic -no-reboot </dev/null 2>&1 | tee "$out/console.log"
set -e
grep -aq '^RESULT 0' "$out/console.log"
