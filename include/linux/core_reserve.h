#ifndef _LINUX_CORE_RESERVE_H
#define _LINUX_CORE_RESERVE_H

#include <linux/types.h>
struct task_struct;

#if defined(CONFIG_X86) && defined(CONFIG_SMP)

void core_reserve_exit(void);
bool is_core_claimed_already(unsigned int cpu);
bool core_reserve_task_allowed(struct task_struct *p,
                               unsigned int cpu);

#else

static inline void core_reserve_exit(void) {}

static inline bool is_core_claimed_already(unsigned int cpu)
{
    return false;
}

static inline bool core_reserve_task_allowed(struct task_struct *p,
                                             unsigned int cpu)
{
    return true;
}

#endif
#endif