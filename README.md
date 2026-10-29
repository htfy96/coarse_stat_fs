# coarse_stat_fs

coarse_stat_fs is a Linux kernel module that exposes coarse versions of
`/proc/self/schedstat` and `/proc/stat` as read-only shared memory pages.
After a one-time `open` and `mmap`, a snapshot costs about as much as
`rdtscp` and makes no syscall.

One use is profiling a hot loop and reporting only the operations that are
especially slow. Take a snapshot before each operation and time the
operation with `rdtscp`. When an operation is slow, take a second snapshot
and print where the wall time went:

```c
#include "lib/coarse_stat_fs.h"

struct csf_snap before, after;
char buf[CSF_DIFF_BUFSZ]; /* fits any csf_format_diff() output */

csf_thread_snap(&before); /* ~16 ns, no syscall */
uint64_t t0 = csf_rdtsc();
op();
if (csf_rdtsc() - t0 > slow) {
	csf_thread_snap(&after);
	csf_format_diff(buf, sizeof(buf), &before, &after);
	puts(buf);
}
```

Output for an operation that slept for 25 ms:

```
wall 25.119ms = cpu 0.017 + runq 0.000 + sleep 25.102 + dsleep 0.000 + iowait 0.000 | preempt 0 block 1 migrate 0 | flt 4/0 | sysc r0 w0 | io r0B w0B
```

`examples/eventloop.c` is a complete program, and it also reads the fields
one at a time; see [Reading a snapshot](#reading-a-snapshot).

"Coarse" is about freshness. The time buckets are exact to the cycle, but
the other fields are only as fresh as the thread's last context switch or
the cpu page's last refresh:

| Data | Resolution | Up to date as of | Why |
|------|------------|------------------|-----|
| Thread time buckets (`*_cyc`) and `s.tsc` | 1 TSC cycle (0.5 ns at 2 GHz) | the snapshot (`rdtscp`) | The kernel adds to them at every switch, and the reader extrapolates the current bucket to `s.tsc`. |
| Switch counters (`nr_*`), `state`, `cpu` | exact counts | the last context switch | The switch hooks write them. |
| `sum_exec_ns`, `run_delay_ns`, `pcount`, `nvcsw`, `nivcsw`, faults, I/O counters | exact (ns, counts, bytes) | the last switch-out | The kernel copies them from the task only when it leaves the CPU. |
| `utime_ns`, `stime_ns` | one scheduler tick (1 ms at `CONFIG_HZ=1000`, 4 ms at 250) | the last switch-out | The kernel samples them per tick, except on `nohz_full` CPUs. |
| CPU page rows | `period_us` refresh (10 ms default, 1 ms minimum); `user`, `system`, `irq` and `softirq` also in ticks | the last refresh: `update_mono_ns` | Delayed work copies the per-CPU counters, and the kernel accounts most of those per tick. |

For comparison, `/proc/stat` and the `utime` and `stime` fields of
`/proc/<tid>/stat` count in USER_HZ ticks of 10 ms.

## Contents

- [Getting started](#getting-started)
- [Repository layout](#repository-layout)
- [Developing](#developing)
- [Testing](#testing)
- [Continuous integration](#continuous-integration)
- [Interface](#interface)
- [Cost](#cost)
- [Installing](#installing)
- [Caveats](#caveats)
- [License](#license)

## Getting started

### Prerequisites

- An x86-64 kernel with `CONFIG_PREEMPT_NOTIFIERS` and `CONFIG_SCHED_INFO`,
  version 6.1 or later. CI tests 6.1, 6.12 and 7.2. Most distribution
  kernels qualify: KVM selects preempt notifiers, and schedstats select
  `SCHED_INFO`.
- The headers for that kernel, for example `linux-headers` on Arch or
  `linux-headers-amd64` on Debian.
- gcc and make. Testing also needs QEMU and
  [virtme-ng](https://github.com/arighi/virtme-ng). Linting needs clang-format,
  clang-tidy, bear and, optionally, shellcheck.

### Building and trying it

1. Build the module, the tools, the example and the tests:

   ```sh
   make
   ```

2. Run the tests in a throwaway VM before you load the module on your
   machine. A bug in a scheduler hook can crash the kernel that runs it, so
   the VM boots your kernel and runs the module there instead:

   ```sh
   make check
   ```

   The run ends with `RESULT 0` when every test passes.

3. Load the module and mount its filesystem. Both steps need root:

   ```sh
   sudo insmod kmod/coarse_stat_fs.ko
   sudo mount -t coarse_stat_fs none /sys/kernel/coarse_stat_fs
   ```

4. Read the pages:

   ```sh
   tools/csfcat cpu        # the cpu page, in /proc/stat format
   tools/csfcat self 100   # sleep 50 ms, spin 50 ms, print the diff
   build/eventloop        # the example: reports only the slow operations
   ```

   `csfcat self 100` prints a line similar to this one:

   ```
   sleep 50 ms + spin 50 ms: wall 100.141ms = cpu 50.014 + runq 0.000 + sleep 50.127 + ...
   ```

5. Unmount and unload when you're done:

   ```sh
   sudo umount /sys/kernel/coarse_stat_fs
   sudo rmmod coarse_stat_fs
   ```

## Repository layout

| Path | Contents |
|------|----------|
| `include/uapi/coarse_stat_fs.h` | The ABI: page layouts, constants, flags. Shared by the module and user space. |
| `kmod/` | The module. `main.c` (init, filesystem, clock), `thread.c` (per-thread pages, preempt notifiers), `cpu.c` (cpu page), `compat.h` (fallbacks for older kernels). |
| `lib/coarse_stat_fs.h` | Header-only user-space library (MIT): mapping, snapshots, diff formatting. |
| `tools/` | `csfcat`, which prints the pages as text. |
| `examples/` | `eventloop.c`: profiles a loop and reports only the slow operations. |
| `tests/` | Test programs (`t_*.c`), the benchmark, and the VM harnesses. |
| `tests/debian/` | Tests against Debian kernels, as CI runs them. |
| `scripts/` | `kmod-compdb.py` (compile database for the module), `pre-commit` (git hook). |
| `packaging/` | systemd mount unit and modules-load configuration. |
| `dkms.conf` | DKMS build configuration. |
| `LICENSE`, `LICENSES/` | GPL-2.0, and the texts of the MIT license and the syscall note. See [License](#license). |
| `.github/workflows/ci.yml` | CI: lint, then tests on Debian kernels. |
| `.clang-format`, `.clang-tidy`, `kmod/.clang-tidy`, `.editorconfig` | Code style and static-check configuration. |

## Developing

### Running common tasks

Every task has a make target. Run `make help` to list them.

| Command | What it does |
|---------|--------------|
| `make` | Builds the module, tools, example and tests. |
| `make check` | Runs all tests in a VM that boots the host kernel. |
| `make bench` | Runs the benchmarks in a VM with memory debugging off. |
| `make debian SUITE=bookworm` | Runs all tests on a Debian kernel, the way CI does. |
| `make format` | Reformats all C sources with clang-format. |
| `make lint` | Runs every static check: clang-format, checkpatch, shellcheck, clang-tidy, and a `W=1` build. |
| `make compdb` | Generates `compile_commands.json` for clangd and clang-tidy. |
| `make clean` | Removes all build output. |

To reload the module on the host after a rebuild, optionally with a
different refresh period, and then check the kernel log:

```sh
sudo umount /sys/kernel/coarse_stat_fs; sudo rmmod coarse_stat_fs
sudo insmod kmod/coarse_stat_fs.ko period_us=2000
sudo mount -t coarse_stat_fs none /sys/kernel/coarse_stat_fs
sudo dmesg | tail -n 50
```

`rmmod` fails while any registered thread is still alive; see
[Caveats](#caveats).

`make` builds the module against the running kernel's headers,
`/lib/modules/$(uname -r)/build`, which the headers package installs. If
they are missing, the build stops and tells you which package to install. To
build for a different kernel, set `KDIR`, for example
`make KDIR=/usr/src/linux-headers-6.12.0-amd64`.

### Following the code style

All C code, kernel and user space, uses Linux kernel style with a 100-column
limit. `.clang-format` encodes it; run `make format` before committing.

The static checks are split by where the code runs:

- Kernel code (`kmod/`, `include/uapi/`) goes through `checkpatch.pl` from
  the kernel headers, clang-tidy with `kmod/.clang-tidy` (the static
  analyzer and the `linuxkernel-*` checks), and a `W=1` build. The module
  builds with `-Werror`.
- User-space code (`lib/`, `tools/`, `tests/`, `examples/`) goes through
  clang-tidy with `.clang-tidy`. The comments at the top of that file
  explain why some checks are off.

`make lint` runs all of them. To run the fast checks on every commit,
install the hook:

```sh
ln -s ../../scripts/pre-commit .git/hooks/pre-commit
```

### Setting up your editor

Run `make compdb` once, and again after you add files. It writes
`compile_commands.json` for user-space code and `kmod/compile_commands.json`
for the module. kbuild generates the module's database with the host
kernel's gcc flags, so `scripts/kmod-compdb.py` removes the flags that clang
rejects. clangd then handles both kernel and user-space files. Without a
database, clangd falls back to `compile_flags.txt`, which covers user space
only.

### Supporting older kernels

The module targets current kernels. `kmod/compat.h` provides fallbacks back
to 6.1, and each fallback names the release that introduced the real
interface. When a new kernel breaks the build, add the fallback there rather
than at the call site, and run `make debian SUITE=<suite>` for the oldest and
newest suites.

## Testing

### Running the tests on your kernel

`make check` (`tests/vm.sh`) boots the host kernel in QEMU with virtme-ng,
loads the module, and runs the tests inside the guest. The guest boots with
`panic_on_warn`, `slub_debug`, `init_on_free` and KFENCE, so any kernel
warning fails the run.

```sh
tests/vm.sh                  # cpu thread life stress tools
tests/vm.sh thread life      # only these
VM_DEBUG=0 tests/vm.sh bench # benchmarks without memory debugging
```

The log is in `build/vm/tests.log`. If the guest dies, `tests/vm.sh` prints
the tail of `build/vm/console.log`, which contains the kernel's oops or
panic. `VM_TIMEOUT` (seconds, default 600) limits the run.

| Test | What it checks |
|------|----------------|
| `t_cpu` | Layout, refresh rate, the aggregate row against the per-CPU rows, agreement with `/proc/stat`. |
| `t_thread` | Bucket accuracy for spinning, sleeping, uninterruptible waits (vfork), O_DIRECT I/O, run-queue contention and migration; cross-checks against `/proc/thread-self/{schedstat,stat,io}`. |
| `t_life` | Refcounts and lifetimes: owner and non-owner release, revival, thread death with live mappings, fork, exec. |
| `t_stress` | About 80,000 threads in 20 s with random release orders; the module refcount must return to its baseline. |
| `tools` | `csfcat` and the example. |

Every run also checks that `rmmod` fails while an object is alive and
succeeds afterwards, and that the kernel log has no warnings.

### Running the tests on Debian kernels

`tests/debian/run.sh SUITE` reproduces a CI job locally:

1. `mmdebstrap` builds a root filesystem for the suite, with the suite's
   kernel, headers and compiler and a copy of this tree.
2. QEMU boots that kernel from the filesystem.
3. `tests/debian/guest.sh` runs as the guest's init. It builds the module and
   tests inside the guest and runs them.

```sh
tests/debian/run.sh bookworm           # kernel 6.1
tests/debian/run.sh trixie-backports   # newest stable kernel
tests/debian/run.sh sid thread life    # a subset of the tests
```

The script needs sudo, `mmdebstrap`, `qemu-system-x86`, `e2fsprogs` and
`curl`. The console log, including all test output, is in
`build/debian/<suite>/console.log`.

On hosts without mmdebstrap, such as Arch, use `tests/debian/docker.sh`
with the same arguments. It runs `run.sh` in an Ubuntu container, as on the
CI runner.

### Writing a test

Tests are plain C programs that use `tests/t.h`:

```c
#include "t.h"

int main(void)
{
	struct csf_snap a, b;

	TEST("my scenario") {
		t_snap(&a); /* dies if the snapshot fails */
		t_sleep_ns(10000000);
		t_snap(&b);
		CHECK(t_diff(&a, &b).sleep > 9, "slept too little");
	}
	return t_done("t_mine");
}
```

Add the program to `TESTS` in `tests/Makefile`, then add a line for it in
`tests/vm-inner.sh`. Keep timing assertions loose: CI runners are shared
VMs, and time stolen by the host counts as the thread's current state.

## Continuous integration

`.github/workflows/ci.yml` runs on every push and pull request, and weekly to
pick up new Debian kernels:

- `lint` runs clang-format, checkpatch, shellcheck and the user-space
  clang-tidy checks, with the tools pinned to the versions used locally.
- `debian` runs `tests/debian/run.sh` for each suite: bookworm (6.1),
  bookworm-backports and trixie (6.12), and trixie-backports, forky and sid
  (7.2). Each job uploads its console log as an artifact.

The jobs don't use containers. GitHub's Ubuntu runners have KVM, so each
guest runs at close to native speed.

## Interface

The module registers a filesystem, `coarse_stat_fs`, and creates its mount point
`/sys/kernel/coarse_stat_fs`:

| File | Contents |
|------|----------|
| `thread-self` | One page for the calling thread (`struct csf_thread_page`). |
| `cpu` | `/proc/stat`-style rows: the aggregate and one per CPU (`struct csf_cpu_page`). |

Both files have mode 0444. Any thread can map them without privileges, but
only read-only, with `MAP_SHARED`, at offset 0. To find the length to map,
`fstat()` the open file: the kernel reports it as `st_size`. That is one
page for `thread-self`, and the header plus one row per possible CPU for
`cpu`. `lib/coarse_stat_fs.h` does this for you, so nothing hard-codes a
page size.

`include/uapi/coarse_stat_fs.h` defines the ABI. Every page starts with a 64-byte
`struct csf_hdr` that holds a magic number, the ABI version, the page kind
and size, flags, and the TSC frequency and cycles-to-ns conversion.

### Thread page

The thread page records the time since the thread first opened
`thread-self`, split into five buckets of TSC cycles:

| Bucket | Meaning |
|--------|---------|
| `oncpu` | Running. |
| `runq` | Runnable and waiting for a CPU, including the wait after a wakeup. |
| `sleep` | Interruptible sleep. |
| `dsleep` | Uninterruptible sleep, not I/O. |
| `iowait` | Blocked with `in_iowait` set. |

The buckets add up exactly to elapsed wall time. The page also records:

- the current state, the CPU, and the TSC value at the last switch
- counts of switch-ins, preemptions, blocks and migrations
- warm fields copied from the task at each switch-out: the schedstat triple,
  nvcsw and nivcsw, utime and stime, minor and major faults, and the
  `/proc/<pid>/io` counters

The kernel updates the page from preempt notifiers at every context switch
of a registered thread, under a seqcount. The reader extrapolates the
current bucket with `rdtscp - last_switch_tsc`, so a snapshot is exact to the
cycle even though the kernel writes only at switches.

Each `open()` of `thread-self` returns its own file descriptor, but all of a
thread's descriptors share one refcounted object: one page and one notifier.
The object goes away when the thread dies or when the thread closes its last
file. If another thread holds the last reference, for example after the
owner handed off its mapping, closing that reference only parks the object.
A parked object stops accounting, and the owner's next open resets the page
and resumes. The page memory is charged to the opener's memory cgroup.

`lib/coarse_stat_fs.h` maps the page once per thread on first use, keeps it in
thread-local storage, and unmaps it at thread exit. After `fork()`, the child
maps its own page, because the mapping is `VM_DONTCOPY`.

### Reading a snapshot

`csf_thread_snap()` fills a `struct csf_snap`: a private copy of the thread
page in `s.p`, the TSC value at the time of the copy in `s.tsc`, and
`s.extrapolated`, which is set when the current bucket was brought up to
`s.tsc`. Because the copy is private, you can read its fields at any later
time without retrying. Subtract two snapshots field by field:

```c
#include <stdio.h>

#include "lib/coarse_stat_fs.h"

struct csf_snap a, b;

if (csf_thread_snap(&a))
	return -1; /* errno says why, for example ENOENT when not mounted */
op();
csf_thread_snap(&b);

/* Time: TSC cycles; the header holds the conversion to ns. */
const struct csf_hdr *h = &b.p.hdr;
double wall_ms = csf_cyc_to_ns(h, b.tsc - a.tsc) / 1e6;
double iowait_ms = csf_cyc_to_ns(h, b.p.iowait_cyc - a.p.iowait_cyc) / 1e6;

/* Counters: plain differences. */
unsigned long long blocked = b.p.nr_block - a.p.nr_block;
unsigned long long major_faults = b.p.maj_flt - a.p.maj_flt;

printf("%.1f ms, %.1f ms in iowait, blocked %llu times, %llu major faults\n", wall_ms,
       iowait_ms, blocked, major_faults);
```

The table at the top lists each field's unit and freshness. The five
buckets of `b` minus `a` add up to `b.tsc - a.tsc` when both snapshots are
extrapolated, so the largest bucket tells you what an operation mostly did.

The fields copied at switch-out lag behind a thread that stays on the CPU.
In one run of the example, a 30 ms spin showed 30.0 ms in `oncpu_cyc` but
18.7 ms of `sum_exec_ns`, because the spin's last stretch on the CPU hadn't
been copied yet. Use the buckets to measure CPU time.

For the whole machine, `csf_cpu_snap(&c, CSF_CPU_ALL)` copies the sum row,
and `csf_cpu_snap(&c, cpu)` copies one CPU's row. Rows are in nanoseconds
and only as fresh as `period_us`; `c.update_mono_ns` says when they were
taken.

`examples/eventloop.c` puts all of this together. For each slow operation
it prints:

```
slow op sleep wall 25.1 ms, mostly sleeping (poll, futex, timer, ...)
  time       cpu 0.0 + runq 0.0 + sleep 25.1 + dsleep 0.0 + iowait 0.0 ms
  switches   0 preempted, 1 blocked, 0 migrated; tid 26071 now on cpu 0
  faults     3 minor, 0 major
  i/o        0 read and 0 write syscalls, 0 B read and 0 B written (0 B and 0 B at the block layer)
  scheduler  ran 0.0 ms, waited 0.0 ms runnable (/proc/<tid>/schedstat)
  system     user 8.0, system 0.0, iowait 0.0, steal 0.0 ms on all CPUs (refreshed every 10 ms)
  one line   wall 25.064ms = cpu 0.006 + runq 0.000 + sleep 25.057 + dsleep 0.000 + iowait 0.000 | preempt 0 block 1 migrate 0 | flt 3/0 | sysc r0 w0 | io r0B w0B
```

### CPU page

The `cpu` page holds row 0, the sum over all CPUs, and one row per possible
CPU. Each row has user, nice, system, idle, iowait, irq, softirq, steal,
guest and guest_nice in nanoseconds, plus interrupt and softirq totals. The
values come from the same sources as `/proc/stat`: `kcpustat_cpu_fetch()`,
plus the NO_HZ idle counters on kernels before 7.2, and
`kstat_cpu_irqs_sum()`.

Delayed work refreshes the rows every `period_us` microseconds (default
10000, range 1000 to 1000000), only while at least one `cpu` file is open.
`update_tsc` and `update_mono_ns` tell you how old the data is. Over an
operation shorter than a few periods, a row delta shows whether the machine
was busy, not an exact share: the 30 ms spin above came with 21.9 ms of user
time across all CPUs. To convert a row value to the USER_HZ ticks of
`/proc/stat`, divide it by `1e9 / sysconf(_SC_CLK_TCK)`. To change the
period:

```sh
echo 2000 | sudo tee /sys/module/coarse_stat_fs/parameters/period_us
```

## Cost

These figures come from a KVM guest on the development host (Zen, 2 GHz
TSC), with memory debugging off (`make bench`):

| Operation | ns |
|-----------|----|
| `csf_thread_snap` | 16.4 |
| `rdtscp` alone | 14.6 |
| `csf_cpu_snap(CSF_CPU_ALL)` | 2.2 |
| `clock_gettime(CLOCK_THREAD_CPUTIME_ID)` | 101 |
| `pread /proc/thread-self/schedstat` | 159 |
| `pread /proc/stat` | 1843 |

A context switch of a registered thread costs about 35 ns extra: in a futex
ping-pong, 643 ns becomes 678 ns per switch. Threads that never open
`thread-self` pay nothing; the module calls `preempt_notifier_inc()` once,
which enables the notifier static key.

## Installing

To build the module for every installed kernel with DKMS, copy or link the
repository to `/usr/src/coarse_stat_fs-0.1`, then run:

```sh
sudo dkms install coarse_stat_fs/0.1
```

To load and mount it at boot, install the files in `packaging/`:

```sh
sudo install -m644 packaging/coarse_stat_fs.conf /etc/modules-load.d/
sudo install -m644 packaging/sys-kernel-coarse_stat_fs.mount /etc/systemd/system/
sudo systemctl enable sys-kernel-coarse_stat_fs.mount
```

## Caveats

- Cycle counts are meaningful only when `CSF_F_TSC_CONSTANT` is set
  (constant and nonstop TSC). The library extrapolates from another CPU's
  switch time only when `CSF_F_TSC_SYNCED` is also set, which means the
  kernel hasn't marked the TSC unstable.
- When you read another thread's page while it sleeps, the whole sleep is in
  the bucket of its block reason. The run-queue part of the wait after the
  wakeup moves to `runq` only at its next switch-in. Your own snapshots
  don't have this problem, because you are on the CPU when you take them.
- In a VM, host steal counts as whatever state the thread was in. The
  buckets come from the TSC, which keeps running while the hypervisor
  deschedules the vCPU, so a thread that was on the CPU gets the stolen time
  as cpu. `sum_exec_ns`, the scheduler's own clock, can exclude steal when
  the guest has paravirt steal accounting.
- The `irqs` total excludes the x86 architecture counters (NMI, LOC, and so
  on) that `/proc/stat` adds, so the two don't match exactly.
- A `vfork()` parent waits in killable sleep, so the wait shows in dsleep.
- A live thread object holds a module reference, so `rmmod` fails until
  every registered thread has exited or closed its files.
- The module uses `file_operations.mmap`. A kernel that requires
  `mmap_prepare` needs a small port.

## License

coarse_stat_fs is licensed under the GNU General Public License, version 2
only (GPL-2.0); the full text is in `LICENSE`. This covers the module, the
tools and the tests. Two parts carry looser terms, so that programs can use
them without becoming GPL:

| Files | License |
|-------|---------|
| `include/uapi/coarse_stat_fs.h` | GPL-2.0 WITH Linux-syscall-note (`LICENSES/Linux-syscall-note`), like the kernel's own uapi headers. |
| `lib/coarse_stat_fs.h`, `examples/eventloop.c` | MIT (`LICENSES/MIT`), so you can copy the library and the example into programs under any license. |

Every source file names its license in an `SPDX-License-Identifier` line.
