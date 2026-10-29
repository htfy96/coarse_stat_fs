// SPDX-License-Identifier: GPL-2.0
/* thread-self accounting. Pass a block device to enable the O_DIRECT iowait case. */
#include "t.h"

#include <pthread.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* The invariant: buckets partition wall time, to the cycle. */
static struct t_delta check_partition(const char *what, const struct csf_snap *a,
				      const struct csf_snap *b)
{
	struct t_delta d = t_diff(a, b);

	t_print(what, a, b);
	CHECK(a->extrapolated && b->extrapolated, "%s: not extrapolated", what);
	CHECK(d.sum_cyc == d.wall_cyc, "%s: buckets %llu != wall %llu cycles", what,
	      (unsigned long long)d.sum_cyc, (unsigned long long)d.wall_cyc);
	return d;
}

static unsigned long long proc_field(const char *file, const char *key)
{
	unsigned long long v = ~0ULL;
	char path[64], *line = NULL;
	size_t cap = 0;
	int n;
	FILE *f;

	n = snprintf(path, sizeof(path), "/proc/thread-self/%s", file);
	if (n < 0 || (size_t)n >= sizeof(path))
		DIE("path too long: %s", file);
	f = fopen(path, "re");
	if (!f)
		DIE("open %s", path);
	/* getline: /proc lines have no length limit (Cpus_allowed_list etc.) */
	while (getline(&line, &cap, f) >= 0)
		if (!strncmp(line, key, strlen(key)))
			sscanf(line + strlen(key), " %llu", &v);
	free(line);
	fclose(f);
	return v;
}

static void schedstat(unsigned long long v[3])
{
	FILE *f = fopen("/proc/thread-self/schedstat", "re");

	if (!f || fscanf(f, "%llu %llu %llu", &v[0], &v[1], &v[2]) != 3)
		DIE("schedstat");
	fclose(f);
}

struct spinner {
	pthread_t th;
	uint64_t ns;
	struct csf_snap a, b;
	pthread_barrier_t *bar;
};

static void *spin_thread(void *arg)
{
	struct spinner *s = arg;

	t_pin(0);
	pthread_barrier_wait(s->bar);
	t_snap(&s->a);
	t_spin_ns(s->ns);
	t_snap(&s->b);
	return NULL;
}

struct watch {
	const struct csf_thread_page *pg;
	volatile int stop;
	int saw_sleep, saw_running;
};

static void *watch_thread(void *arg)
{
	struct watch *w = arg;
	struct csf_snap s;

	t_pin(0); /* the watched thread is pinned to CPU 1 */
	while (!w->stop) {
		csf_thread_snap_of(w->pg, &s, 0);
		if (s.p.state == CSF_ST_SLEEP)
			w->saw_sleep = 1;
		if (s.p.state == CSF_ST_RUNNING)
			w->saw_running = 1;
		t_sleep_ns(1000000);
	}
	return NULL;
}

/* vfork's parent blocks killable (D) until the child exits. */
static const struct timespec vfork_child_sleep = { 0, 60000000 };

static __attribute__((noinline)) struct t_delta vfork_case(void)
{
	struct csf_snap a, b;
	pid_t pid;

	t_snap(&a);
	/* NOLINTBEGIN: vfork is the point; the child only makes raw syscalls. */
	pid = vfork();
	if (pid == 0) {
		syscall(SYS_nanosleep, &vfork_child_sleep, NULL);
		_exit(0);
	}
	/* NOLINTEND */
	t_snap(&b);
	waitpid(pid, NULL, 0);
	return check_partition("vfork", &a, &b);
}

int main(int argc, char **argv)
{
	struct csf_snap a, b;
	struct t_delta d;
	const struct csf_thread_page *pg;

	pg = csf_thread_page();
	if (!pg)
		DIE("register");

	TEST("header") {
		CHECK(pg->hdr.kind == CSF_KIND_THREAD, "kind %u", pg->hdr.kind);
		CHECK(pg->tid == (uint32_t)gettid(), "tid %u vs %d", pg->tid, gettid());
		CHECK(pg->state == CSF_ST_RUNNING, "state %u", pg->state);
		CHECK(pg->hdr.flags & CSF_F_TSC_CONSTANT, "flags %#x", pg->hdr.flags);
		fprintf(stderr, "    hdr.flags %#x tsc_khz %llu\n", pg->hdr.flags,
			(unsigned long long)pg->hdr.tsc_khz);
	}

	TEST("csf_format_diff fits CSF_DIFF_BUFSZ") {
		/* Widest possible output: every difference is UINT64_MAX. */
		struct csf_snap lo, hi;
		char buf[CSF_DIFF_BUFSZ];
		int n;

		memset(&lo, 0, sizeof(lo));
		memset(&hi, 0xff, sizeof(hi));
		hi.p.hdr.cyc2ns_mult = 1; /* 1 cycle = 1 ns: the widest times */
		hi.p.hdr.cyc2ns_shift = 0;
		n = csf_format_diff(buf, sizeof(buf), &lo, &hi);
		CHECK(n > 0 && n < (int)sizeof(buf), "needs %d bytes", n);
		fprintf(stderr, "    widest diff: %d bytes\n", n);
		/* volatile: GCC would reject the deliberate truncation. */
		volatile size_t small = 8;
		n = csf_format_diff(buf, small, &lo, &hi);
		CHECK(n >= 8 && strlen(buf) == 7, "truncation: n %d len %zu", n, strlen(buf));
	}

	TEST("no switch: pure extrapolation") {
		t_snap(&a);
		for (volatile int i = 0; i < 1000; i++)
			;
		t_snap(&b);
		d = t_diff(&a, &b);
		if (b.p.nr_switch_in == a.p.nr_switch_in)
			CHECK(b.p.oncpu_cyc - a.p.oncpu_cyc == b.tsc - a.tsc, "oncpu != wall");
		CHECK(d.sum_cyc == d.wall_cyc, "partition");
	}

	TEST("spin") {
		uint64_t c0 = t_now_ns(CLOCK_THREAD_CPUTIME_ID), c1;

		t_snap(&a);
		t_spin_ns(200000000);
		t_snap(&b);
		c1 = t_now_ns(CLOCK_THREAD_CPUTIME_ID);
		d = check_partition("spin", &a, &b);
		CHECK(d.oncpu > 0.9 * d.wall, "oncpu %.2f of wall %.2f", d.oncpu, d.wall);
		/* Cross-check against the kernel's own thread CPU clock. */
		double cpu_ms = (c1 - c0) / 1e6;
		CHECK(d.oncpu > cpu_ms * 0.97 && d.oncpu < cpu_ms * 1.03 + 1,
		      "oncpu %.3f vs CLOCK_THREAD_CPUTIME_ID %.3f", d.oncpu, cpu_ms);
	}

	TEST("sleep") {
		t_snap(&a);
		t_sleep_ns(100000000);
		t_snap(&b);
		d = check_partition("sleep", &a, &b);
		CHECK(d.sleep > 95 && d.sleep < 150, "sleep %.2f", d.sleep);
		CHECK(d.oncpu < 5, "oncpu %.2f", d.oncpu);
		CHECK(b.p.nr_block > a.p.nr_block, "nr_block did not grow");
	}

	TEST("dsleep (vfork parent waits killable)") {
		d = vfork_case();
		/* Host steal while on CPU shows up as oncpu (see README). */
		CHECK(d.dsleep > 40 && d.dsleep + d.oncpu > 55, "dsleep %.2f oncpu %.2f", d.dsleep,
		      d.oncpu);
		CHECK(d.sleep < 5, "sleep %.2f", d.sleep);
	}

	if (argc > 1) {
		TEST("iowait (O_DIRECT reads)") {
			enum { DIO_BLK = 4096 }; /* a multiple of any logical block size */
			int fd = open(argv[1], O_RDONLY | O_DIRECT);
			void *buf;

			if (fd < 0 || posix_memalign(&buf, DIO_BLK, DIO_BLK))
				DIE("open %s", argv[1]);
			off_t size = lseek(fd, 0, SEEK_END);
			t_snap(&a);
			for (int i = 0; i < 300; i++) {
				off_t off = ((off_t)random() % (size / DIO_BLK)) * DIO_BLK;

				if (pread(fd, buf, DIO_BLK, off) != DIO_BLK)
					DIE("pread");
			}
			t_snap(&b);
			close(fd);
			free(buf);
			d = check_partition("iowait", &a, &b);
			CHECK(d.iowait > 1 && d.iowait > d.sleep + d.dsleep,
			      "iowait %.3f sleep %.3f dsleep %.3f", d.iowait, d.sleep, d.dsleep);
			/* warm fields are as of the last switch-out: the last read may not have blocked */
			CHECK(b.p.syscr - a.p.syscr >= 299, "syscr %llu",
			      (unsigned long long)(b.p.syscr - a.p.syscr));
		}
	}

	TEST("contention: two spinners on one CPU") {
		pthread_barrier_t bar;
		struct spinner s[2] = { { .ns = 300000000, .bar = &bar },
					{ .ns = 300000000, .bar = &bar } };

		pthread_barrier_init(&bar, NULL, 2);
		for (int i = 0; i < 2; i++)
			pthread_create(&s[i].th, NULL, spin_thread, &s[i]);
		for (int i = 0; i < 2; i++)
			pthread_join(s[i].th, NULL);
		for (int i = 0; i < 2; i++) {
			d = check_partition("contend", &s[i].a, &s[i].b);
			/* Each wants 300 ms of CPU; the other is running half the time. */
			CHECK(d.runq > 0.3 * d.wall, "runq %.2f wall %.2f", d.runq, d.wall);
			CHECK(d.oncpu > 0.3 * d.wall, "oncpu %.2f wall %.2f", d.oncpu, d.wall);
			CHECK(s[i].b.p.nr_preempt > s[i].a.p.nr_preempt, "no preemptions");
		}
	}

	TEST("migration") {
		t_pin(0);
		t_sleep_ns(1000000);
		t_snap(&a);
		t_pin(1);
		t_sleep_ns(1000000);
		t_snap(&b);
		CHECK(b.p.cpu == 1, "cpu %u", b.p.cpu);
		CHECK(b.p.nr_migrate > a.p.nr_migrate, "nr_migrate did not grow");
		check_partition("migrate", &a, &b);
	}

	TEST("cross-check /proc/thread-self") {
		unsigned long long ss[3];

		t_sleep_ns(1000000); /* fresh switch-out values */
		t_snap(&a);
		unsigned long long nvcsw = proc_field("status", "voluntary_ctxt_switches:");
		unsigned long long nivcsw = proc_field("status", "nonvoluntary_ctxt_switches:");
		schedstat(ss);
		/* Reading /proc can itself switch, so allow a little slack. */
		CHECK(nvcsw >= a.p.nvcsw && nvcsw - a.p.nvcsw <= 2, "nvcsw %llu page %llu", nvcsw,
		      (unsigned long long)a.p.nvcsw);
		CHECK(nivcsw >= a.p.nivcsw && nivcsw - a.p.nivcsw <= 2, "nivcsw %llu page %llu",
		      nivcsw, (unsigned long long)a.p.nivcsw);
		CHECK(ss[2] >= a.p.pcount && ss[2] - a.p.pcount <= 2, "pcount %llu page %llu",
		      ss[2], (unsigned long long)a.p.pcount);
		CHECK(ss[1] >= a.p.run_delay_ns, "run_delay %llu page %llu", ss[1],
		      (unsigned long long)a.p.run_delay_ns);
		CHECK(ss[0] >= a.p.sum_exec_ns, "sum_exec %llu page %llu", ss[0],
		      (unsigned long long)a.p.sum_exec_ns);
	}

	TEST("watcher reads a sibling's page") {
		struct watch w = { .pg = pg };
		pthread_t th;

		t_pin(1);
		pthread_create(&th, NULL, watch_thread, &w);
		t_sleep_ns(50000000);
		t_spin_ns(50000000);
		w.stop = 1;
		pthread_join(th, NULL);
		CHECK(w.saw_sleep && w.saw_running, "sleep %d running %d", w.saw_sleep,
		      w.saw_running);
	}

	TEST("faults") {
		size_t len = 64 << 20;
		char *m =
			mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		madvise(m, len, MADV_NOHUGEPAGE); /* one fault per page */
		t_snap(&a);
		for (size_t i = 0; i < len; i += t_page())
			m[i] = 1;
		t_sleep_ns(1000000); /* warm fields are copied at switch-out */
		t_snap(&b);
		munmap(m, len);
		CHECK(b.p.min_flt - a.p.min_flt >= 1000, "min_flt %llu",
		      (unsigned long long)(b.p.min_flt - a.p.min_flt));
	}

	return t_done("t_thread");
}
