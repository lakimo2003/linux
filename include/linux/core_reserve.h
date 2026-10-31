// SPDX-License-Identifier: GPL-2.0
#ifndef _LINUX_CORE_RESERVE_H
#define _LINUX_CORE_RESERVE_H

#include <linux/errno.h>
#include <linux/types.h>
struct task_struct;
struct cpumask;

#if defined(CONFIG_X86) && defined(CONFIG_SMP)
#include <linux/rwsem.h>

extern struct rw_semaphore core_reserve_lock;
extern struct task_struct *core_reserve_owner;

long core_reserve_enable(unsigned long selected_cpu);
long core_reserve_disable(void);
void core_reserve_exit(void);
int core_reserve_change_affinity(struct task_struct *task,
				const struct cpumask *available_cpus,
				bool restore);
#else
// Report that this implementation needs an x86 SMP kernel.
static inline long core_reserve_enable(unsigned long selected_cpu)
{
	return -EOPNOTSUPP;
}

// Report that this implementation needs an x86 SMP kernel.
static inline long core_reserve_disable(void)
{
	return -EOPNOTSUPP;
}

// There is no reservation to release on unsupported configurations.
static inline void core_reserve_exit(void) {}
#endif
#endif
