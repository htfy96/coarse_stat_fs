// SPDX-License-Identifier: GPL-2.0
/* csfcat cpu: /proc/stat-style rows. csfcat self [MS]: sleep and spin MS/2 each, then diff. */
#include <stdlib.h>

#include "../lib/coarse_stat_fs.h"

static int cat_cpu(void)
{
	const uint64_t t = csf_ns_per_tick();
	const struct csf_cpu_page *cp = csf_cpu_page();
	struct csf_cpu_snap s;
	struct timespec ts;

	if (!cp) {
		perror("csfcat: " CSF_CPU_PATH);
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &ts);
	for (int cpu = -1; cpu + 1 < (int)cp->nr_rows; cpu++) {
		if (csf_cpu_snap(&s, cpu))
			return 1;
		if (cpu >= 0 && !s.row.online)
			continue;
		if (cpu < 0)
			printf("cpu ");
		else
			printf("cpu%d", cpu);
		printf(" %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu\n",
		       (unsigned long long)(s.row.user / t), (unsigned long long)(s.row.nice / t),
		       (unsigned long long)(s.row.system / t), (unsigned long long)(s.row.idle / t),
		       (unsigned long long)(s.row.iowait / t), (unsigned long long)(s.row.irq / t),
		       (unsigned long long)(s.row.softirq / t),
		       (unsigned long long)(s.row.steal / t), (unsigned long long)(s.row.guest / t),
		       (unsigned long long)(s.row.guest_nice / t));
	}
	if (csf_cpu_snap(&s, CSF_CPU_ALL))
		return 1;
	printf("# irqs %llu softirqs %llu, updated %.3f ms ago, period %.1f ms, %llu updates\n",
	       (unsigned long long)s.row.irqs, (unsigned long long)s.row.softirqs,
	       ((uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec - s.update_mono_ns) / 1e6,
	       cp->period_ns / 1e6, (unsigned long long)s.nr_updates);
	return 0;
}

static int cat_self(int ms)
{
	struct csf_snap a, b;
	struct timespec ts = { ms / 2000, (ms / 2 % 1000) * 1000000L };
	uint64_t end;
	char buf[CSF_DIFF_BUFSZ];

	if (csf_thread_snap(&a)) {
		perror("csfcat: " CSF_THREAD_SELF_PATH);
		return 1;
	}
	nanosleep(&ts, NULL);
	end = csf_rdtsc() + (uint64_t)ms / 2 * a.p.hdr.tsc_khz;
	while (csf_rdtsc() < end)
		;
	if (csf_thread_snap(&b))
		return 1;
	printf("tid %u cpu %u state %u flags %#x tsc_khz %llu\n", b.p.tid, b.p.cpu, b.p.state,
	       b.p.hdr.flags, (unsigned long long)b.p.hdr.tsc_khz);
	printf("switch_in %llu preempt %llu block %llu migrate %llu nvcsw %llu nivcsw %llu\n",
	       (unsigned long long)b.p.nr_switch_in, (unsigned long long)b.p.nr_preempt,
	       (unsigned long long)b.p.nr_block, (unsigned long long)b.p.nr_migrate,
	       (unsigned long long)b.p.nvcsw, (unsigned long long)b.p.nivcsw);
	printf("schedstat %llu %llu %llu\n", (unsigned long long)b.p.sum_exec_ns,
	       (unsigned long long)b.p.run_delay_ns, (unsigned long long)b.p.pcount);
	csf_format_diff(buf, sizeof(buf), &a, &b);
	printf("sleep %d ms + spin %d ms: %s\n", ms / 2, ms / 2, buf);
	return 0;
}

enum { SELF_DEFAULT_MS = 100, SELF_MAX_MS = 60000 };

int main(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "cpu"))
		return cat_cpu();
	if (argc >= 2 && argc <= 3 && !strcmp(argv[1], "self")) {
		long ms = SELF_DEFAULT_MS;
		char *end;

		if (argc == 3) {
			ms = strtol(argv[2], &end, 10);
			if (end == argv[2] || *end || ms < 0 || ms > SELF_MAX_MS) {
				fprintf(stderr, "csfcat: MS must be 0-%d\n", SELF_MAX_MS);
				return 2;
			}
		}
		return cat_self((int)ms);
	}
	fprintf(stderr, "usage: csfcat cpu | csfcat self [MS]\n");
	return 2;
}
