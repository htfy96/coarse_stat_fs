/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Fallbacks for older kernels, back to 6.1 (Debian bookworm). Each block
 * names the release that introduced the real interface.
 */
#ifndef _COARSE_STAT_FS_COMPAT_H
#define _COARSE_STAT_FS_COMPAT_H

#include <linux/fs.h>
#include <linux/kernel_stat.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/tick.h>
#include <linux/version.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
static inline void vm_flags_mod(struct vm_area_struct *vma, vm_flags_t set, vm_flags_t clear)
{
	vma->vm_flags = (vma->vm_flags | set) & ~clear;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
/* Without delayed dequeue, being on the runqueue means runnable. */
static inline bool task_is_runnable(struct task_struct *p)
{
	return p->on_rq;
}
#endif

/*
 * simple_fill_super() pins its dentries. Until 6.19 (d_make_persistent())
 * such a superblock had to be killed with kill_litter_super(), or umount
 * warns that the dentries are still in use.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 19, 0)
#define csf_kill_sb kill_litter_super
#else
#define csf_kill_sb kill_anon_super
#endif

#ifndef kzalloc_obj
#define kzalloc_obj(var, gfp) kzalloc(sizeof(var), gfp)
#endif

/*
 * Before 7.2, kcpustat_cpu_fetch() left NO_HZ idle time out of idle and
 * iowait, and fs/proc/stat.c added it back; do what it did.
 */
static inline void csf_kcpustat_fetch(struct kernel_cpustat *kcs, int cpu)
{
	kcpustat_cpu_fetch(kcs, cpu);
#if LINUX_VERSION_CODE < KERNEL_VERSION(7, 2, 0)
	if (cpu_online(cpu)) {
		u64 idle = get_cpu_idle_time_us(cpu, NULL);
		u64 iowait = get_cpu_iowait_time_us(cpu, NULL);

		if (idle != -1ULL)
			kcs->cpustat[CPUTIME_IDLE] = idle * NSEC_PER_USEC;
		if (iowait != -1ULL)
			kcs->cpustat[CPUTIME_IOWAIT] = iowait * NSEC_PER_USEC;
	}
#endif
}

#endif
