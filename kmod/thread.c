// SPDX-License-Identifier: GPL-2.0
/*
 * The "thread-self" file.
 *
 * open() by a thread finds or creates that thread's object: one zeroed page
 * plus a preempt notifier on the thread. Every open gets its own file but
 * shares the object. The notifier's sched_out/sched_in hooks, which run only
 * for this thread, keep the page current.
 *
 * Lifetime. obj->ref counts open files plus one for the registered notifier.
 * The notifier list may only be modified by its owner, so:
 *  - last file released by the owner (munmap/close/exec/exit task_work):
 *    unregister the notifier right there;
 *  - last file released by anyone else: mark the object idle (hooks become
 *    no-ops); the owner's next open revives it, its death frees it;
 *  - the owner's final switch-out (TASK_DEAD): drop the notifier reference.
 *    The scheduler still walks the list after our hook returns, so objects
 *    are always freed via call_rcu (that walk runs with IRQs disabled).
 * At most one object per thread exists at any time.
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/preempt.h>
#include <linux/rcupdate.h>
#include <linux/refcount.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <asm/tsc.h>

#include "coarse_stat_fs.h"

struct csf_tobj {
	struct preempt_notifier pn;
	struct csf_thread_page *pg;
	struct page *page;
	/* Only compared with current, never dereferenced. */
	struct task_struct *owner;
	/* run_delay at the last switch-out; touched only by the hooks */
	u64 out_run_delay;
	bool idle;       /* hooks skip; written under lock, read locklessly */
	bool registered; /* notifier still on owner's list */
	refcount_t ref;
	struct mutex lock; /* users, idle, unregistering */
	unsigned int users;
	struct rcu_head rcu;
};

static struct preempt_ops csf_preempt_ops;

/* Read-only counters in /sys/module/coarse_stat_fs/parameters, for tests. */
static atomic_t csf_nr_objs, csf_nr_idle, csf_nr_files;

static int csf_get_atomic(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "%d\n", atomic_read((atomic_t *)kp->arg));
}

static const struct kernel_param_ops csf_atomic_ops = { .get = csf_get_atomic };
module_param_cb(nr_objs, &csf_atomic_ops, &csf_nr_objs, 0444);
module_param_cb(nr_idle, &csf_atomic_ops, &csf_nr_idle, 0444);
module_param_cb(nr_files, &csf_atomic_ops, &csf_nr_files, 0444);

static inline u64 csf_delta(u64 now, u64 then)
{
	return likely(now > then) ? now - then : 0;
}

static void csf_tobj_free_rcu(struct rcu_head *rcu)
{
	struct csf_tobj *obj = container_of(rcu, struct csf_tobj, rcu);

	__free_page(obj->page);
	kfree(obj);
}

/*
 * The module reference goes as soon as the free is queued, because with lazy
 * RCU the callback may run seconds later and rmmod shouldn't wait for it.
 * The rcu_barrier() in coarse_stat_fs_exit() covers the callback; call_rcu()
 * comes first so that the barrier is ordered after it.
 */
static void csf_tobj_put(struct csf_tobj *obj)
{
	if (!refcount_dec_and_test(&obj->ref))
		return;
	if (obj->idle)
		atomic_dec(&csf_nr_idle);
	atomic_dec(&csf_nr_objs);
	call_rcu(&obj->rcu, csf_tobj_free_rcu);
	module_put(THIS_MODULE);
}

/* Copies the counters that are read from the task at switch-out. */
static void csf_copy_warm(struct csf_thread_page *pg, struct task_struct *p)
{
	pg->sum_exec_ns = p->se.sum_exec_runtime;
	pg->run_delay_ns = p->sched_info.run_delay;
	pg->pcount = p->sched_info.pcount;
	pg->nvcsw = p->nvcsw;
	pg->nivcsw = p->nivcsw;
	pg->utime_ns = p->utime;
	pg->stime_ns = p->stime;
	pg->min_flt = p->min_flt;
	pg->maj_flt = p->maj_flt;
#ifdef CONFIG_TASK_XACCT
	pg->rchar = p->ioac.rchar;
	pg->wchar = p->ioac.wchar;
	pg->syscr = p->ioac.syscr;
	pg->syscw = p->ioac.syscw;
#endif
#ifdef CONFIG_TASK_IO_ACCOUNTING
	pg->read_bytes = p->ioac.read_bytes;
	pg->write_bytes = p->ioac.write_bytes;
#endif
}

/*
 * Restart accounting from "now, running on this CPU". Called by the owner
 * with preemption disabled, so neither hook can run concurrently.
 */
static void csf_baseline(struct csf_tobj *obj)
{
	struct csf_thread_page *pg = obj->pg;

	csf_write_begin(&pg->seq);
	pg->state = CSF_ST_RUNNING;
	pg->cpu = smp_processor_id();
	pg->last_switch_tsc = rdtsc();
	csf_copy_warm(pg, current);
	csf_write_end(&pg->seq);
	obj->out_run_delay = current->sched_info.run_delay;
}

/* The owner's final switch-out. Its notifier list is never walked again. */
static void csf_thread_dead(struct csf_tobj *obj, u64 now)
{
	struct csf_thread_page *pg = obj->pg;

	csf_write_begin(&pg->seq);
	if (pg->state == CSF_ST_RUNNING)
		pg->oncpu_cyc += csf_delta(now, pg->last_switch_tsc);
	pg->last_switch_tsc = now;
	pg->state = CSF_ST_DEAD;
	csf_copy_warm(pg, current);
	csf_write_end(&pg->seq);

	WRITE_ONCE(obj->registered, false);
	csf_tobj_put(obj);
}

/*
 * Called with the runqueue lock held and IRQs off, with current == prev.
 * sched_info_switch() and update_curr() have already run, so
 * sum_exec_runtime is fresh, and on_rq/sched_delayed say whether the
 * task blocked (stable under the rq lock, unlike __state, which a
 * concurrent ttwu() may already have set to TASK_WAKING).
 */
static void csf_sched_out(struct preempt_notifier *pn, struct task_struct *next)
{
	struct csf_tobj *obj = container_of(pn, struct csf_tobj, pn);
	struct csf_thread_page *pg = obj->pg;
	struct task_struct *p = current;
	u64 now = rdtsc();
	u32 state;

	if (unlikely(READ_ONCE(p->__state) == TASK_DEAD)) {
		csf_thread_dead(obj, now);
		return;
	}
	if (READ_ONCE(obj->idle))
		return;

	if (task_is_runnable(p))
		state = CSF_ST_RUNQ;
	else if (p->in_iowait)
		state = CSF_ST_IOWAIT;
	else if (p->sched_contributes_to_load) /* set by block_task() */
		state = CSF_ST_DSLEEP;
	else
		state = CSF_ST_SLEEP;

	csf_write_begin(&pg->seq);
	pg->oncpu_cyc += csf_delta(now, pg->last_switch_tsc);
	pg->last_switch_tsc = now;
	pg->state = state;
	if (state == CSF_ST_RUNQ)
		pg->nr_preempt++;
	else
		pg->nr_block++;
	csf_copy_warm(pg, p);
	csf_write_end(&pg->seq);
	obj->out_run_delay = p->sched_info.run_delay;
}

/*
 * Called from finish_task_switch() in the owner's context. run_delay has
 * already been updated by sched_info_arrive() for this switch.
 */
static void csf_sched_in(struct preempt_notifier *pn, int cpu)
{
	struct csf_tobj *obj = container_of(pn, struct csf_tobj, pn);
	struct csf_thread_page *pg = obj->pg;
	struct task_struct *p = current;
	u64 now, off, wait, rd;

	if (READ_ONCE(obj->idle))
		return;

	now = rdtsc();
	rd = p->sched_info.run_delay;
	off = csf_delta(now, pg->last_switch_tsc);
	/* Time runnable-but-waiting since the wakeup, clamped to the gap. */
	wait = min(csf_ns_to_cyc(csf_delta(rd, obj->out_run_delay)), off);

	csf_write_begin(&pg->seq);
	switch (pg->state) {
	case CSF_ST_SLEEP:
		pg->runq_cyc += wait;
		pg->sleep_cyc += off - wait;
		break;
	case CSF_ST_DSLEEP:
		pg->runq_cyc += wait;
		pg->dsleep_cyc += off - wait;
		break;
	case CSF_ST_IOWAIT:
		pg->runq_cyc += wait;
		pg->iowait_cyc += off - wait;
		break;
	default:
		/*
		 * Preempted while runnable. run_delay misses preemptions in a
		 * non-RUNNING __state, so count the whole gap.
		 */
		pg->runq_cyc += off;
		break;
	}
	if (pg->cpu != cpu)
		pg->nr_migrate++;
	pg->cpu = cpu;
	pg->nr_switch_in++;
	pg->last_switch_tsc = now;
	pg->state = CSF_ST_RUNNING;
	pg->run_delay_ns = rd;
	pg->pcount = p->sched_info.pcount;
	csf_write_end(&pg->seq);
}

static struct preempt_ops csf_preempt_ops = {
	.sched_in = csf_sched_in,
	.sched_out = csf_sched_out,
};

/* Only the owner modifies its list, and we are the owner. */
static struct csf_tobj *csf_find_current(void)
{
	struct preempt_notifier *pn;
	struct csf_tobj *obj = NULL;

	preempt_disable();
	hlist_for_each_entry(pn, &current->preempt_notifiers, link) {
		if (pn->ops == &csf_preempt_ops) {
			obj = container_of(pn, struct csf_tobj, pn);
			break;
		}
	}
	preempt_enable();
	return obj;
}

static struct csf_tobj *csf_tobj_create(void)
{
	struct csf_tobj *obj;

	obj = kzalloc_obj(*obj, GFP_KERNEL_ACCOUNT);
	if (!obj)
		return ERR_PTR(-ENOMEM);
	obj->page = alloc_page(GFP_KERNEL_ACCOUNT | __GFP_ZERO);
	if (!obj->page) {
		kfree(obj);
		return ERR_PTR(-ENOMEM);
	}
	obj->pg = page_address(obj->page);
	obj->owner = current;
	obj->users = 1;
	obj->registered = true;
	refcount_set(&obj->ref, 2); /* this file + the notifier */
	mutex_init(&obj->lock);

	csf_init_hdr(&obj->pg->hdr, CSF_KIND_THREAD, sizeof(*obj->pg), CSF_FEAT_THREAD_BASE);
	obj->pg->tid = task_pid_vnr(current);

	__module_get(THIS_MODULE); /* dropped by the last csf_tobj_put() */
	atomic_inc(&csf_nr_objs);
	preempt_notifier_init(&obj->pn, &csf_preempt_ops);
	/* Fully initialize before the hooks can see it. */
	preempt_disable();
	csf_baseline(obj);
	preempt_notifier_register(&obj->pn);
	preempt_enable();
	return obj;
}

static int csf_thread_open(struct inode *inode, struct file *file)
{
	struct csf_tobj *obj;

	if (file->f_mode & FMODE_WRITE)
		return -EACCES;
	if (current->flags & (PF_KTHREAD | PF_IO_WORKER))
		return -EINVAL;
	i_size_write(inode, PAGE_SIZE);

	obj = csf_find_current();
	if (obj) {
		mutex_lock(&obj->lock);
		/* The notifier's reference keeps it alive: only we drop it. */
		refcount_inc(&obj->ref);
		if (obj->users++ == 0) {
			/* Nobody saw the page while idle; restart cleanly. */
			preempt_disable();
			csf_baseline(obj);
			WRITE_ONCE(obj->idle, false);
			preempt_enable();
			atomic_dec(&csf_nr_idle);
		}
		mutex_unlock(&obj->lock);
	} else {
		obj = csf_tobj_create();
		if (IS_ERR(obj))
			return PTR_ERR(obj);
	}
	file->private_data = obj;
	atomic_inc(&csf_nr_files);
	return 0;
}

static int csf_thread_release(struct inode *inode, struct file *file)
{
	struct csf_tobj *obj = file->private_data;
	bool drop_notifier = false;

	mutex_lock(&obj->lock);
	if (--obj->users == 0) {
		/*
		 * registered is cleared at the owner's death, before its
		 * task_struct can be freed and reused, so a recycled pointer
		 * that equals current can't pass this check.
		 */
		if (current == obj->owner && READ_ONCE(obj->registered)) {
			preempt_disable();
			preempt_notifier_unregister(&obj->pn);
			preempt_enable();
			WRITE_ONCE(obj->registered, false);
			drop_notifier = true;
		} else {
			WRITE_ONCE(obj->idle, true);
			atomic_inc(&csf_nr_idle);
		}
	}
	mutex_unlock(&obj->lock);

	atomic_dec(&csf_nr_files);
	if (drop_notifier)
		csf_tobj_put(obj);
	csf_tobj_put(obj);
	return 0;
}

static int csf_thread_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct csf_tobj *obj = file->private_data;
	int ret = csf_check_mmap(file, vma);

	if (ret)
		return ret;
	/* A forked child must not see its parent thread's page. */
	vm_flags_mod(vma, VM_DONTCOPY | VM_DONTEXPAND | VM_DONTDUMP, VM_MAYWRITE);
	return vm_insert_page(vma, vma->vm_start, obj->page);
}

const struct file_operations csf_thread_fops = {
	.owner = THIS_MODULE,
	.open = csf_thread_open,
	.release = csf_thread_release,
	.mmap = csf_thread_mmap,
	.llseek = noop_llseek,
};
