/* SPDX-License-Identifier: GPL-2.0 */
#ifndef COARSE_STAT_FS_T_H
#define COARSE_STAT_FS_T_H

#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../lib/coarse_stat_fs.h"

static int t_failures;
static const char *t_name = "?";

#define CHECK(cond, ...)                                                                    \
	do {                                                                                \
		if (!(cond)) {                                                              \
			t_failures++;                                                       \
			fprintf(stderr, "FAIL %s:%d [%s] %s: ", __FILE__, __LINE__, t_name, \
				#cond);                                                     \
			fprintf(stderr, __VA_ARGS__);                                       \
			fputc('\n', stderr);                                                \
		}                                                                           \
	} while (0)

#define DIE(...)                                                             \
	do {                                                                 \
		fprintf(stderr, "FATAL [%s] ", t_name);                      \
		fprintf(stderr, __VA_ARGS__);                                \
		fprintf(stderr, " (errno %d %s)\n", errno, strerror(errno)); \
		exit(2);                                                     \
	} while (0)

#define TEST(name)                                    \
	for (int _once = (t_name = (name), 1); _once; \
	     _once = 0, fprintf(stderr, "  ran %s\n", name))

static inline int t_done(const char *prog)
{
	if (t_failures)
		fprintf(stderr, "%s: %d FAILURE(S)\n", prog, t_failures);
	else
		fprintf(stderr, "%s: ok\n", prog);
	return t_failures ? 1 : 0;
}

/* The kernel's page size: the thread-self mapping must be exactly one page. */
static inline size_t t_page(void)
{
	return (size_t)sysconf(_SC_PAGESIZE);
}

/* Every live thread object and open file holds one module reference. */
static inline long t_refcnt(void)
{
	FILE *f = fopen("/sys/module/" CSF_FS_NAME "/refcnt", "re");
	long v = -1;

	if (!f || fscanf(f, "%ld", &v) != 1)
		DIE("refcnt");
	fclose(f);
	return v;
}

static inline uint64_t t_now_ns(clockid_t clk)
{
	struct timespec ts;

	clock_gettime(clk, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static inline void t_sleep_ns(uint64_t ns)
{
	struct timespec ts = { ns / 1000000000ULL, ns % 1000000000ULL };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static inline void t_spin_ns(uint64_t ns)
{
	uint64_t end = t_now_ns(CLOCK_MONOTONIC) + ns;

	while (t_now_ns(CLOCK_MONOTONIC) < end)
		;
}

static inline void t_snap(struct csf_snap *s)
{
	if (csf_thread_snap(s))
		DIE("csf_thread_snap");
}

static inline void t_cpu_snap(struct csf_cpu_snap *s, int cpu)
{
	if (csf_cpu_snap(s, cpu))
		DIE("csf_cpu_snap");
}

static inline void t_pin(int cpu)
{
	cpu_set_t set;

	CPU_ZERO(&set);
	CPU_SET(cpu, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		DIE("sched_setaffinity(%d)", cpu);
}

static inline double t_ms(const struct csf_snap *s, uint64_t cyc)
{
	return csf_cyc_to_ns(&s->p.hdr, cyc) / 1e6;
}

/* Bucket deltas b - a, in ms. */
struct t_delta {
	double wall, oncpu, runq, sleep, dsleep, iowait;
	uint64_t sum_cyc, wall_cyc;
};

static inline struct t_delta t_diff(const struct csf_snap *a, const struct csf_snap *b)
{
	struct t_delta d;
	uint64_t c[5] = {
		b->p.oncpu_cyc - a->p.oncpu_cyc,   b->p.runq_cyc - a->p.runq_cyc,
		b->p.sleep_cyc - a->p.sleep_cyc,   b->p.dsleep_cyc - a->p.dsleep_cyc,
		b->p.iowait_cyc - a->p.iowait_cyc,
	};

	d.wall_cyc = b->tsc - a->tsc;
	d.sum_cyc = c[0] + c[1] + c[2] + c[3] + c[4];
	d.wall = t_ms(b, d.wall_cyc);
	d.oncpu = t_ms(b, c[0]);
	d.runq = t_ms(b, c[1]);
	d.sleep = t_ms(b, c[2]);
	d.dsleep = t_ms(b, c[3]);
	d.iowait = t_ms(b, c[4]);
	return d;
}

static inline void t_print(const char *what, const struct csf_snap *a, const struct csf_snap *b)
{
	char buf[CSF_DIFF_BUFSZ];

	csf_format_diff(buf, sizeof(buf), a, b);
	fprintf(stderr, "    %-10s %s\n", what, buf);
}

#endif
