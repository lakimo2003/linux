#ifndef _LINUX_CORE_RESERVE_H
#define _LINUX_CORE_RESERVE_H

#if defined(CONFIG_X86) && defined(CONFIG_SMP)
void core_reserve_exit(void);
#else
static inline void core_reserve_exit(void) {}
#endif

#endif