// SPDX-License-Identifier: MIT
/*
 * Profiling a hot loop: snapshot before every operation for ~20 ns, and
 * only when an operation is especially slow, take a second snapshot and
 * work out where the wall time went. report() reads each group of
 * snapshot fields by hand; csf_format_diff() prints the same data on one line.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <time.h>

#include "../lib/coarse_stat_fs.h"

enum {
	SLOW_MS = 20, /* report operations slower than this */
	SPIN_MS = 30,
	SLEEP_MS = 25,
	FSYNC_WRITES = 64,
};

static uint64_t tsc_khz; /* TSC cycles per millisecond */

static void op_spin(void)
{
	uint64_t end = csf_rdtsc() + SPIN_MS * tsc_khz;

	while (csf_rdtsc() < end)
		;
}

static void op_sleep(void)
{
	struct timespec ts = { 0, SLEEP_MS * 1000000L };

	nanosleep(&ts, NULL);
}

static void op_fsync(void)
{
	char path[] = "/tmp/eventloop.XXXXXX", data[4096] = { 1 };
	int fd = mkstemp(path);

	if (fd < 0)
		return;
	for (int i = 0; i < FSYNC_WRITES; i++)
		if (write(fd, data, sizeof(data)) < 0)
			break;
	fsync(fd);
	close(fd);
	unlink(path);
}

static void op_fast(void)
{
}

/* Time buckets and the wall clock are TSC cycles; the header converts them. */
static double cyc_ms(const struct csf_hdr *h, uint64_t cyc)
{
	return csf_cyc_to_ns(h, cyc) / 1e6;
}

/*
 * Explains one slow operation from snapshots of this thread (a, b) and of
 * the whole machine (ca, cb) taken before and after it.
 */
static void report(const char *name, const struct csf_snap *a, const struct csf_snap *b,
		   const struct csf_cpu_snap *ca, const struct csf_cpu_snap *cb)
{
#define D(f)   ((unsigned long long)(b->p.f - a->p.f))
#define SYS(f) ((cb->row.f - ca->row.f) / 1e6) /* cpu rows are in ns */
	const struct csf_hdr *h = &b->p.hdr;
	/*
	 * The five buckets partition wall time: when both snapshots are
	 * extrapolated, their differences add up to b->tsc - a->tsc.
	 */
	const struct {
		const char *name, *meaning;
		uint64_t cyc;
	} t[] = {
		{ "cpu", "running on a CPU", D(oncpu_cyc) },
		{ "runq", "waiting for a CPU", D(runq_cyc) },
		{ "sleep", "sleeping (poll, futex, timer, ...)", D(sleep_cyc) },
		{ "dsleep", "in uninterruptible sleep (kernel lock, ...)", D(dsleep_cyc) },
		{ "iowait", "waiting for block I/O", D(iowait_cyc) },
	};
	const size_t nt = sizeof(t) / sizeof(t[0]);
	char line[CSF_DIFF_BUFSZ];
	size_t top = 0;

	for (size_t i = 1; i < nt; i++)
		if (t[i].cyc > t[top].cyc)
			top = i;
	printf("slow op %-5s wall %.1f ms, mostly %s\n", name, cyc_ms(h, b->tsc - a->tsc),
	       t[top].meaning);

	printf("  time      ");
	for (size_t i = 0; i < nt; i++)
		printf("%s %s %.1f", i ? " +" : "", t[i].name, cyc_ms(h, t[i].cyc));
	printf(" ms%s\n", a->extrapolated && b->extrapolated ? "" : " (not extrapolated)");

	/* Counters, updated at every switch: how often it left the CPU, and why. */
	printf("  switches   %llu preempted, %llu blocked, %llu migrated; tid %u now on cpu %u\n",
	       D(nr_preempt), D(nr_block), D(nr_migrate), b->p.tid, b->p.cpu);

	/*
	 * Copied from the task at switch-out, so these cover the operation up
	 * to the thread's last switch-out: complete for an operation that
	 * blocked at the end, possibly short for one that spun.
	 */
	printf("  faults     %llu minor, %llu major\n", D(min_flt), D(maj_flt));
	printf("  i/o        %llu read and %llu write syscalls, %llu B read and %llu B written"
	       " (%llu B and %llu B at the block layer)\n",
	       D(syscr), D(syscw), D(rchar), D(wchar), D(read_bytes), D(write_bytes));
	printf("  scheduler  ran %.1f ms, waited %.1f ms runnable (/proc/<tid>/schedstat)\n",
	       D(sum_exec_ns) / 1e6, D(run_delay_ns) / 1e6);

	/* The whole machine: CSF_CPU_ALL sums every CPU's row. */
	printf("  system     user %.1f, system %.1f, iowait %.1f, steal %.1f ms on all CPUs"
	       " (refreshed every %.0f ms)\n",
	       SYS(user), SYS(system), SYS(iowait), SYS(steal), csf_cpu_page()->period_ns / 1e6);

	csf_format_diff(line, sizeof(line), a, b);
	printf("  one line   %s\n", line);
#undef SYS
#undef D
}

int main(void)
{
	static const struct {
		const char *name;
		void (*fn)(void);
	} ops[] = {
		{ "fast", op_fast },   { "spin", op_spin }, { "fast", op_fast },
		{ "sleep", op_sleep }, { "fast", op_fast }, { "fsync", op_fsync },
	};
	struct csf_snap before, after;
	struct csf_cpu_snap sys_before, sys_after;
	uint64_t slow;

	if (!csf_thread_page()) {
		perror("coarse_stat_fs");
		return 1;
	}
	tsc_khz = csf_thread_page()->hdr.tsc_khz;
	slow = SLOW_MS * tsc_khz;

	for (unsigned int i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		if (csf_thread_snap(&before) || csf_cpu_snap(&sys_before, CSF_CPU_ALL)) {
			perror("coarse_stat_fs");
			return 1;
		}
		ops[i].fn();
		/* Cheap check first: the snapshot already carries the TSC. */
		if (__builtin_expect(csf_rdtsc() - before.tsc > slow, 0)) {
			if (csf_thread_snap(&after) || csf_cpu_snap(&sys_after, CSF_CPU_ALL))
				return 1;
			report(ops[i].name, &before, &after, &sys_before, &sys_after);
		}
	}
	return 0;
}
