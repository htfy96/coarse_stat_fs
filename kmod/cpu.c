// SPDX-License-Identifier: GPL-2.0
/* The "cpu" file. It is refreshed only while open; a mapping holds it open. */
#include <linux/cpumask.h>
#include <linux/kernel_stat.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/timekeeping.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <asm/tsc.h>

#include "coarse_stat_fs.h"

static struct csf_cpu_page *csf_cpu;
static size_t csf_cpu_size;
static DEFINE_MUTEX(csf_cpu_lock);
static unsigned int csf_cpu_users;

static void csf_cpu_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(csf_cpu_work, csf_cpu_work_fn);

static struct csf_cpu_row *csf_cpu_rows(void)
{
	return (void *)csf_cpu + csf_cpu->rows_off;
}

static void csf_row_add(struct csf_cpu_row *sum, const struct csf_cpu_row *r)
{
	sum->user += r->user;
	sum->nice += r->nice;
	sum->system += r->system;
	sum->idle += r->idle;
	sum->iowait += r->iowait;
	sum->irq += r->irq;
	sum->softirq += r->softirq;
	sum->steal += r->steal;
	sum->guest += r->guest;
	sum->guest_nice += r->guest_nice;
	sum->irqs += r->irqs;
	sum->softirqs += r->softirqs;
	sum->online += r->online;
}

/* Only ever called from the work item, so there is a single writer. */
static void csf_cpu_refresh(void)
{
	struct csf_cpu_row *rows = csf_cpu_rows(), sum = {};
	int cpu, j;

	csf_write_begin(&csf_cpu->seq);
	for_each_possible_cpu(cpu) {
		struct csf_cpu_row *r = &rows[1 + cpu];
		struct kernel_cpustat kcs;
		u64 *t = kcs.cpustat;
		u64 softirqs = 0;

		csf_kcpustat_fetch(&kcs, cpu);
		for (j = 0; j < NR_SOFTIRQS; j++)
			softirqs += kstat_softirqs_cpu(j, cpu);

		r->user = t[CPUTIME_USER];
		r->nice = t[CPUTIME_NICE];
		r->system = t[CPUTIME_SYSTEM];
		r->idle = t[CPUTIME_IDLE];
		r->iowait = t[CPUTIME_IOWAIT];
		r->irq = t[CPUTIME_IRQ];
		r->softirq = t[CPUTIME_SOFTIRQ];
		r->steal = t[CPUTIME_STEAL];
		r->guest = t[CPUTIME_GUEST];
		r->guest_nice = t[CPUTIME_GUEST_NICE];
		r->irqs = kstat_cpu_irqs_sum(cpu);
		r->softirqs = softirqs;
		r->online = cpu_online(cpu);
		csf_row_add(&sum, r);
	}
	rows[0] = sum;
	csf_cpu->period_ns = (u64)READ_ONCE(csf_period_us) * NSEC_PER_USEC;
	csf_cpu->update_tsc = rdtsc();
	csf_cpu->update_mono_ns = ktime_get_ns();
	csf_cpu->nr_updates++;
	csf_write_end(&csf_cpu->seq);
}

static void csf_cpu_work_fn(struct work_struct *work)
{
	csf_cpu_refresh();
	queue_delayed_work(system_unbound_wq, &csf_cpu_work,
			   usecs_to_jiffies(READ_ONCE(csf_period_us)));
}

static int csf_cpu_open(struct inode *inode, struct file *file)
{
	if (file->f_mode & FMODE_WRITE)
		return -EACCES;
	i_size_write(inode, csf_cpu_size);

	mutex_lock(&csf_cpu_lock);
	if (csf_cpu_users++ == 0) {
		/* Publish fresh data before the first reader can map it. */
		mod_delayed_work(system_unbound_wq, &csf_cpu_work, 0);
		flush_delayed_work(&csf_cpu_work);
	}
	mutex_unlock(&csf_cpu_lock);
	return 0;
}

static int csf_cpu_release(struct inode *inode, struct file *file)
{
	mutex_lock(&csf_cpu_lock);
	if (--csf_cpu_users == 0)
		cancel_delayed_work_sync(&csf_cpu_work);
	mutex_unlock(&csf_cpu_lock);
	return 0;
}

static int csf_cpu_mmap(struct file *file, struct vm_area_struct *vma)
{
	int ret = csf_check_mmap(file, vma);

	if (ret)
		return ret;
	vm_flags_mod(vma, VM_DONTDUMP, VM_MAYWRITE);
	return remap_vmalloc_range(vma, csf_cpu, 0);
}

const struct file_operations csf_cpu_fops = {
	.owner = THIS_MODULE,
	.open = csf_cpu_open,
	.release = csf_cpu_release,
	.mmap = csf_cpu_mmap,
	.llseek = noop_llseek,
};

int csf_cpu_init(void)
{
	u32 rows_off = sizeof(struct csf_cpu_page);
	u32 nr_rows = 1 + nr_cpu_ids;

	csf_cpu_size = rows_off + (size_t)nr_rows * sizeof(struct csf_cpu_row);
	csf_cpu = vmalloc_user(PAGE_ALIGN(csf_cpu_size));
	if (!csf_cpu)
		return -ENOMEM;

	csf_init_hdr(&csf_cpu->hdr, CSF_KIND_CPU, csf_cpu_size, CSF_FEAT_CPU_BASE);
	csf_cpu->nr_rows = nr_rows;
	csf_cpu->row_size = sizeof(struct csf_cpu_row);
	csf_cpu->rows_off = rows_off;
	return 0;
}

void csf_cpu_exit(void)
{
	/* No files are open (they hold module references), so no work. */
	cancel_delayed_work_sync(&csf_cpu_work);
	vfree(csf_cpu);
}
