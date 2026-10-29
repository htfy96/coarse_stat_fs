// SPDX-License-Identifier: GPL-2.0
/* coarse_stat_fs: module setup and the pseudo filesystem. */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/clocksource.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/kobject.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/stringify.h>
#include <linux/sysfs.h>
#include <asm/cpufeature.h>
#include <asm/tsc.h>

#include "coarse_stat_fs.h"

#define CSF_PERIOD_US_DEFAULT 10000 /* 10 ms, the /proc/stat tick granularity */
#define CSF_PERIOD_US_MIN     1000
#define CSF_PERIOD_US_MAX     1000000

unsigned int csf_period_us = CSF_PERIOD_US_DEFAULT;

static int csf_set_period(const char *val, const struct kernel_param *kp)
{
	unsigned int v;
	int ret = kstrtouint(val, 0, &v);

	if (ret)
		return ret;
	if (v < CSF_PERIOD_US_MIN || v > CSF_PERIOD_US_MAX)
		return -ERANGE;
	WRITE_ONCE(csf_period_us, v);
	return 0;
}

static const struct kernel_param_ops csf_period_ops = {
	.set = csf_set_period,
	.get = param_get_uint,
};
module_param_cb(period_us, &csf_period_ops, &csf_period_us, 0644);
#define CSF_PERIOD_US_RANGE __stringify(CSF_PERIOD_US_MIN) "-" __stringify(CSF_PERIOD_US_MAX)
MODULE_PARM_DESC(period_us, "cpu page refresh period in microseconds (" CSF_PERIOD_US_RANGE ")");

#define CSF_CYC2NS_MAXSEC (60 * 60)

static u32 csf_cyc2ns_mult, csf_cyc2ns_shift, csf_tsc_flags;

void csf_init_hdr(struct csf_hdr *hdr, enum csf_kind kind, u32 size, u64 features)
{
	hdr->magic = CSF_MAGIC;
	hdr->abi_major = CSF_ABI_MAJOR;
	hdr->kind = kind;
	hdr->size = size;
	hdr->flags = csf_tsc_flags;
	if (!check_tsc_unstable() && (csf_tsc_flags & CSF_F_TSC_CONSTANT))
		hdr->flags |= CSF_F_TSC_SYNCED;
	hdr->features = features;
	hdr->tsc_khz = tsc_khz;
	hdr->cyc2ns_mult = csf_cyc2ns_mult;
	hdr->cyc2ns_shift = csf_cyc2ns_shift;
}

int csf_check_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_WRITE)
		return -EPERM;
	if (vma->vm_pgoff ||
	    vma->vm_end - vma->vm_start > PAGE_ALIGN(i_size_read(file_inode(file))))
		return -EINVAL;
	return 0;
}

u64 csf_ns_to_cyc(u64 ns)
{
	return mul_u64_u32_div(ns, tsc_khz, NSEC_PER_MSEC);
}

static int csf_fill_super(struct super_block *sb, struct fs_context *fc)
{
	static const struct tree_descr files[] = {
		[2] = { "cpu", &csf_cpu_fops, 0444 },
		[3] = { "thread-self", &csf_thread_fops, 0444 },
		{ "" },
	};

	return simple_fill_super(sb, CSF_SUPER_MAGIC, files);
}

static int csf_get_tree(struct fs_context *fc)
{
	return get_tree_single(fc, csf_fill_super);
}

static const struct fs_context_operations csf_context_ops = {
	.get_tree = csf_get_tree,
};

static int csf_init_fs_context(struct fs_context *fc)
{
	fc->ops = &csf_context_ops;
	return 0;
}

static struct file_system_type csf_fs_type = {
	.owner = THIS_MODULE,
	.name = CSF_FS_NAME,
	.init_fs_context = csf_init_fs_context,
	.kill_sb = csf_kill_sb,
};

static int __init coarse_stat_fs_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct csf_hdr) != 64);
	BUILD_BUG_ON(offsetof(struct csf_thread_page, seq) != 64);
	BUILD_BUG_ON(offsetof(struct csf_thread_page, nr_switch_in) != 128);
	BUILD_BUG_ON(sizeof(struct csf_thread_page) > PAGE_SIZE);
	BUILD_BUG_ON(sizeof(struct csf_cpu_row) != 128);
	BUILD_BUG_ON(sizeof(struct csf_cpu_page) != 128);

	if (!boot_cpu_has(X86_FEATURE_TSC) || !tsc_khz) {
		pr_err("no usable TSC\n");
		return -ENODEV;
	}
	if (boot_cpu_has(X86_FEATURE_CONSTANT_TSC) && boot_cpu_has(X86_FEATURE_NONSTOP_TSC))
		csf_tsc_flags |= CSF_F_TSC_CONSTANT;
	else
		pr_warn("TSC is not constant/nonstop; cycle buckets may drift\n");
	/*
	 * maxsec bounds the conversion range against 64-bit overflow. Readers
	 * multiply in 128 bits, so it only trades range for precision.
	 */
	clocks_calc_mult_shift(&csf_cyc2ns_mult, &csf_cyc2ns_shift, tsc_khz * 1000ULL, NSEC_PER_SEC,
			       CSF_CYC2NS_MAXSEC);

	ret = csf_cpu_init();
	if (ret)
		return ret;

	/* Never decremented while loaded: static_branch_dec may sleep. */
	preempt_notifier_inc();

	ret = sysfs_create_mount_point(kernel_kobj, CSF_FS_NAME);
	if (ret)
		goto err_notifier;
	ret = register_filesystem(&csf_fs_type);
	if (ret)
		goto err_mnt;

	pr_info("loaded (tsc %u kHz, flags %#x)\n", tsc_khz, csf_tsc_flags);
	return 0;

err_mnt:
	sysfs_remove_mount_point(kernel_kobj, CSF_FS_NAME);
err_notifier:
	preempt_notifier_dec();
	csf_cpu_exit();
	return ret;
}

static void __exit coarse_stat_fs_exit(void)
{
	unregister_filesystem(&csf_fs_type);
	sysfs_remove_mount_point(kernel_kobj, CSF_FS_NAME);
	/*
	 * Every thread object holds a module reference until its free is
	 * queued, so none are live; wait for the queued frees to finish.
	 */
	rcu_barrier();
	preempt_notifier_dec();
	csf_cpu_exit();
	pr_info("unloaded\n");
}

module_init(coarse_stat_fs_init);
module_exit(coarse_stat_fs_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Syscall-free mmap-able scheduler and CPU accounting");
MODULE_AUTHOR("Vic Luo");
