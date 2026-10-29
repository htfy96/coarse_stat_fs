/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _COARSE_STAT_FS_INTERNAL_H
#define _COARSE_STAT_FS_INTERNAL_H

#include <linux/compiler.h>
#include <linux/fs.h>
#include <uapi/coarse_stat_fs.h>

#include "compat.h"

#if !defined(CONFIG_X86_64) || !defined(CONFIG_PREEMPT_NOTIFIERS) || !defined(CONFIG_SCHED_INFO)
#error "coarse_stat_fs needs x86-64 with CONFIG_PREEMPT_NOTIFIERS and CONFIG_SCHED_INFO"
#endif

extern const struct file_operations csf_cpu_fops;
extern const struct file_operations csf_thread_fops;

extern unsigned int csf_period_us;

void csf_init_hdr(struct csf_hdr *hdr, enum csf_kind kind, u32 size, u64 features);

/* The open handlers set i_size to the mappable size. */
int csf_check_mmap(struct file *file, struct vm_area_struct *vma);

u64 csf_ns_to_cyc(u64 ns);

int csf_cpu_init(void);
void csf_cpu_exit(void);

/*
 * Single-writer seqcount on a user-visible u32. The barriers pair with the
 * reader's acquire load of seq and acquire fence around its copy in
 * lib/coarse_stat_fs.h.
 */
static __always_inline void csf_write_begin(u32 *seq)
{
	WRITE_ONCE(*seq, *seq + 1);
	smp_wmb(); /* odd seq is visible before any data store */
}

static __always_inline void csf_write_end(u32 *seq)
{
	smp_wmb(); /* data stores are visible before the even seq */
	WRITE_ONCE(*seq, *seq + 1);
}

#endif
