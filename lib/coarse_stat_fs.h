/* SPDX-License-Identifier: MIT */
/*
 * Header-only reader for coarse_stat_fs. The first csf_thread_snap() in a
 * thread registers it by mapping thread-self. A pthread key destructor drops
 * the mapping at thread exit, and fork children start unregistered.
 */
#ifndef COARSE_STAT_FS_LIB_H
#define COARSE_STAT_FS_LIB_H

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <x86intrin.h>

#include <uapi/coarse_stat_fs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CSF_CPU_PATH         CSF_MOUNT_POINT "/cpu"
#define CSF_THREAD_SELF_PATH CSF_MOUNT_POINT "/thread-self"

struct csf_snap {
	uint64_t tsc;     /* when the snapshot was taken */
	int extrapolated; /* buckets include time up to tsc, so they sum to wall time */
	struct csf_thread_page p;
};

struct csf_cpu_snap {
	uint64_t update_tsc;
	uint64_t update_mono_ns;
	uint64_t nr_updates;
	struct csf_cpu_row row;
};

static inline uint64_t csf_rdtsc(void)
{
	unsigned int aux;

	return __rdtscp(&aux);
}

static inline uint64_t csf_cyc_to_ns(const struct csf_hdr *h, uint64_t cyc)
{
	return (uint64_t)(((unsigned __int128)cyc * h->cyc2ns_mult) >> h->cyc2ns_shift);
}

/* The valid layout must cover what the caller reads and fit in the mapping. */
static inline int csf__hdr_ok(const struct csf_hdr *h, enum csf_kind kind, size_t min, size_t len)
{
	return h->magic == CSF_MAGIC && h->abi_major == CSF_ABI_MAJOR && h->kind == kind &&
	       h->size >= min && h->size <= len;
}

/* Nanoseconds per USER_HZ tick, the unit of /proc/stat. */
static inline uint64_t csf_ns_per_tick(void)
{
	return 1000000000ULL / (uint64_t)sysconf(_SC_CLK_TCK);
}

/* The open handler sets st_size to the mappable length. */
static inline const void *csf__map(const char *path, size_t min, size_t *len)
{
	void *p = MAP_FAILED;
	struct stat st;
	int fd, err;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (fstat(fd, &st)) {
		err = errno;
	} else if (st.st_size < 0 || (size_t)st.st_size < min) {
		err = EPROTO;
	} else {
		*len = (size_t)st.st_size;
		p = mmap(NULL, *len, PROT_READ, MAP_SHARED, fd, 0);
		err = errno;
	}
	close(fd); /* the mapping keeps the kernel object alive */
	errno = err;
	return p == MAP_FAILED ? NULL : p;
}

static __thread const struct csf_thread_page *csf__tpage;
static __thread size_t csf__tlen;
static __thread int csf__terr;
static pthread_key_t csf__tkey;
static pthread_once_t csf__tonce = PTHREAD_ONCE_INIT;

static void csf__tdtor(void *p)
{
	csf__tpage = NULL;
	munmap(p, csf__tlen); /* owner releases: kernel unregisters */
}

static void csf__atfork_child(void)
{
	/* VM_DONTCOPY: the parent's page is not mapped in the child. */
	csf__tpage = NULL;
	csf__terr = 0;
	pthread_setspecific(csf__tkey, NULL);
}

static void csf__tinit(void)
{
	pthread_key_create(&csf__tkey, csf__tdtor);
	pthread_atfork(NULL, NULL, csf__atfork_child);
}

static __attribute__((noinline)) const struct csf_thread_page *csf__tregister(void)
{
	const struct csf_thread_page *pg;
	size_t len;

	if (csf__terr)
		return NULL;
	pthread_once(&csf__tonce, csf__tinit);
	pg = (const struct csf_thread_page *)csf__map(CSF_THREAD_SELF_PATH, sizeof(*pg), &len);
	if (!pg) {
		csf__terr = errno ? errno : EIO;
		return NULL;
	}
	if (!csf__hdr_ok(&pg->hdr, CSF_KIND_THREAD, sizeof(*pg), len)) {
		munmap((void *)pg, len);
		csf__terr = EPROTO;
		return NULL;
	}
	csf__tlen = len;
	pthread_setspecific(csf__tkey, pg);
	csf__tpage = pg;
	return pg;
}

/* This thread's page, registering on first use. NULL + errno on failure. */
static inline const struct csf_thread_page *csf_thread_page(void)
{
	const struct csf_thread_page *pg = csf__tpage;

	if (__builtin_expect(pg != NULL, 1))
		return pg;
	pg = csf__tregister();
	if (!pg)
		errno = csf__terr;
	return pg;
}

/* Forget a cached registration error (e.g. after mounting coarse_stat_fs). */
static inline void csf_thread_reset_error(void)
{
	csf__terr = 0;
}

static inline __u64 *csf__bucket(struct csf_thread_page *p, uint32_t state)
{
	switch (state) {
	case CSF_ST_RUNNING:
		return &p->oncpu_cyc;
	case CSF_ST_RUNQ:
		return &p->runq_cyc;
	case CSF_ST_SLEEP:
		return &p->sleep_cyc;
	case CSF_ST_DSLEEP:
		return &p->dsleep_cyc;
	case CSF_ST_IOWAIT:
		return &p->iowait_cyc;
	default:
		return NULL; /* DEAD: final */
	}
}

/*
 * Snapshot any thread page. For the calling thread's own page the
 * extrapolation is always valid; for another thread's page it is applied
 * only if the TSC is synchronized across CPUs.
 */
static inline void csf_thread_snap_of(const struct csf_thread_page *pg, struct csf_snap *s,
				      int self)
{
	const volatile uint32_t *seqp = &pg->seq;
	uint32_t seq;
	__u64 *b;

	for (;;) {
		seq = __atomic_load_n(seqp, __ATOMIC_ACQUIRE);
		if (__builtin_expect(seq & 1, 0)) {
			_mm_pause();
			continue;
		}
		memcpy(&s->p, pg, sizeof(s->p));
		s->tsc = csf_rdtsc();
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(seqp, __ATOMIC_RELAXED) == seq)
			break;
	}

	s->extrapolated = 0;
	if (!self && !(s->p.hdr.flags & CSF_F_TSC_SYNCED))
		return;
	b = csf__bucket(&s->p, s->p.state);
	if (b && s->tsc > s->p.last_switch_tsc) {
		*b += s->tsc - s->p.last_switch_tsc;
		s->extrapolated = 1;
	}
}

/* Snapshot the calling thread. Returns 0, or -1 with errno set. */
static inline int csf_thread_snap(struct csf_snap *s)
{
	const struct csf_thread_page *pg = csf_thread_page();

	if (__builtin_expect(pg == NULL, 0))
		return -1;
	csf_thread_snap_of(pg, s, 1);
	return 0;
}

static const struct csf_cpu_page *csf__cpage;
static int csf__cerr;
static pthread_once_t csf__conce = PTHREAD_ONCE_INIT;

static void csf__cinit(void)
{
	const struct csf_cpu_page *cp;
	size_t len;

	cp = (const struct csf_cpu_page *)csf__map(CSF_CPU_PATH, sizeof(*cp), &len);
	if (!cp) {
		csf__cerr = errno ? errno : EIO;
		return;
	}
	/* Every row must lie inside the valid layout: csf_cpu_snap() trusts them. */
	if (!csf__hdr_ok(&cp->hdr, CSF_KIND_CPU, sizeof(*cp), len) || cp->nr_rows < 1 ||
	    cp->row_size < sizeof(struct csf_cpu_row) ||
	    cp->rows_off + (size_t)cp->nr_rows * cp->row_size > cp->hdr.size) {
		munmap((void *)cp, len);
		csf__cerr = EPROTO;
		return;
	}
	csf__cpage = cp;
}

static inline const struct csf_cpu_page *csf_cpu_page(void)
{
	pthread_once(&csf__conce, csf__cinit);
	if (!csf__cpage)
		errno = csf__cerr;
	return csf__cpage;
}

#define CSF_CPU_ALL (-1)

/* Snapshot one CPU's row, or the sum with CSF_CPU_ALL. 0 or -1/errno. */
static inline int csf_cpu_snap(struct csf_cpu_snap *s, int cpu)
{
	const struct csf_cpu_page *cp = csf_cpu_page();
	const volatile uint32_t *seqp;
	const struct csf_cpu_row *row;
	uint32_t seq;

	if (!cp)
		return -1;
	if (cpu < -1 || (uint32_t)(cpu + 1) >= cp->nr_rows) {
		errno = EINVAL;
		return -1;
	}
	row = (const struct csf_cpu_row *)((const char *)cp + cp->rows_off +
					   (size_t)(cpu + 1) * cp->row_size);
	seqp = &cp->seq;
	for (;;) {
		seq = __atomic_load_n(seqp, __ATOMIC_ACQUIRE);
		if (__builtin_expect(seq & 1, 0)) {
			_mm_pause();
			continue;
		}
		s->update_tsc = cp->update_tsc;
		s->update_mono_ns = cp->update_mono_ns;
		s->nr_updates = cp->nr_updates;
		memcpy(&s->row, row, sizeof(s->row));
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(seqp, __ATOMIC_RELAXED) == seq)
			return 0;
	}
}

/*
 * Large enough for any csf_format_diff() output: 108 bytes of fixed text,
 * six "%.3f" values of at most 18 characters (a u64 of ns as ms), and nine
 * u64 counters of at most 20 digits, plus the NUL: 397 bytes. tests/t_thread.c
 * checks this with extreme values.
 */
#define CSF_DIFF_BUFSZ 512

/*
 * Formats the b - a difference, e.g.
 * "wall 120.3ms = cpu 30.1 + runq 5.0 + sleep 0.0 + dsleep 0.0 + iowait 85.2 | ...".
 * Like snprintf(), it never writes more than n bytes, always NUL-terminates
 * when n > 0, and returns the length the full text needs. A return value of
 * n or more means the text was truncated; CSF_DIFF_BUFSZ always fits.
 */
static inline int csf_format_diff(char *buf, size_t n, const struct csf_snap *a,
				  const struct csf_snap *b)
{
	const struct csf_hdr *h = &b->p.hdr;
#define CSF__MS(f) (csf_cyc_to_ns(h, b->p.f - a->p.f) / 1e6)
	return snprintf(buf, n,
			"wall %.3fms = cpu %.3f + runq %.3f + sleep %.3f"
			" + dsleep %.3f + iowait %.3f"
			" | preempt %llu block %llu migrate %llu"
			" | flt %llu/%llu | sysc r%llu w%llu | io r%lluB w%lluB",
			csf_cyc_to_ns(h, b->tsc - a->tsc) / 1e6, CSF__MS(oncpu_cyc),
			CSF__MS(runq_cyc), CSF__MS(sleep_cyc), CSF__MS(dsleep_cyc),
			CSF__MS(iowait_cyc),
			(unsigned long long)(b->p.nr_preempt - a->p.nr_preempt),
			(unsigned long long)(b->p.nr_block - a->p.nr_block),
			(unsigned long long)(b->p.nr_migrate - a->p.nr_migrate),
			(unsigned long long)(b->p.min_flt - a->p.min_flt),
			(unsigned long long)(b->p.maj_flt - a->p.maj_flt),
			(unsigned long long)(b->p.syscr - a->p.syscr),
			(unsigned long long)(b->p.syscw - a->p.syscw),
			(unsigned long long)(b->p.rchar - a->p.rchar),
			(unsigned long long)(b->p.wchar - a->p.wchar));
#undef CSF__MS
}

#ifdef __cplusplus
}
#endif

#endif /* COARSE_STAT_FS_LIB_H */
