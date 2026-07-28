/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_CACHE_GUARD_H
#define _LINUX_SCHED_CACHE_GUARD_H

struct task_struct;

int cache_guard_mark_secure(struct task_struct *p);
int cache_guard_migrate_secure_task_or_force_isolation(struct task_struct *p);
void cache_guard_periodic_check(void);

#endif
