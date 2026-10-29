#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
set -u
R=$(cd "$(dirname "$0")/.." && pwd)
OUT=$R/build/vm
B=$R/build/tests
MNT=/sys/kernel/coarse_stat_fs
mkdir -p "$OUT"
# A dying Debian guest takes its log file with it, so it sets CSF_TEE.
if [ -n "${CSF_TEE:-}" ]; then
	exec > >(tee "$OUT/tests.log") 2>&1
else
	exec >"$OUT/tests.log" 2>&1
fi
fail=0

step() { echo; echo "== $*"; }
run() { echo "\$ $*"; "$@" || { echo "!! failed: $*"; fail=1; }; }
klog_bad() { dmesg | grep -E 'WARNING:|BUG:|Oops|Call Trace|refcount_t|coarse_stat_fs: ' ; }

dmesg -C
step load
run insmod "$R/kmod/coarse_stat_fs.ko"
run mount -t coarse_stat_fs none $MNT
ls -l $MNT

# Scratch disk for the O_DIRECT test.
disk=${CSF_DISK:-}
if [ -z "$disk" ]; then
	for d in vda sda; do [ -b /dev/$d ] && { disk=/dev/$d; break; }; done
fi

want=${*:-all}
has() { [[ $want == all || " $want " == *" $1 "* ]]; }

has cpu    && { step t_cpu;    run "$B/t_cpu"; }
has thread && { step t_thread; run "$B/t_thread" ${disk:+"$disk"}; }
has life   && { step t_life;   run "$B/t_life"; }
has stress && { step t_stress; run "$B/t_stress" "${STRESS_SECS:-20}"; }
has bench  && { step bench;    run "$B/bench"; }
if has tools; then
	step tools
	run "$R/tools/csfcat" cpu
	run "$R/tools/csfcat" self 100
	run "$R/build/eventloop"
	"$R/build/eventloop" | grep -q 'slow op spin' || { echo "!! eventloop missed the spin"; fail=1; }
	"$R/build/eventloop" | grep -q 'slow op sleep' || { echo "!! eventloop missed the sleep"; fail=1; }
	[ "$("$R/build/eventloop" | grep -c 'slow op fast')" = 0 ] || { echo "!! fast op reported"; fail=1; }
fi

step "rmmod refused while a thread object is alive"
"$B/t_hold" > "$OUT/hold.out" &
hold=$!
for _ in $(seq 100); do grep -q ready "$OUT/hold.out" 2>/dev/null && break; sleep 0.05; done
run umount -l $MNT
if rmmod coarse_stat_fs 2>/dev/null; then echo "!! rmmod succeeded with a live object"; fail=1; fi
kill $hold; wait $hold 2>/dev/null
ok=0
for _ in $(seq 100); do rmmod coarse_stat_fs 2>/dev/null && { ok=1; break; }; sleep 0.05; done
if [ $ok = 1 ]; then
	echo "rmmod ok"
else
	echo "!! rmmod failed after holder exit"
	fail=1
fi

step "kernel log"
klog_bad
if dmesg | grep -qE 'WARNING:|BUG:|Oops|Call Trace|refcount_t'; then fail=1; fi

echo "RESULT $fail" | tee "$OUT/result"
