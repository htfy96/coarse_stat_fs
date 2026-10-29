/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * coarse_stat_fs: syscall-free, mmap-able scheduler/CPU accounting.
 *
 *   cpu          mmap: struct csf_cpu_page, /proc/stat equivalent, refreshed
 *                every period_ns by the kernel while anyone has it open.
 *   thread-self  open() registers the calling thread; mmap: its
 *                struct csf_thread_page, updated at every context switch.
 *
 * Mapping: fstat() an open file; st_size is the length to mmap, at offset 0.
 *
 * ABI rules: fields are only ever appended; hdr.size says how many bytes are
 * valid; an incompatible change bumps CSF_ABI_MAJOR. All pages are read-only.
 *
 * Consistency: readers copy fields between two loads of `seq` and retry if
 * it was odd or changed (see lib/coarse_stat_fs.h).
 */
#ifndef _UAPI_LINUX_COARSE_STAT_FS_H
#define _UAPI_LINUX_COARSE_STAT_FS_H

#include <linux/types.h>

#define CSF_FS_NAME     "coarse_stat_fs"
#define CSF_MOUNT_POINT "/sys/kernel/coarse_stat_fs"

/* csf_hdr.magic and statfs(2) f_type: the ASCII bytes "CSFS". */
#define CSF_MAGIC       0x43534653
#define CSF_SUPER_MAGIC CSF_MAGIC
#define CSF_ABI_MAJOR   1

enum csf_kind {
	CSF_KIND_THREAD = 1,
	CSF_KIND_CPU = 2,
};

/* csf_hdr.flags: static properties of the clock, fixed at page creation. */
#define CSF_F_TSC_CONSTANT (1U << 0) /* constant_tsc && nonstop_tsc */
#define CSF_F_TSC_SYNCED   (1U << 1) /* TSC comparable across CPUs */

/* Shared 64-byte header at offset 0 of every page. Written once. */
struct csf_hdr {
	__u32 magic;     /* CSF_MAGIC */
	__u16 abi_major; /* CSF_ABI_MAJOR */
	__u16 kind;      /* enum csf_kind */
	__u32 size;      /* bytes of valid layout */
	__u32 flags;     /* CSF_F_* */
	__u64 features;  /* CSF_FEAT_* */
	__u64 tsc_khz;
	/* ns = (cycles * cyc2ns_mult) >> cyc2ns_shift  (use 128-bit math) */
	__u32 cyc2ns_mult;
	__u32 cyc2ns_shift;
	__u64 _rsvd[3];
};

/* Why the thread last left the CPU (or RUNNING if it is on CPU). */
enum csf_thread_state {
	CSF_ST_RUNNING = 1,
	CSF_ST_RUNQ = 2,   /* still runnable: preempted, yielded */
	CSF_ST_SLEEP = 3,  /* S: interruptible sleep (epoll, futex, ...) */
	CSF_ST_DSLEEP = 4, /* D: uninterruptible, not I/O (locks, ...) */
	CSF_ST_IOWAIT = 5, /* D in io_schedule() */
	CSF_ST_DEAD = 6,   /* thread exited; values are final */
};

#define CSF_FEAT_THREAD_BASE (1ULL << 0) /* everything up to tid */

/*
 * Time buckets are TSC cycles accumulated up to last_switch_tsc. They
 * partition wall time: their sum always equals last_switch_tsc - the
 * registration time. To get "now" values, add (rdtsc() - last_switch_tsc)
 * to the bucket named by `state` (oncpu_cyc when RUNNING).
 *
 * Off-CPU time while blocked is split using the kernel's run_delay: the
 * part spent runnable-but-waiting after the wakeup goes to runq_cyc, the
 * rest to the bucket of the block reason. That split happens at switch-in,
 * so for a thread that is currently off CPU it is provisional.
 */
struct csf_thread_page {
	struct csf_hdr hdr; /* 0 */

	/* hot: written at every switch-in / switch-out */
	__u32 seq;   /* 64 */
	__u32 state; /* enum csf_thread_state */
	__u32 cpu;   /* CPU of last switch-in */
	__u32 tid;   /* in the opener's pid namespace */
	__u64 last_switch_tsc;
	__u64 oncpu_cyc;
	__u64 runq_cyc;
	__u64 sleep_cyc;
	__u64 dsleep_cyc;
	__u64 iowait_cyc;

	__u64 nr_switch_in; /* 128 */
	__u64 nr_preempt;   /* switch-outs while runnable */
	__u64 nr_block;     /* switch-outs while blocked */
	__u64 nr_migrate;   /* switch-ins on a different CPU */

	/* warm: copied from the task at switch-out (run_delay/pcount at in) */
	__u64 sum_exec_ns;  /* /proc/<tid>/schedstat field 1 */
	__u64 run_delay_ns; /* field 2 */
	__u64 pcount;       /* field 3 */
	__u64 nvcsw;
	__u64 nivcsw;   /* 192 */
	__u64 utime_ns; /* tick-sampled */
	__u64 stime_ns;
	__u64 min_flt; /* as of last switch-out */
	__u64 maj_flt;
	__u64 rchar;
	__u64 wchar;
	__u64 syscr;
	__u64 syscw; /* 256 */
	__u64 read_bytes;
	__u64 write_bytes;
	__u64 _rsvd0;
};

#define CSF_FEAT_CPU_BASE (1ULL << 0)

/*
 * All times in ns, same sources as /proc/stat. For /proc/stat's USER_HZ
 * ticks, divide by 1e9 / sysconf(_SC_CLK_TCK).
 */
struct csf_cpu_row {
	__u64 user;
	__u64 nice;
	__u64 system;
	__u64 idle;
	__u64 iowait;
	__u64 irq;
	__u64 softirq;
	__u64 steal;
	__u64 guest;
	__u64 guest_nice;
	__u64 irqs;     /* generic IRQs handled (excludes x86 LOC/NMI/...) */
	__u64 softirqs; /* all softirq vectors */
	__u32 online;
	__u32 _pad;
	__u64 _rsvd[3];
};

/*
 * rows[0] is the sum over all possible CPUs; rows[1 + cpu] is that CPU.
 * Use rows_off/row_size rather than sizeof() so rows can grow.
 */
struct csf_cpu_page {
	struct csf_hdr hdr; /* 0 */
	__u32 seq;          /* 64 */
	__u32 nr_rows;      /* 1 + nr_cpu_ids */
	__u32 row_size;
	__u32 rows_off;
	__u64 period_ns;
	__u64 update_tsc;     /* when the rows were last refreshed */
	__u64 update_mono_ns; /* CLOCK_MONOTONIC of the same */
	__u64 nr_updates;
	__u64 _rsvd[2];
	/* rows at rows_off (128) */
};

#endif /* _UAPI_LINUX_COARSE_STAT_FS_H */
