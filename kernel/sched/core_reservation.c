// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exclusive, persistent SMT core ownership. Included by core.c so evacuation
 * can use the same locked migration primitives as ordinary affinity changes.
 * See Documentation/admin-guide/core-reservation.rst for the supported scope.
 */
#include <uapi/linux/prctl.h>

enum core_reserve_state {
	CORE_RESERVE_DRAINING,
	CORE_RESERVE_ACTIVE,
	CORE_RESERVE_RELEASING,
};

struct sched_core_reservation {
	struct task_struct *owner;
	unsigned int cpu;
	enum core_reserve_state state;
	cpumask_var_t siblings;
	cpumask_var_t saved_mask;
	cpumask_var_t acknowledged;
	bool closing;
	bool stop_frozen;
	struct work_struct release_work;
	struct rcu_head rcu;
};

struct core_reserve_task {
	struct list_head node;
	struct task_struct *task;
};

/* One protected victim at a time. */
static struct sched_core_reservation __rcu *core_reservation;
static DEFINE_MUTEX(core_reserve_mutex);
/* Always acquired after task pi_lock/rq lock, never the reverse. */
static DEFINE_RAW_SPINLOCK(core_reserve_lock);

bool sched_core_reserve_hotplug_busy(void)
{
	/* Publication/removal is inside a cpus_read_lock() critical section. */
	return rcu_access_pointer(core_reservation) != NULL;
}

bool sched_core_reserve_allowed(struct task_struct *p, int cpu)
{
	struct sched_core_reservation *reserved_cpu;
	bool allowed;

	// if task is a per-cpu kernel thread, it is allowed to run on any CPU
	if (kthread_is_per_cpu(p))
		return true;
	rcu_read_lock();

	reserved_cpu = rcu_dereference(core_reservation);

	if (reserved_cpu && !cpumask_test_cpu(cpu, reserved_cpu->siblings))
		reserved_cpu = NULL;

	allowed = !reserved_cpu || (reserved_cpu->owner == p && reserved_cpu->cpu == cpu);
	
	if (reserved_cpu &&
	    READ_ONCE(reserved_cpu->state) == CORE_RESERVE_DRAINING &&
	    is_migration_disabled(p) && task_cpu(p) == cpu)
	{
		allowed = true;
	}

	rcu_read_unlock();

	return allowed;
}

void sched_core_reserve_affinity_lock(void)
{
	raw_spin_lock(&core_reserve_lock);
}

void sched_core_reserve_affinity_unlock(void)
{
	raw_spin_unlock(&core_reserve_lock);
}

// returns the first CPU in mask that is allowed for task p
// or nr_cpu_ids if none are allowed
int sched_core_reserve_dest(struct task_struct *p,
			    const struct cpumask *requested_mask,
			    const struct cpumask *valid_mask)
{
	int cpu;

	// if no SMT core reservation currently exists, 
	//pick any usable CPU from the intersection of the requested mask and the valid mask
	if (!rcu_access_pointer(core_reservation))
		return cpumask_any_and_distribute(valid_mask, requested_mask);
	// otherwise, return first valid option
	for_each_cpu_and(cpu, requested_mask, valid_mask) {
		if (sched_core_reserve_allowed(p, cpu))
			return cpu;
	}
	return nr_cpu_ids;
}

// checks whether a requested CPU-affinity change is compatible with the core reservation
// p: the task whose CPU affinity is being changed
// ctx->new_mask : 	the requested set of allowed CPUs
int sched_core_reserve_affinity_check(struct task_struct *p,
				      struct affinity_context *ctx)
{
	struct sched_core_reservation *r;

	lockdep_assert_held(&core_reserve_lock);
	// if there is no reservation, continue with the affinity change
	if (!rcu_access_pointer(core_reservation))
		return 0;
	// allow internal migration-enable/disable operations
	if (ctx->flags & (SCA_MIGRATE_ENABLE | SCA_MIGRATE_DISABLE))
		return 0;
	// dont't allow a task to be moved off the reserved core
	if (p->core_reservation && !(ctx->flags & SCA_CORE_RESERVE))
		return -EBUSY;

	// affine_move_task() must not enqueue work to a frozen stopper
	r = rcu_dereference_protected(core_reservation,
				      lockdep_is_held(&core_reserve_lock));
	if (r && cpumask_test_cpu(task_cpu(p), r->siblings) &&
	    READ_ONCE(r->closing) && !(ctx->flags & SCA_CORE_RESERVE))
		return -EBUSY;

	// checks if thesre is at least one active CPU in the requested affinity
	// mask where this task is allowed to run taking in account our core reservation
	if (!kthread_is_per_cpu(p) &&
	    sched_core_reserve_dest(p, ctx->new_mask, cpu_active_mask) >=
		    nr_cpu_ids)
		return -EBUSY;
	return 0;
}

int sched_core_reserve_fork(struct task_struct *p)
{
	unsigned long flags;
	int ret = 0;

	/* dup_task_struct copied the pointer; ownership is never inherited. */
	p->core_reservation = NULL;
	raw_spin_lock_irqsave(&core_reserve_lock, flags);
	if (current->core_reservation ||
	    (rcu_access_pointer(core_reservation) &&
	     (!kthread_is_per_cpu(p) &&
	      sched_core_reserve_dest(p, &p->cpus_mask, cpu_active_mask) >=
		      nr_cpu_ids)))
		ret = -EBUSY;
	raw_spin_unlock_irqrestore(&core_reserve_lock, flags);
	return ret;
}

struct task_struct *sched_core_reserve_pick(struct rq *rq)
{
	struct sched_core_reservation *r;
	struct task_struct *p = NULL;
	enum core_reserve_state state;

	lockdep_assert_rq_held(rq);
	rcu_read_lock();
	r = rcu_dereference(core_reservation);
	if (r && !cpumask_test_cpu(cpu_of(rq), r->siblings))
		r = NULL;
	if (!r)
		goto out;
	/* Pair with publication of the exclusion policy during activation. */
	state = smp_load_acquire(&r->state);
	if (state != CORE_RESERVE_ACTIVE)
		goto out;
	p = rq->idle;
	if (r->cpu == cpu_of(rq) && task_cpu(r->owner) == cpu_of(rq) &&
	    task_on_rq_queued(r->owner) && !r->owner->se.sched_delayed &&
	    (!r->owner->sched_class->task_is_throttled ||
	     !r->owner->sched_class->task_is_throttled(r->owner, cpu_of(rq))))
		p = r->owner;
out:
	rcu_read_unlock();
	return p;
}

static void core_reserve_ack(void)
{
	unsigned int cpu = raw_smp_processor_id();
	struct sched_core_reservation *r;
	enum core_reserve_state state;

	rcu_read_lock();
	r = rcu_dereference(core_reservation);
	if (r && !cpumask_test_cpu(cpu, r->siblings))
		r = NULL;
	if (!r)
		goto out;
	/* Only acknowledge after observing the published exclusion policy. */
	state = smp_load_acquire(&r->state);
	if (state == CORE_RESERVE_ACTIVE &&
	    (current == cpu_rq(cpu)->idle ||
	     (current == r->owner && cpu == r->cpu))) {
		/* Publish observation of quiescence before its acknowledgment. */
		smp_mb__before_atomic();
		cpumask_set_cpu(cpu, r->acknowledged);
	}
out:
	rcu_read_unlock();
}

void sched_core_reserve_finish(struct task_struct *prev, unsigned int state)
{
	struct sched_core_reservation *r;

	/* Called after finish_task() and after dropping the runqueue lock. */
	core_reserve_ack();
	if (state != TASK_DEAD)
		return;
	r = READ_ONCE(prev->core_reservation);
	if (r)
		queue_work(system_dfl_wq, &r->release_work);
}

static void core_reserve_kick(void *unused)
{
	struct rq *rq = this_rq();

	core_reserve_ack();
	raw_spin_rq_lock(rq);
	resched_curr(rq);
	raw_spin_rq_unlock(rq);
}

static void core_reserve_resched(const struct cpumask *mask)
{
	/* IRQ context cannot migrate between the local call and mask check. */
	preempt_disable();
	smp_call_function_many(mask, core_reserve_kick, NULL, true);
	if (cpumask_test_cpu(smp_processor_id(), mask)) {
		unsigned long flags;

		local_irq_save(flags);
		core_reserve_kick(NULL);
		local_irq_restore(flags);
	}
	preempt_enable();
}

static void core_reserve_free(struct rcu_head *head)
{
	struct sched_core_reservation *r;

	r = container_of(head, struct sched_core_reservation, rcu);
	put_task_struct(r->owner);
	free_cpumask_var(r->siblings);
	free_cpumask_var(r->saved_mask);
	free_cpumask_var(r->acknowledged);
	kfree(r);
}

static void core_reserve_remove(struct sched_core_reservation *r)
{
	unsigned long flags;

	/* Readers must stop enforcing the gate before ownership disappears. */
	smp_store_release(&r->state, CORE_RESERVE_RELEASING);
	if (r->stop_frozen)
		cpu_stop_unreserve(r->siblings);
	raw_spin_lock_irqsave(&core_reserve_lock, flags);
	RCU_INIT_POINTER(core_reservation, NULL);
	WRITE_ONCE(r->owner->core_reservation, NULL);
	raw_spin_unlock_irqrestore(&core_reserve_lock, flags);
	core_reserve_resched(r->siblings);
	pr_info("core_reserve: released PID %d; SMT CPUs: %*pbl\n",
		task_pid_nr(r->owner), cpumask_pr_args(r->siblings));
	call_rcu(&r->rcu, core_reserve_free);
}

static void core_reserve_release_work(struct work_struct *work)
{
	struct sched_core_reservation *r;

	r = container_of(work, struct sched_core_reservation, release_work);
	cpus_read_lock();
	mutex_lock(&core_reserve_mutex);
	/* A dead task will never run again; affinity restoration is unnecessary. */
	core_reserve_remove(r);
	mutex_unlock(&core_reserve_mutex);
	cpus_read_unlock();
}

static int core_reserve_restore(struct sched_core_reservation *r)
{
	struct affinity_context ac = {
		.new_mask = r->saved_mask,
		.flags = SCA_CORE_RESERVE,
	};

	/* The selected CPU belongs to saved_mask: no stopper is needed here. */
	return __set_cpus_allowed_ptr(r->owner, &ac);
}

static int core_reserve_evict_task(struct sched_core_reservation *r,
				   struct task_struct *p)
{
	struct migration_arg arg = { .task = p };
	struct rq_flags rf;
	struct rq *rq;
	int src, dest, ret = 0;

	if (p == r->owner)
		return 0;
	rq = task_rq_lock(p, &rf);
	/* A zombie may retain the dead owner's singleton affinity until reaped. */
	if (READ_ONCE(p->__state) == TASK_DEAD)
		goto unlock;
	if (kthread_is_per_cpu(p)) {
		if (cpumask_test_cpu(task_cpu(p), r->siblings) &&
		    (is_migration_disabled(p) || p->migration_pending))
			ret = -EAGAIN;
		goto unlock;
	}
	/* Reservation reduces schedulable capacity; do not invalidate DL admission. */
	if (dl_task(p)) {
		ret = -EBUSY;
		goto unlock;
	}
	dest = sched_core_reserve_dest(p, &p->cpus_mask, cpu_active_mask);
	if (dest >= nr_cpu_ids) {
		ret = -EBUSY;
		goto unlock;
	}
	src = task_cpu(p);
	if (!cpumask_test_cpu(src, r->siblings))
		goto unlock;
	/* Let these incumbents finish before trying again, never gate them in. */
	if (is_migration_disabled(p) || p->migration_pending) {
		ret = -EAGAIN;
		goto unlock;
	}
	if (task_on_cpu(rq, p)) {
		arg.dest_cpu = dest;
		task_rq_unlock(rq, p, &rf);
		ret = stop_one_cpu(src, migration_cpu_stop, &arg);
		return ret ?: -EAGAIN;
	}
	if (task_on_rq_queued(p)) {
		update_rq_clock(rq);
		rq = move_queued_task(rq, &rf, p, dest);
	} else {
		/* Never relocate a blocked task's task_cpu; route its next wakeup. */
		p->wake_cpu = dest;
	}
unlock:
	task_rq_unlock(rq, p, &rf);
	return ret;
}

static int core_reserve_drain(struct sched_core_reservation *r)
{
	struct core_reserve_task *item, *next;
	struct task_struct *g, *p;
	LIST_HEAD(tasks);
	int ret = 0;

	/* References allow all sleeping migration operations outside RCU. */
	rcu_read_lock();
	for_each_process_thread(g, p) {
		item = kmalloc_obj(*item, GFP_ATOMIC);
		if (!item) {
			ret = -ENOMEM;
			break;
		}
		get_task_struct(p);
		item->task = p;
		list_add_tail(&item->node, &tasks);
	}
	rcu_read_unlock();
	list_for_each_entry_safe(item, next, &tasks, node) {
		int err;

		if (!ret || ret == -EAGAIN) {
			err = core_reserve_evict_task(r, item->task);
			if (err && (!ret || err != -EAGAIN))
				ret = err;
		}
		put_task_struct(item->task);
		list_del(&item->node);
		kfree(item);
		cond_resched();
	}
	return ret;
}

static int core_reserve_enable(unsigned int cpu)
{
	struct sched_core_reservation *r;
	struct affinity_context ac;
	unsigned long flags, deadline;
	struct rq_flags rf;
	struct rq *rq;
	int ret;

	if (current->core_reservation)
		return -EALREADY;
	if (dl_task(current))
		return -EBUSY;
	if (cpu >= nr_cpu_ids || !cpu_active(cpu))
		return -EINVAL;
	if (is_migration_disabled(current) || current->migration_pending)
		return -EBUSY;
	r = kzalloc_obj(*r, GFP_KERNEL);
	if (!r)
		return -ENOMEM;
	if (!zalloc_cpumask_var(&r->siblings, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&r->saved_mask, GFP_KERNEL) ||
	    !zalloc_cpumask_var(&r->acknowledged, GFP_KERNEL)) {
		ret = -ENOMEM;
		goto free;
	}
	cpumask_copy(r->siblings, cpu_smt_mask(cpu));
	/* All siblings must be active; hotplug cannot change this until release. */
	if (!cpumask_subset(r->siblings, cpu_active_mask) ||
	    cpumask_equal(r->siblings, cpu_active_mask)) {
		ret = -EBUSY;
		goto free;
	}
	r->owner = current;
	r->cpu = cpu;
	r->state = CORE_RESERVE_DRAINING;
	INIT_WORK(&r->release_work, core_reserve_release_work);

	rq = task_rq_lock(current, &rf);
	raw_spin_lock(&core_reserve_lock);
	ret = -EBUSY;
	if (rcu_access_pointer(core_reservation))
		goto unlock;
	ret = -EINVAL;
	if (!cpumask_test_cpu(cpu, &current->cpus_mask))
		goto unlock;
	cpumask_copy(r->saved_mask, &current->cpus_mask);
	get_task_struct(current);
	WRITE_ONCE(current->core_reservation, r);
	rcu_assign_pointer(core_reservation, r);
	ret = 0;
unlock:
	raw_spin_unlock(&core_reserve_lock);
	task_rq_unlock(rq, current, &rf);
	if (ret)
		goto free;

	ac = (struct affinity_context){
		.new_mask = cpumask_of(cpu),
		.flags = SCA_CORE_RESERVE,
	};
	ret = __set_cpus_allowed_ptr(current, &ac);
	if (ret)
		goto rollback;
	/* Close affinity admission before draining accepted migration requests. */
	raw_spin_lock_irqsave(&core_reserve_lock, flags);
	r->closing = true;
	raw_spin_unlock_irqrestore(&core_reserve_lock, flags);
	deadline = jiffies + HZ;
	do {
		ret = core_reserve_drain(r);
		if (!ret)
			ret = cpu_stop_reserve(r->siblings);
		if (!ret)
			break;
		if (ret != -EAGAIN && ret != -EBUSY)
			goto rollback;
		if (fatal_signal_pending(current)) {
			ret = -EINTR;
			goto rollback;
		}
		schedule_timeout_uninterruptible(1);
	} while (time_before(jiffies, deadline));
	if (ret) {
		ret = -EBUSY;
		goto rollback;
	}
	WRITE_ONCE(r->stop_frozen, true);
	/* Publish the gate before requesting post-switch acknowledgments. */
	smp_store_release(&r->state, CORE_RESERVE_ACTIVE);
	core_reserve_resched(r->siblings);
	while (!cpumask_equal(r->siblings, r->acknowledged)) {
		if (time_after_eq(jiffies, deadline) ||
		    fatal_signal_pending(current)) {
			ret = -EBUSY;
			goto rollback;
		}
		schedule_timeout_uninterruptible(1);
	}
	/*
	 * Catch any pre-publication wakeup that attached after the first scan.
	 * After the barrier outsiders are off-CPU, so queued migration needs no
	 * stopper. Placement now checks admission under the reservation lock.
	 */
	ret = core_reserve_drain(r);
	if (ret)
		goto rollback;
	/* Each sibling acknowledged a post-switch safe execution context. */
	smp_mb();
	/* Do not return to protected userspace until every sibling is safe. */
	WARN_ON_ONCE(raw_smp_processor_id() != cpu);
	pr_info("core_reserve: ACTIVE PID %d on CPU %u; SMT CPUs: %*pbl\n",
		task_pid_nr(current), cpu, cpumask_pr_args(r->siblings));
	return 0;

rollback:
	/* saved_mask includes cpu, so restoration needs no frozen stopper. */
	ret = ret ?: -EBUSY;
	WARN_ON_ONCE(core_reserve_restore(r));
	core_reserve_remove(r);
	return ret;
free:
	free_cpumask_var(r->siblings);
	free_cpumask_var(r->saved_mask);
	free_cpumask_var(r->acknowledged);
	kfree(r);
	return ret;
}

int sched_core_reserve_prctl(unsigned long cmd, unsigned long cpu,
			     unsigned long arg4, unsigned long arg5)
{
	struct sched_core_reservation *r;
	int ret;

	if (arg4 || arg5 || cmd > PR_SCHED_CORE_RESERVE_DISABLE ||
	    (cmd != PR_SCHED_CORE_RESERVE_ENABLE && cpu) || cpu > INT_MAX)
		return -EINVAL;
	if (cmd == PR_SCHED_CORE_RESERVE_GET) {
		r = READ_ONCE(current->core_reservation);
		return r ? r->cpu + 1 : 0;
	}
	/* Core ownership can indefinitely defer kernel work: initial namespace. */
	if (cmd == PR_SCHED_CORE_RESERVE_ENABLE && !capable(CAP_SYS_NICE))
		return -EPERM;
	cpus_read_lock();
	mutex_lock(&core_reserve_mutex);
	if (cmd == PR_SCHED_CORE_RESERVE_ENABLE) {
		ret = core_reserve_enable(cpu);
	} else {
		r = current->core_reservation;
		ret = r ? core_reserve_restore(r) : -EINVAL;
		if (!ret)
			core_reserve_remove(r);
	}
	mutex_unlock(&core_reserve_mutex);
	cpus_read_unlock();
	return ret;
}
