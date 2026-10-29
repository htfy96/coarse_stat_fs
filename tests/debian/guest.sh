#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# init of the Debian test guest.
export PATH=/usr/sbin:/usr/bin:/sbin:/bin HOME=/root
# The initramfs has usually moved these into place already.
mount -t proc proc /proc 2>/dev/null
mount -t sysfs sys /sys 2>/dev/null
mount -t devtmpfs dev /dev 2>/dev/null
mount -t tmpfs tmp /tmp
mount -o remount,rw /

poweroff() {
	sync
	echo o >/proc/sysrq-trigger
	sleep 10
}

cd /opt/coarse_stat_fs || poweroff
# shellcheck source=/dev/null
. /etc/os-release
echo "coarse_stat_fs-ci: kernel $(uname -r), $PRETTY_NAME, $(gcc --version | head -1)"
if make -s -C kmod && make -s -C tests && make -s -C tools csfcat ../build/eventloop; then
	# shellcheck disable=SC2046
	CSF_TEE=1 CSF_DISK=/dev/vdb bash tests/vm-inner.sh $(cat /opt/coarse_stat_fs-tests)
else
	echo "RESULT build-failed"
fi
poweroff
