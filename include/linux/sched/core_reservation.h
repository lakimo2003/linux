/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_CORE_RESERVATION_H
#define _LINUX_SCHED_CORE_RESERVATION_H

#include <linux/errno.h>
#include <linux/types.h>

#ifdef CONFIG_SCHED_CORE_RESERVATION
int sched_core_reserve_prctl(unsigned long cmd, unsigned long cpu,
			     unsigned long arg4, unsigned long arg5);
bool sched_core_reserve_hotplug_busy(void);
#else
static inline int sched_core_reserve_prctl(unsigned long cmd, unsigned long cpu,
					  unsigned long arg4, unsigned long arg5)
{
	return -EINVAL;
}

static inline bool sched_core_reserve_hotplug_busy(void) { return false; }
#endif

#endif /* _LINUX_SCHED_CORE_RESERVATION_H */
