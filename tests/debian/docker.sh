#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# run.sh inside an Ubuntu container, for hosts without mmdebstrap.
set -euo pipefail
R=$(cd "$(dirname "$0")/../.." && pwd)
suite=${1:?usage: $0 SUITE [tests...]}
out=$R/build/debian/$suite
mkdir -p "$out"
kvm=()
[ -e /dev/kvm ] && kvm=(--device /dev/kvm)
# CAP_SYS_ADMIN: mmdebstrap mounts /proc and /dev inside its chroot.
exec docker run --rm "${kvm[@]}" --cap-add SYS_ADMIN \
	-v "$R:/src:ro" -v "$out:/out" -e HOST_UID="$(id -u)" \
	-e VM_DEBUG="${VM_DEBUG:-1}" -e VM_TIMEOUT="${VM_TIMEOUT:-1200}" \
	ubuntu:24.04 bash -c '
		set -e
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq >/dev/null
		apt-get install -y -qq --no-install-recommends mmdebstrap qemu-system-x86 \
			e2fsprogs curl ca-certificates >/dev/null
		cp -a /src /work
		rc=0
		/work/tests/debian/run.sh "$@" || rc=$?
		cp -r /work/build/debian/"$1"/. /out/
		chown -R "$HOST_UID" /out
		exit $rc' docker.sh "$@"
