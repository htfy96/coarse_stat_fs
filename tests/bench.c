// SPDX-License-Identifier: GPL-2.0
/*
 * Read cost of a snapshot vs the syscall alternatives, and the extra
 * context-switch cost a registered thread pays.
 */
#include "t.h"

#include <linux/futex.h>
#include <pthread.h>
#include <sys/syscall.h>

static double ns_per(uint64_t t0, uint64_t t1, long n)
{
	return (double)(t1 - t0) / n;
}

static void bench_reads(void)
{
	enum { N = 5000000, NS = 100000 };
	struct csf_snap s;
	struct csf_cpu_snap c;
	char buf[256];
	uint64_t t0, t1, sink = 0;
	int fd;

	t_snap(&s);
	t_cpu_snap(&c, CSF_CPU_ALL);

	t0 = t_now_ns(CLOCK_MONOTONIC);
	for (long i = 0; i < N; i++) {
		csf_thread_snap(&s);
		sink += s.p.oncpu_cyc;
	}
	t1 = t_now_ns(CLOCK_MONOTONIC);
	printf("  csf_thread_snap                         %8.1f ns\n", ns_per(t0, t1, N));

	t0 = t_now_ns(CLOCK_MONOTONIC);
	for (long i = 0; i < N; i++) {
		csf_cpu_snap(&c, CSF_CPU_ALL);
		sink += c.row.user;
	}
	t1 = t_now_ns(CLOCK_MONOTONIC);
	printf("  csf_cpu_snap(ALL)                       %8.1f ns\n", ns_per(t0, t1, N));

	t0 = t_now_ns(CLOCK_MONOTONIC);
	for (long i = 0; i < N; i++)
		sink += csf_rdtsc();
	t1 = t_now_ns(CLOCK_MONOTONIC);
	printf("  rdtscp alone                           %8.1f ns\n", ns_per(t0, t1, N));

	t0 = t_now_ns(CLOCK_MONOTONIC);
	for (long i = 0; i < NS; i++)
		sink += t_now_ns(CLOCK_THREAD_CPUTIME_ID);
	t1 = t_now_ns(CLOCK_MONOTONIC);
	printf("  clock_gettime(THREAD_CPUTIME)          %8.1f ns\n", ns_per(t0, t1, NS));

	fd = open("/proc/thread-self/schedstat", O_RDONLY);
	t0 = t_now_ns(CLOCK_MONOTONIC);
	for (long i = 0; i < NS; i++)
		sink += pread(fd, buf, sizeof(buf), 0);
	t1 = t_now_ns(CLOCK_MONOTONIC);
	close(fd);
	printf("  pread(/proc/thread-self/schedstat)     %8.1f ns\n", ns_per(t0, t1, NS));

	fd = open("/proc/stat", O_RDONLY);
	t0 = t_now_ns(CLOCK_MONOTONIC);
	for (long i = 0; i < NS / 10; i++) {
		char big[8192];

		sink += pread(fd, big, sizeof(big), 0);
	}
	t1 = t_now_ns(CLOCK_MONOTONIC);
	close(fd);
	printf("  pread(/proc/stat)                      %8.1f ns\n", ns_per(t0, t1, NS / 10));
	if (sink == 42)
		puts("");
}

/* Futex ping-pong between two threads pinned to one CPU. */
static uint32_t turn;
static int pp_register;
static long pp_rounds;

static void fwait(uint32_t *a, uint32_t v)
{
	while (__atomic_load_n(a, __ATOMIC_ACQUIRE) == v)
		syscall(SYS_futex, a, FUTEX_WAIT_PRIVATE, v, NULL, NULL, 0);
}

static void fwake(uint32_t *a, uint32_t v)
{
	__atomic_store_n(a, v, __ATOMIC_RELEASE);
	syscall(SYS_futex, a, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

static void *pong(void *arg)
{
	struct csf_snap s;

	(void)arg;
	t_pin(0);
	if (pp_register)
		t_snap(&s);
	for (long i = 0; i < pp_rounds; i++) {
		fwait(&turn, 0);
		fwake(&turn, 0);
	}
	return NULL;
}

static double pingpong(int reg, long rounds)
{
	pthread_t th;
	struct csf_snap s;
	uint64_t t0, t1;

	pp_register = reg;
	pp_rounds = rounds;
	turn = 0;
	pthread_create(&th, NULL, pong, NULL);
	t_pin(0);
	t_sleep_ns(10000000);
	if (reg)
		t_snap(&s);
	t0 = t_now_ns(CLOCK_MONOTONIC);
	for (long i = 0; i < rounds; i++) {
		fwake(&turn, 1);
		fwait(&turn, 1);
	}
	t1 = t_now_ns(CLOCK_MONOTONIC);
	pthread_join(th, NULL);
	return (double)(t1 - t0) / rounds / 2; /* per context switch */
}

static void *pingpong_unregistered(void *arg)
{
	*(double *)arg = pingpong(0, 300000);
	return NULL;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;

	return x < y ? -1 : x > y;
}

int main(void)
{
	enum { RUNS = 9, ROUNDS = 300000 };
	double base[RUNS], reg[RUNS];
	pthread_t th;

	printf("read cost:\n");
	bench_reads();

	/* Alternate variants; each unregistered run uses fresh threads. */
	printf("context switch (futex ping-pong, one CPU, median of %d):\n", RUNS);
	for (int i = 0; i < RUNS; i++) {
		pthread_create(&th, NULL, pingpong_unregistered, &base[i]);
		pthread_join(th, NULL);
		reg[i] = pingpong(1, ROUNDS);
	}
	qsort(base, RUNS, sizeof(double), cmp_double);
	qsort(reg, RUNS, sizeof(double), cmp_double);
	printf("  unregistered %6.1f ns  (min %6.1f)\n", base[RUNS / 2], base[0]);
	printf("  registered   %6.1f ns  (min %6.1f)\n", reg[RUNS / 2], reg[0]);
	printf("  overhead     %+6.1f ns per switch (median), %+6.1f ns (min)\n",
	       reg[RUNS / 2] - base[RUNS / 2], reg[0] - base[0]);
	return 0;
}
