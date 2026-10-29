// SPDX-License-Identifier: GPL-2.0
/* thread-self lifetime. After each scenario the module refcount must return to its baseline. */
#include "t.h"

#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* Objects are freed after an RCU grace period: poll. */
static int wait_refcnt(long want)
{
	for (int i = 0; i < 400; i++) {
		if (t_refcnt() == want)
			return 1;
		t_sleep_ns(5000000);
	}
	return 0;
}

#define CHECK_REFCNT(want) \
	CHECK(wait_refcnt(want), "refcnt %ld, want %ld", t_refcnt(), (long)(want))

static const struct csf_thread_page *map_fd(int fd)
{
	void *p = mmap(NULL, t_page(), PROT_READ, MAP_SHARED, fd, 0);

	if (p == MAP_FAILED)
		DIE("mmap");
	return p;
}

static const struct csf_thread_page *raw_register(void)
{
	int fd = open(CSF_THREAD_SELF_PATH, O_RDONLY | O_CLOEXEC);
	const struct csf_thread_page *pg;

	if (fd < 0)
		DIE("open thread-self");
	pg = map_fd(fd);
	close(fd);
	return pg;
}

static void *lib_thread(void *arg)
{
	struct csf_snap s;

	(void)arg;
	if (csf_thread_snap(&s))
		DIE("snap");
	return NULL; /* the key destructor unmaps */
}

static void *leak_thread(void *arg)
{
	/* Exits with the mapping still in place; someone else unmaps it. */
	*(const struct csf_thread_page **)arg = raw_register();
	return NULL;
}

struct reviver {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int step;
	int fd;
	const struct csf_thread_page *pg;
	double slept_ms;
	int state_after;
};

static void step_wait(struct reviver *r, int step)
{
	pthread_mutex_lock(&r->mu);
	while (r->step < step)
		pthread_cond_wait(&r->cv, &r->mu);
	pthread_mutex_unlock(&r->mu);
}

static void step_set(struct reviver *r, int step)
{
	pthread_mutex_lock(&r->mu);
	r->step = step;
	pthread_cond_broadcast(&r->cv);
	pthread_mutex_unlock(&r->mu);
}

static void *revive_thread(void *arg)
{
	struct reviver *r = arg;
	struct csf_snap a, b;

	r->fd = open(CSF_THREAD_SELF_PATH, O_RDONLY | O_CLOEXEC);
	if (r->fd < 0)
		DIE("open");
	r->pg = map_fd(r->fd);
	step_set(r, 1);
	step_wait(r, 2);      /* main unmapped + closed: object is idle */
	t_sleep_ns(20000000); /* hooks must not account this */

	int fd = open(CSF_THREAD_SELF_PATH, O_RDONLY | O_CLOEXEC); /* revives */
	const struct csf_thread_page *pg = map_fd(fd);

	close(fd);
	r->state_after = pg->state;
	csf_thread_snap_of(pg, &a, 1);
	t_sleep_ns(30000000);
	csf_thread_snap_of(pg, &b, 1);
	r->slept_ms = t_diff(&a, &b).sleep;
	munmap((void *)pg, t_page()); /* owner release: unregisters */
	step_set(r, 3);
	return NULL;
}

static void *idle_exit_thread(void *arg)
{
	int *fdp = arg;

	*fdp = open(CSF_THREAD_SELF_PATH, O_RDONLY | O_CLOEXEC);
	if (*fdp < 0)
		DIE("open");
	return NULL; /* exits; main closes the fd later or first */
}

int main(int argc, char **argv)
{
	long base;

	if (argc > 1 && !strcmp(argv[1], "--exec-child")) {
		/* Fresh image after exec: registering again must work. */
		struct csf_snap s;

		t_name = "exec child";
		t_snap(&s);
		CHECK(s.p.tid == (uint32_t)gettid(), "tid");
		return t_failures ? 1 : 0;
	}

	base = t_refcnt();
	fprintf(stderr, "  baseline refcnt %ld\n", base);

	TEST("write access refused") {
		int fd = open(CSF_THREAD_SELF_PATH, O_RDWR);

		CHECK(fd < 0 && errno == EACCES, "O_RDWR: fd %d errno %d", fd, errno);
		fd = open(CSF_THREAD_SELF_PATH, O_RDONLY);
		if (fd < 0)
			DIE("open thread-self");
		void *p = mmap(NULL, t_page(), PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
		CHECK(p == MAP_FAILED, "writable private mapping allowed");
		p = mmap(NULL, 2 * t_page(), PROT_READ, MAP_SHARED, fd, 0);
		CHECK(p == MAP_FAILED, "two-page mapping allowed");
		struct stat st;
		CHECK(fstat(fd, &st) == 0 && (size_t)st.st_size == t_page(),
		      "st_size %lld, want one page", (long long)st.st_size);
		close(fd);
		CHECK_REFCNT(base);
	}

	TEST("owner close frees") {
		int fd = open(CSF_THREAD_SELF_PATH, O_RDONLY);

		CHECK(t_refcnt() == base + 2, "after open %ld", t_refcnt());
		close(fd);
		CHECK_REFCNT(base);
	}

	TEST("many opens share one object") {
		int fds[100];
		const struct csf_thread_page *p0, *p1;

		for (int i = 0; i < 100; i++)
			fds[i] = open(CSF_THREAD_SELF_PATH, O_RDONLY);
		CHECK(t_refcnt() == base + 1 + 100, "refcnt %ld", t_refcnt());
		p0 = map_fd(fds[0]);
		p1 = map_fd(fds[99]);
		t_sleep_ns(1000000);
		int same = p0->seq == p1->seq && p0->oncpu_cyc == p1->oncpu_cyc &&
			   p0->nr_switch_in == p1->nr_switch_in;
		CHECK(same, "pages differ");
		for (int i = 0; i < 100; i++)
			close(fds[i]);
		CHECK(t_refcnt() == base + 1 + 2, "each mapping holds its file: %ld", t_refcnt());
		munmap((void *)p0, t_page());
		munmap((void *)p1, t_page());
		CHECK_REFCNT(base);
	}

	TEST("thread churn via lib (owner munmap at exit)") {
		for (int round = 0; round < 20; round++) {
			pthread_t th[100];

			for (int i = 0; i < 100; i++)
				if (pthread_create(&th[i], NULL, lib_thread, NULL))
					DIE("pthread_create");
			for (int i = 0; i < 100; i++)
				pthread_join(th[i], NULL);
		}
		CHECK_REFCNT(base);
	}

	TEST("threads exit with mapping; non-owner munmap after death") {
		enum { N = 500 };
		static const struct csf_thread_page *pg[N];
		pthread_t th[N];

		for (int i = 0; i < N; i++)
			pthread_create(&th[i], NULL, leak_thread, (void *)&pg[i]);
		for (int i = 0; i < N; i++)
			pthread_join(th[i], NULL);
		t_sleep_ns(20000000);
		int dead = 0;
		for (int i = 0; i < N; i++)
			dead += pg[i]->state == CSF_ST_DEAD;
		CHECK(dead == N, "only %d/%d pages DEAD", dead, N);
		CHECK_REFCNT(base + 2L * N); /* object + mapped file each */
		for (int i = 0; i < N; i++)
			munmap((void *)pg[i], t_page());
		CHECK_REFCNT(base);
	}

	TEST("non-owner release idles; owner reopen revives") {
		struct reviver r = { .mu = PTHREAD_MUTEX_INITIALIZER,
				     .cv = PTHREAD_COND_INITIALIZER };
		pthread_t th;

		pthread_create(&th, NULL, revive_thread, &r);
		step_wait(&r, 1);
		munmap((void *)r.pg, t_page());
		close(r.fd);
		CHECK_REFCNT(base + 1); /* idle object, still registered */
		step_set(&r, 2);
		step_wait(&r, 3);
		pthread_join(th, NULL);
		CHECK(r.state_after == CSF_ST_RUNNING, "state after revive %d", r.state_after);
		CHECK(r.slept_ms > 25 && r.slept_ms < 60, "revived page slept %.2f ms", r.slept_ms);
		CHECK_REFCNT(base);
	}

	TEST("idle object freed at owner death") {
		int fd = -1;
		pthread_t th;

		pthread_create(&th, NULL, idle_exit_thread, &fd);
		pthread_join(th, NULL); /* owner dead, file still open */
		/* Notifier ref dropped at death; the open file keeps the object. */
		CHECK_REFCNT(base + 2);
		close(fd); /* non-owner, after death */
		CHECK_REFCNT(base);
	}

	TEST("fork: child does not inherit the page") {
		const struct csf_thread_page *pg = csf_thread_page();

		if (!pg)
			DIE("register");
		pid_t pid = fork();
		if (pid == 0) {
			unsigned char vec;
			struct csf_snap s;
			int bad = 0;

			/* mincore fails with ENOMEM on unmapped addresses. */
			if (mincore((void *)pg, t_page(), &vec) == 0 || errno != ENOMEM)
				bad |= 1;
			if (csf_thread_snap(&s) || s.p.tid != (uint32_t)getpid())
				bad |= 2;
			_exit(bad);
		}
		int st;
		waitpid(pid, &st, 0);
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child status %#x", st);
		struct csf_snap a, b;
		t_snap(&a);
		t_sleep_ns(5000000);
		t_snap(&b);
		CHECK(t_diff(&a, &b).sleep > 4, "parent page stopped updating");
	}

	TEST("exec") {
		pid_t pid = fork();

		if (pid == 0) {
			struct csf_snap s;

			if (csf_thread_snap(&s))
				_exit(3);
			execl("/proc/self/exe", argv[0], "--exec-child", (char *)NULL);
			_exit(4);
		}
		int st;
		waitpid(pid, &st, 0);
		CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "exec child status %#x", st);
	}

	/* The lib's own registration for this thread is still mapped. */
	TEST("final refcount") {
		long want = base + 2;

		CHECK_REFCNT(want);
	}

	return t_done("t_life");
}
