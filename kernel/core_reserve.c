// SPDX-License-Identifier: GPL-2.0
// Minimal experimental isolation of user tasks on one physical core.
// Keep CPU hotplug, cpusets and scheduling policies unchanged while active.
#include <linux/core_reserve.h>

#if defined(CONFIG_X86) && defined(CONFIG_SMP)
#include <linux/capability.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/topology.h>

DECLARE_RWSEM(core_reserve_lock);
struct task_struct *core_reserve_owner;
static cpumask_t reserved_cpus;
static cpumask_t available_cpus;

// Find one user thread to restrict or restore, and hold a reference to it.
// Restarting the walk avoids allocating a task array or sleeping under RCU.
static struct task_struct *find_affected_task(bool restore)
{
	struct task_struct *process, *task, *found = NULL;
	unsigned long interrupt_flags;
	bool affected;

	rcu_read_lock();
	for_each_process_thread(process, task) {
		if (task->flags & PF_KTHREAD)
			continue;
		raw_spin_lock_irqsave(&task->pi_lock, interrupt_flags);
		affected = restore ? task->core_reserve_affinity_saved :
			!task->core_reserve_affinity_saved &&
			(cpumask_intersects(&task->cpus_mask, &reserved_cpus) ||
			 cpumask_test_cpu(task_cpu(task), &reserved_cpus));
		raw_spin_unlock_irqrestore(&task->pi_lock, interrupt_flags);
		if (affected) {
			found = get_task_struct(task);
			break;
		}
	}
	rcu_read_unlock();
	return found;
}

// Restore original masks, including masks inherited by children.
// Keep unfinished saved masks available if restoration needs another attempt.
static int restore_user_tasks(void)
{
	struct task_struct *task;
	int error;

	while ((task = find_affected_task(true))) {
		error = core_reserve_change_affinity(task, NULL, true);
		put_task_struct(task);
		if (error)
			return error;
		cond_resched();
	}
	WRITE_ONCE(core_reserve_owner, NULL);
	return 0;
}

// Pin the caller to the selected CPU and move other user threads off its core.
// A successful return means all affected threads have finished migrating.
long core_reserve_enable(unsigned long selected_cpu)
{
	struct task_struct *task;
	int error = 0, restore_error;

	// Moving other users' threads requires host administrator privileges.
	if (sched_proxy_exec())
		return -EOPNOTSUPP;
	if (!capable(CAP_SYS_NICE))
		return -EPERM;
	if (selected_cpu >= nr_cpu_ids)
		return -EINVAL;

	down_write(&core_reserve_lock);
	cpus_read_lock();
	if (core_reserve_owner) {
		error = -EBUSY;
		goto unlock;
	}

	// Finish any cleanup left behind by an owner that exited.
	error = restore_user_tasks();
	if (error)
		goto unlock;

	if (!cpu_active(selected_cpu)) {
		error = -EINVAL;
		goto unlock;
	}

	cpumask_copy(&reserved_cpus, topology_sibling_cpumask(selected_cpu));
	cpumask_andnot(&available_cpus, cpu_active_mask, &reserved_cpus);
	if (cpumask_empty(&available_cpus)) {
		error = -EBUSY;
		goto unlock;
	}
	cpuset_cpus_allowed(current, &available_cpus);
	if (!cpumask_test_cpu(selected_cpu, &available_cpus)) {
		error = -EINVAL;
		goto unlock;
	}

	// Publish before changing masks so new affinity requests cannot race us.
	WRITE_ONCE(core_reserve_owner, current);
	error = core_reserve_change_affinity(current, cpumask_of(selected_cpu), false);
	if (error)
		goto restore;

	while ((task = find_affected_task(false))) {
		// Respect cpuset boundaries even when overriding a user's CPU pin.
		cpuset_cpus_allowed(task, &available_cpus);
		cpumask_and(&available_cpus, &available_cpus, cpu_active_mask);
		cpumask_andnot(&available_cpus, &available_cpus, &reserved_cpus);
		error = cpumask_empty(&available_cpus) ? -EBUSY :
			core_reserve_change_affinity(task, &available_cpus, false);
		put_task_struct(task);
		if (error)
			goto restore;
		cond_resched();
	}
	goto unlock;

restore:
	restore_error = restore_user_tasks();
	if (restore_error)
		error = restore_error;
unlock:
	cpus_read_unlock();
	up_write(&core_reserve_lock);
	return error;
}

// Release the reservation and allow the original CPU masks again.
long core_reserve_disable(void)
{
	int error;

	down_write(&core_reserve_lock);
	cpus_read_lock();

	// The owner can release protection. An administrator can retry
	// unfinished cleanup after the owner has exited.
	if (core_reserve_owner == current ||
	    (!core_reserve_owner && capable(CAP_SYS_NICE)))
		error = restore_user_tasks();
	else
		error = -EINVAL;

	cpus_read_unlock();
	up_write(&core_reserve_lock);
	return error;
}

// Release protection automatically if its owning thread exits.
void core_reserve_exit(void)
{
	int error;

	if (READ_ONCE(core_reserve_owner) != current)
		return;

	down_write(&core_reserve_lock);
	cpus_read_lock();
	error = restore_user_tasks();

	// Never leave ownership pointing to a thread that is exiting.
	// Failed restorations retain their saved masks for a later retry.
	WRITE_ONCE(core_reserve_owner, NULL);
	if (error)
		pr_warn("core_reserve: exit cleanup incomplete (%d); administrator must retry disable\n",
			error);

	cpus_read_unlock();
	up_write(&core_reserve_lock);
}
#endif
