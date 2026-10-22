#ifndef _LINUX_CORE_RESERVE_H
#define _LINUX_CORE_RESERVE_H

#include <linux/types.h>

#if defined(CONFIG_X86) && defined(CONFIG_SMP)

void core_reserve_exit(void);
bool is_core_claimed_already(unsigned int cpu);

#else

static inline void core_reserve_exit(void) {}

static inline bool is_core_claimed_already(unsigned int cpu)
{
    return false;
}

#endif
#endif