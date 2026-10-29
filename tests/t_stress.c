// SPDX-License-Identifier: GPL-2.0
/*
 * Workers register and exit in random ways while reapers release their files.
 * The module refcount must end at its baseline.
 */
#include "t.h"

#include <pthread.h>
#include <sys/mman.h>
#include <stdatomic.h>

#define QLEN 4096

/* Mappings handed to reapers for non-owner release. */
static struct {
	pthread_mutex_t mu;
	void *q[QLEN];
	int n;
} handoff = { .mu = PTHREAD_MUTEX_INITIALIZER };

static atomic_int stop, stop_reapers;
static atomic_long nr_threads, nr_handoff, nr_revive;

static void *map_new(void)
{
	int fd = open(CSF_THREAD_SELF_PATH, O_RDONLY | O_CLOEXEC);
	void *p;

	if (fd < 0)
		DIE("open");
	p = mmap(NULL, t_page(), PROT_READ, MAP_SHARED, fd, 0);
	close(fd);
	if (p == MAP_FAILED)
		DIE("mmap");
	return p;
}

static int hand_off(void *p)
{
	int ok = 0;

	pthread_mutex_lock(&handoff.mu);
	if (handoff.n < QLEN) {
		handoff.q[handoff.n++] = p;
		ok = 1;
	}
	pthread_mutex_unlock(&handoff.mu);
	if (ok)
		atomic_fetch_add(&nr_handoff, 1);
	return ok;
}

static void churn(unsigned int *seed)
{
	switch (rand_r(seed) % 4) {
	case 0:
		sched_yield();
		break;
	case 1:
		t_sleep_ns(rand_r(seed) % 200000);
		break;
	case 2:
		t_spin_ns(rand_r(seed) % 200000);
		break;
	default:
		break;
	}
}

static void *worker(void *arg)
{
	unsigned int seed = (unsigned int)(uintptr_t)arg;
	const struct csf_thread_page *pg = map_new();
	struct csf_snap s;

	for (int i = 0, n = rand_r(&seed) % 50; i < n; i++) {
		churn(&seed);
		csf_thread_snap_of(pg, &s, 1);
		if (s.p.tid != (uint32_t)gettid())
			DIE("wrong page");
		/* Give it away, then revive by opening again. */
		if (rand_r(&seed) % 16 == 0 && hand_off((void *)pg)) {
			churn(&seed);
			pg = map_new();
			atomic_fetch_add(&nr_revive, 1);
		}
		if (rand_r(&seed) % 32 == 0) {
			/* Extra opens of the same object. */
			void *extra = map_new();

			munmap(extra, t_page());
		}
	}
	/* Release it here (owner release), or let a reaper do it, maybe after we exit. */
	if (rand_r(&seed) % 3 == 0 || !hand_off((void *)pg))
		munmap((void *)pg, t_page());
	atomic_fetch_add(&nr_threads, 1);
	return NULL;
}

static void *reaper(void *arg)
{
	(void)arg;
	/* Drain until all spawners are done and the queue is empty. */
	while (!atomic_load(&stop_reapers) || handoff.n) {
		void *p = NULL;

		pthread_mutex_lock(&handoff.mu);
		if (handoff.n)
			p = handoff.q[--handoff.n];
		pthread_mutex_unlock(&handoff.mu);
		if (p)
			munmap(p, t_page());
		else
			t_sleep_ns(50000);
	}
	return NULL;
}

static void *spawner(void *arg)
{
	unsigned int seed = (unsigned int)(uintptr_t)arg;

	while (!atomic_load(&stop)) {
		pthread_t th[16];
		int n = 1 + rand_r(&seed) % 16;

		for (int i = 0; i < n; i++)
			if (pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)rand_r(&seed)))
				DIE("pthread_create");
		for (int i = 0; i < n; i++)
			pthread_join(th[i], NULL);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 20;
	pthread_t sp[8], rp[2];
	long base = t_refcnt();

	t_name = "stress";
	for (int i = 0; i < 2; i++)
		pthread_create(&rp[i], NULL, reaper, NULL);
	for (int i = 0; i < 8; i++)
		pthread_create(&sp[i], NULL, spawner, (void *)(uintptr_t)(i + 1));
	t_sleep_ns((uint64_t)secs * 1000000000ULL);
	atomic_store(&stop, 1);
	for (int i = 0; i < 8; i++)
		pthread_join(sp[i], NULL);
	atomic_store(&stop_reapers, 1);
	for (int i = 0; i < 2; i++)
		pthread_join(rp[i], NULL);

	fprintf(stderr, "  %ld threads, %ld handoffs, %ld revives\n", atomic_load(&nr_threads),
		atomic_load(&nr_handoff), atomic_load(&nr_revive));
	int ok = 0;
	for (int i = 0; i < 400 && !ok; i++) {
		ok = t_refcnt() == base;
		if (!ok)
			t_sleep_ns(5000000);
	}
	CHECK(ok, "refcnt %ld, baseline %ld", t_refcnt(), base);
	if (!ok) {
		static const char *const diag[] = {
			"grep . /sys/module/coarse_stat_fs/parameters/nr_*",
			"grep -c coarse_stat_fs /proc/self/maps /proc/[0-9]*/maps 2>/dev/null | grep -v ':0$'",
		};

		for (unsigned int i = 0; i < sizeof(diag) / sizeof(diag[0]); i++)
			/* NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c): diagnostics */
			if (system(diag[i]) < 0)
				perror("system");
	}
	return t_done("t_stress");
}
