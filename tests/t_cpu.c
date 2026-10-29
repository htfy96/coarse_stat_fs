// SPDX-License-Identifier: GPL-2.0
/* The "cpu" page: permissions, layout, agreement with /proc/stat, refresh. */
#include "t.h"

#include <sys/mman.h>
#include <sys/stat.h>

/* Parses "cpu" (cpu == -1) or "cpuN" from /proc/stat into USER_HZ ticks. */
static int proc_stat_row(int cpu, unsigned long long v[10])
{
	char want[sizeof("cpu-2147483648 ")], *line = NULL;
	FILE *f = fopen("/proc/stat", "re");
	size_t cap = 0;
	int found = 0, n;

	if (!f)
		DIE("open /proc/stat");
	n = cpu < 0 ? snprintf(want, sizeof(want), "cpu ") :
		      snprintf(want, sizeof(want), "cpu%d ", cpu);
	if (n < 0 || (size_t)n >= sizeof(want))
		DIE("cpu %d", cpu);
	/* getline: the "intr" line runs to many kilobytes. */
	while (!found && getline(&line, &cap, f) >= 0) {
		if (strncmp(line, want, (size_t)n))
			continue;
		found = sscanf(line + n, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0],
			       &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]) == 10;
	}
	free(line);
	fclose(f);
	return found;
}

static void row_ticks(const struct csf_cpu_row *r, unsigned long long v[10])
{
	const uint64_t tick = csf_ns_per_tick();
	const uint64_t ns[10] = { r->user, r->nice,    r->system, r->idle,  r->iowait,
				  r->irq,  r->softirq, r->steal,  r->guest, r->guest_nice };

	for (int i = 0; i < 10; i++)
		v[i] = ns[i] / tick;
}

static void compare(int cpu, long ncpus, uint64_t period_ns)
{
	static const char *const names[10] = { "user", "nice",    "system", "idle",  "iowait",
					       "irq",  "softirq", "steal",  "guest", "guest_nice" };
	unsigned long long mine[10], proc[10];
	struct csf_cpu_snap s;
	/* The page may lag /proc/stat by up to a period plus scheduling slop. */
	long long slack = (long long)((period_ns + 30000000ULL) / csf_ns_per_tick() + 2) *
			  (cpu < 0 ? ncpus : 1);

	CHECK(csf_cpu_snap(&s, cpu < 0 ? CSF_CPU_ALL : cpu) == 0, "snap cpu %d", cpu);
	if (!proc_stat_row(cpu, proc))
		return; /* offline CPUs are absent from /proc/stat */
	row_ticks(&s.row, mine);
	for (int i = 0; i < 10; i++) {
		long long d = (long long)proc[i] - (long long)mine[i];

		CHECK(d >= -1 && d <= slack, "cpu %d %s: proc %llu page %llu (slack %lld)", cpu,
		      names[i], proc[i], mine[i], slack);
	}
}

int main(void)
{
	long ncpus = sysconf(_SC_NPROCESSORS_CONF);
	const struct csf_cpu_page *cp;
	struct stat st = { 0 };

	TEST("permissions") {
		int fd = open(CSF_CPU_PATH, O_RDWR);

		CHECK(fd < 0 && errno == EACCES, "O_RDWR open: fd %d errno %d", fd, errno);
		fd = open(CSF_CPU_PATH, O_RDONLY);
		if (fd < 0 || fstat(fd, &st))
			DIE("open cpu");
		void *p = mmap(NULL, t_page(), PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
		CHECK(p == MAP_FAILED, "private writable mapping allowed");
		p = mmap(NULL, t_page(), PROT_READ, MAP_SHARED, fd, (off_t)t_page());
		CHECK(p == MAP_FAILED && errno == EINVAL, "nonzero offset allowed");
		p = mmap(NULL, t_page(), PROT_READ, MAP_SHARED, fd, 0);
		CHECK(p != MAP_FAILED, "read-only mapping refused: %d", errno);
		if (p != MAP_FAILED) {
			CHECK(mprotect(p, t_page(), PROT_READ | PROT_WRITE) != 0,
			      "mprotect to writable allowed");
			munmap(p, t_page());
		}
		close(fd);
	}

	cp = csf_cpu_page();
	if (!cp)
		DIE("csf_cpu_page");

	TEST("layout") {
		CHECK(cp->hdr.kind == CSF_KIND_CPU, "kind %u", cp->hdr.kind);
		CHECK(cp->hdr.tsc_khz > 0, "tsc_khz 0");
		CHECK(cp->nr_rows >= (uint32_t)ncpus + 1, "nr_rows %u ncpus %ld", cp->nr_rows,
		      ncpus);
		CHECK(cp->row_size == sizeof(struct csf_cpu_row), "row_size %u", cp->row_size);
		CHECK(cp->rows_off == sizeof(struct csf_cpu_page), "rows_off %u", cp->rows_off);
		CHECK(st.st_size == cp->hdr.size &&
			      cp->hdr.size == cp->rows_off + (size_t)cp->nr_rows * cp->row_size,
		      "st_size %lld hdr.size %u", (long long)st.st_size, cp->hdr.size);
		CHECK(cp->period_ns >= 1000000, "period_ns %llu",
		      (unsigned long long)cp->period_ns);
	}

	TEST("refresh") {
		struct csf_cpu_snap a, b;
		uint64_t period = cp->period_ns;

		t_cpu_snap(&a, CSF_CPU_ALL);
		t_sleep_ns(period * 6);
		t_cpu_snap(&b, CSF_CPU_ALL);
		CHECK(b.nr_updates - a.nr_updates >= 3 && b.nr_updates - a.nr_updates <= 8,
		      "%llu updates in 6 periods",
		      (unsigned long long)(b.nr_updates - a.nr_updates));
		CHECK(b.update_mono_ns > a.update_mono_ns, "update time did not advance");
		/* Staleness: last update no older than ~2 periods. */
		CHECK(t_now_ns(CLOCK_MONOTONIC) - b.update_mono_ns < 2 * period + 20000000,
		      "stale by %llu ns",
		      (unsigned long long)(t_now_ns(CLOCK_MONOTONIC) - b.update_mono_ns));
	}

	TEST("sum row") {
		/* rows[0] must equal the sum of the per-CPU rows in one snapshot. */
		const struct csf_cpu_row *rows = (const void *)((const char *)cp + cp->rows_off);
		struct csf_cpu_row sum, r0;
		uint32_t seq;

		do {
			seq = __atomic_load_n(&cp->seq, __ATOMIC_ACQUIRE);
			memset(&sum, 0, sizeof(sum));
			r0 = rows[0];
			for (uint32_t i = 1; i < cp->nr_rows; i++) {
				sum.user += rows[i].user;
				sum.idle += rows[i].idle;
				sum.softirqs += rows[i].softirqs;
				sum.online += rows[i].online;
			}
			__atomic_thread_fence(__ATOMIC_ACQUIRE);
		} while ((seq & 1) || __atomic_load_n(&cp->seq, __ATOMIC_RELAXED) != seq);
		CHECK(sum.user == r0.user && sum.idle == r0.idle && sum.softirqs == r0.softirqs,
		      "aggregate row is not the sum");
		CHECK(r0.online == (uint32_t)sysconf(_SC_NPROCESSORS_ONLN), "online %u", r0.online);
	}

	TEST("matches /proc/stat") {
		compare(-1, ncpus, cp->period_ns);
		for (int c = 0; c < ncpus; c++)
			compare(c, ncpus, cp->period_ns);
	}

	TEST("busy cpu shows up") {
		struct csf_cpu_snap a, b;

		t_pin(0);
		t_cpu_snap(&a, 0);
		t_spin_ns(200000000);
		t_sleep_ns(cp->period_ns * 2);
		t_cpu_snap(&b, 0);
		double busy = (b.row.user + b.row.system - a.row.user - a.row.system) / 1e6;
		CHECK(busy > 150 && busy < 260, "cpu0 user+system %.1f ms for 200 ms spin", busy);
	}

	return t_done("t_cpu");
}
