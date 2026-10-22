.. SPDX-License-Identifier: GPL-2.0-only

Experimental exclusive SMT core reservation
===========================================

This research prototype reserves one physical SMT core for one calling thread.
Only one reservation may exist system-wide at a time.
It replaces the earlier bookkeeping-only implementation in kernel/sys.c.
Enable returns success only after movable outsiders have been evacuated and
all siblings have acknowledged owner-or-idle execution. A sleeping owner
retains its reservation. Explicit disable restores its saved CPU affinity.
Exit cleanup runs after its final context switch; RCU protects readers while
the reservation object is retired.

Scope and restrictions
----------------------

This is experimental scheduler code, not a production security guarantee.
It excludes scheduled tasks on the reserved core, including per-CPU kernel
threads, after successful activation. Hard interrupts, direct softirq work,
NMIs, firmware and caches shared beyond the physical core remain outside
this exclusion. It neither flushes caches nor guarantees that every cache
attack stops working.

Keep protected intervals short. The owner must not wait for per-CPU work
that its own reservation prevents from running. Otherwise the owner and
other system work can stall. At least one active, unreserved CPU must remain
for housekeeping and cleanup.

The supported build is X86_64 with EXPERT, SMP and SCHED_SMT. Disable CPUSETS,
SCHED_CORE, SCHED_CLASS_EXT, SCHED_PROXY_EXEC and PREEMPT_RT. This deliberately
limits the scheduler and affinity paths supported by the prototype.
CPU online/offline operations are refused while any reservation exists.
SCHED_DEADLINE tasks prevent activation; changing to that policy while a
reservation exists is refused. Owners cannot fork/clone or change scheduler
policy while reserved.

Other tasks retain their original affinity masks. A user thread pinned
entirely to the target core causes EBUSY; the implementation does not override
that affinity or silently suspend the thread. Allow the attacker on an
active CPU outside the target core before testing evacuation.

All SMT siblings must be online and active. The destination CPU must already
belong to the caller's affinity mask. In particular, a victim pinned only to
CPU 1 must expand its mask to include CPU 11 before requesting CPU 11.
This differs from the earlier migration-only helper.

Interface
---------

The private option remains PR_SCHED_CORE_RESERVE (0x43525356)::

    prctl(PR_SCHED_CORE_RESERVE, 1UL, target_cpu, 0UL, 0UL); /* enable */
    /* Begin protected work only after a successful return. */
    prctl(PR_SCHED_CORE_RESERVE, 2UL, 0UL, 0UL, 0UL);       /* disable */

Pass target_cpu as unsigned long. Check both return values; failure follows
the ordinary prctl convention of -1 with errno set. Enable requires
CAP_SYS_NICE in the initial user namespace, normally obtained through sudo.
GET (operation 0, with all remaining arguments zero) returns the selected
CPU plus one, or zero if this thread owns no reservation.

EINVAL indicates invalid arguments, unavailable CPU, a destination outside
the caller's affinity, or a disabled feature. EBUSY indicates conflicting
ownership, an immovable task, a deadline task, or setup unable to quiesce.
EPERM indicates missing privilege. EALREADY indicates a repeated enable.

DRAINING closes ordinary placement and evacuates queued/running tasks using
scheduler migration primitives. Temporarily migration-disabled incumbents
are allowed to finish. After draining, new CPU-stopper work is frozen.
ACTIVE enforces owner-or-idle selection. Enable waits for every sibling to
acknowledge that policy before returning success. RELEASING reopens
scheduling and retires ownership.
The retry budget is about one second, but blocking migration and mutex
operations mean that enable has no strict wall-clock timeout.

Kernel messages identify successful activation and release::

    sudo dmesg -w | grep --line-buffered 'core_reserve:'

An ACTIVE message reports completed activation; the earlier diagnostic-only
"owner allowed" messages are no longer used.

Build with an existing configuration
------------------------------------

From the kernel source directory, preserve the lab's current hardware
configuration and adjust only the feature requirements::

    cp .config .config.before-core-reservation
    scripts/config -e EXPERT -e SMP -e SCHED_SMT \
        -d CPUSETS -d SCHED_CORE -d SCHED_CLASS_EXT \
        -d SCHED_PROXY_EXEC -d PREEMPT_RT -e SCHED_CORE_RESERVATION
    make olddefconfig
    grep '^CONFIG_SCHED_CORE_RESERVATION=y' .config
    make -j8 LOCALVERSION=-smt-step5

Do not use tinyconfig or defconfig to replace a remote lab machine's hardware
configuration. A successful build alone does not establish bootability or
runtime exclusion on that machine. Keep the known-good kernel available.
Use the same LOCALVERSION for modules_install and install.

Focused validation
------------------

The patch includes tools/testing/selftests/sched/core_reservation.c. It checks
argument validation, permissions, pinned-task rejection, migration, affinity
restoration, contention, sleeping/stopped owners, repeated intervals and
cleanup after exit and SIGKILL. Attacker workers sample CPUs only within a
published protected interval and must continue progressing elsewhere::

    gcc -O2 -Wall -Wextra -Werror \
        tools/testing/selftests/sched/core_reservation.c \
        -o /tmp/core_reservation_test
    sudo /tmp/core_reservation_test

The test requires at least two SMT cores and skips unsupported setups. The
optional hotplug case performs a real sysfs write and is intended for a
disposable test environment::

    sudo env CORE_RESERVATION_TEST_HOTPLUG=1 /tmp/core_reservation_test

Run the test after booting the modified kernel, before benchmarking the
victim/attacker workload. Inspect dmesg for warnings and lockdep reports.
