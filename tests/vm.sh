#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -eu
R=$(cd "$(dirname "$0")/.." && pwd)
OUT=$R/build/vm
mkdir -p "$OUT"
make -s -C "$R/kmod"
make -s -C "$R/tests"
make -s -C "$R/tools"
rm -f "$OUT/result" "$OUT/tests.log"
DEBUG="slub_debug=FZPU init_on_free=1 kfence.sample_interval=1"
[ "${VM_DEBUG:-1}" = 0 ] && DEBUG=
[ -f "$OUT/disk.img" ] || dd if=/dev/urandom of="$OUT/disk.img" bs=1M count=64 status=none

timeout "${VM_TIMEOUT:-600}" vng -r --verbose --user root --disable-microvm \
	--cpus 4 --memory 2G --rwdir "$OUT" --disk "$OUT/disk.img" \
	--qemu-opts="-cpu host,topoext=on,+invtsc" \
	--append "panic_on_warn=1 softlockup_panic=1 hung_task_panic=1 hung_task_timeout_secs=30 panic=-1 $DEBUG" \
	--exec "$R/tests/vm-inner.sh ${*:-cpu thread life stress tools}" \
	</dev/null >"$OUT/console.log" 2>&1 || true

[ -f "$OUT/tests.log" ] && cat "$OUT/tests.log"
if [ ! -f "$OUT/result" ]; then
	echo "!! no result: guest died or hung; console tail:"
	grep -E 'BUG|WARNING|Oops|panic|RIP|Call Trace|coarse_stat_fs|^\s+\S+\+0x' "$OUT/console.log" | tail -60
	exit 1
fi
grep -q 'RESULT 0' "$OUT/result"
