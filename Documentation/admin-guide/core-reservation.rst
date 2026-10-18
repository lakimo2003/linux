.. SPDX-License-Identifier: GPL-2.0-only

Experimental exclusive SMT core reservations
============================================

``CONFIG_SCHED_CORE_RESERVATION`` provides a privileged research interface for
reserving a physical core for one thread. A successful enable operation drains
other movable tasks, places the caller on its selected logical CPU, and waits
until every SMT sibling has reached a safe execution context. The reservation
persists until the owner disables it or finishes exiting.

This is experimental kernel code and a private ABI, not an upstream interface
or a production isolation guarantee. Use a disposable test system. Deferring
per-CPU kernel threads can prevent essential work from completing; a privileged
owner can therefore stall the system. Keep intervals short and understand the
owner's dependencies before enabling a reservation.

Execution contract
------------------

Let ``V`` be the owner thread, ``C`` its selected logical CPU, and ``S`` the
topology mask returned by ``cpu_smt_mask(C)``. Once enable returns success:

========================== =======================================================
CPU                        Permitted scheduled execution contexts
========================== =======================================================
``C``                      ``V`` or that CPU's idle task
Every other CPU in ``S``   That CPU's idle task
CPUs outside ``S``         Ordinary scheduling, subject to other reservations
========================== =======================================================

The final task-selection gate applies to every scheduling class, including
per-CPU kernel threads and the stop scheduling class. A runnable owner remains
subject to its class's throttling rules; if it cannot run, its CPU runs idle.
Other threads in the same process do not share ownership.

Sleep, preemption and ``SIGSTOP`` do not release ownership. If the owner sleeps,
all CPUs in ``S`` run idle until it becomes eligible again. This covers the gap
between two protected computations separated by a sleep.

Idle execution, hard interrupts, NMIs and firmware execution remain possible.
Softirq work executed directly in an interrupt or the owner's context is not
a separately scheduled task. A scheduled ``ksoftirqd`` thread is excluded by
the task-selection gate. The mechanism neither clears microarchitectural state
nor isolates shared caches outside the core. It does not establish complete
protection against any particular side channel.

Supported configuration and preparation
---------------------------------------

This prototype supports ``X86_64`` only. Enable ``EXPERT``, ``SMP``,
``SCHED_SMT`` and ``SCHED_CORE_RESERVATION``. The configuration excludes:

* ``SCHED_CORE``: ordinary cookie-based core scheduling.
* ``SCHED_CLASS_EXT``: BPF scheduler extensions.
* ``SCHED_PROXY_EXEC``: execution through a different scheduling donor.
* ``CPUSETS``: cpuset hierarchy changes and their affinity overrides.
* ``PREEMPT_RT``: real-time preemption mode.

These exclusions delimit the implementation's supported paths. Cpuset changes
are unavailable in this configuration; they are not silently accepted as a
supported way to reconfigure a reserved thread.

All siblings of the selected core must be active. The selected CPU must be in
the caller's existing affinity mask. At least one active, unreserved CPU must
remain outside the reserved core for housekeeping and eventual cleanup. The
ABI does not select a core automatically or reserve a whole process.

Every ordinary task must retain a legal CPU outside the new reservation.
Consequently, a user thread pinned entirely to the selected core prevents
enable, even if that thread is sleeping. Relax such affinity before enabling.
Movable tasks retain their requested affinity masks; reservation admission is
an additional scheduling constraint, not a permanent edit of their masks.

Per-CPU kernel threads are treated differently: their local placement remains
valid and their execution is deferred. They are not moved to an arbitrary CPU.
The owner must not wait for a local worker, timer thread or other per-CPU task
that the reservation prevents from running. Long reservations can also disrupt
RCU progress, watchdogs and unrelated system services.

``SCHED_DEADLINE`` admission is restricted while reservations exist. Enable
rejects existing deadline tasks, and changing a task to deadline policy is
refused while a reservation is claimed. Removing CPUs from ordinary scheduling
without revising deadline bandwidth admission would otherwise overcommit the
remaining CPUs.

Private userspace ABI
---------------------

The experimental option is ``PR_SCHED_CORE_RESERVE`` (``0x43525356``), defined
in ``include/uapi/linux/prctl.h``. It is not an allocated upstream prctl number.
All operations apply to the calling thread. Pass ``unsigned long`` values to
the variadic ``prctl()`` interface, including zero arguments::

    prctl(PR_SCHED_CORE_RESERVE, operation, cpu, 0UL, 0UL);

================================== ====================================================
Operation                          Meaning and successful return value
================================== ====================================================
``PR_SCHED_CORE_RESERVE_GET``      ``cpu`` must be zero; return selected CPU + 1,
                                   or zero when no reservation is owned.
``PR_SCHED_CORE_RESERVE_ENABLE``   Reserve ``cpu`` and return zero only after
                                   placement and exclusion are effective.
``PR_SCHED_CORE_RESERVE_DISABLE``  ``cpu`` must be zero; restore affinity,
                                   release ownership and return zero.
================================== ====================================================

Only enable requires ``CAP_SYS_NICE`` in the initial user namespace. GET and
disable remain usable after an owner drops that privilege. There is no remote
PID operation, nesting or automatic expiry. GET reports ownership; it is not
a cross-thread query or a substitute for successful enable completion.

Errors use the normal ``prctl()`` convention: return ``-1`` and set ``errno``.
Common errors include:

``EINVAL``
    Invalid command, nonzero reserved arguments, an invalid or inactive CPU,
    a selected CPU outside the caller's affinity, disabling without ownership,
    or a kernel built without this feature.
``EPERM``
    Enable lacks the required capability.
``EALREADY``
    The caller already owns a reservation.
``EBUSY``
    A core is owned, tasks cannot be evacuated, an incompatible affinity or
    scheduling condition exists, insufficient housekeeping CPUs remain, or
    setup cannot complete its drain and quiescence checks.
``ENOMEM``
    Reservation or task-list allocation failed.
``EINTR``
    A fatal signal was observed at an interruptible setup checkpoint.

Some internal migration failures can propagate their own error. A failed
enable rolls back the claim and restores the saved affinity before returning.
Do not begin the protected computation after any failed enable.

For example, an application can bracket two computations and a sleep as
follows. The computation functions are application code::

    #include <errno.h>
    #include <linux/prctl.h>
    #include <stdio.h>
    #include <stdlib.h>
    #include <sys/prctl.h>
    #include <time.h>

    static int protected_interval(unsigned int cpu)
    {
        struct timespec delay = { .tv_nsec = 1000000 };
        int saved_error = 0;

        if (prctl(PR_SCHED_CORE_RESERVE,
                  (unsigned long)PR_SCHED_CORE_RESERVE_ENABLE,
                  (unsigned long)cpu, 0UL, 0UL) < 0)
            return -1;

        vulnerable_part_one();
        if (nanosleep(&delay, NULL) < 0)
            saved_error = errno;
        if (!saved_error)
            vulnerable_part_two();

        if (prctl(PR_SCHED_CORE_RESERVE,
                  (unsigned long)PR_SCHED_CORE_RESERVE_DISABLE,
                  0UL, 0UL, 0UL) < 0) {
            perror("release core reservation");
            abort(); /* Exit also initiates kernel-side release. */
        }
        if (saved_error) {
            errno = saved_error;
            return -1;
        }
        return 0;
    }

Check every failure path after enabling, including application exceptions and
cancellation. Explicit disable is preferable to relying on process termination.
Protection ends when the owner invokes disable; normal scheduling can resume
before that syscall returns.

Enable transaction and scheduler enforcement
--------------------------------------------

The implementation is in ``kernel/sched/core_reservation.c``, included by
``core.c`` to use existing locked migration primitives. A reservation holds a
task reference, the selected CPU, sibling and saved-affinity masks, an
acknowledgment mask, and its state. Each sibling publishes the same
RCU-protected reservation pointer.

The enable transaction proceeds as follows:

1. Hold ``cpus_read_lock()`` during setup and serialize reservation operations
   with ``core_reserve_mutex``. Validate the requested CPU and topology. Under
   the owner's task/runqueue locks and ``core_reserve_lock``, claim every
   sibling, save the owner's ``cpus_mask`` and publish ``DRAINING``. Another
   request for either sibling cannot acquire the same physical core.
2. Pin the owner with the ordinary affinity/migration implementation. The
   private ``SCA_CORE_RESERVE`` flag preserves ``user_cpus_ptr`` and bypasses
   its ordinary requested-mask intersection. It is used for restoration too;
   the exact saved ``cpus_mask`` is restored without replacing the previous
   userspace affinity request.
3. Close incompatible affinity admission and evacuate existing movable tasks.
   Running tasks use ``migration_cpu_stop`` through ``stop_one_cpu()``;
   queued tasks use locked runqueue migration. Blocked tasks keep their
   recorded CPU, with their next wakeup redirected to an allowed destination.
4. Keep normal task selection available during ``DRAINING``. In particular, a
   task in ``migrate_disable()`` must be able to resume and eventually call
   ``migrate_enable()``. Pending migrations and temporarily busy stoppers are
   retried instead of permanently trapping their tasks behind the final gate.
5. Reserve the core's CPU stoppers only when callbacks are neither queued nor
   running. New stopper work targeting the core is then refused. Publish
   ``QUIESCING``, activate owner/idle task selection, and reschedule siblings.
6. Wait for all siblings to acknowledge an allowed execution context. The
   post-switch hook runs after ``finish_task()`` and release of the runqueue
   lock. An IPI may acknowledge a CPU already executing its owner or idle
   context. Sending a reschedule IPI, observing ``rq->curr``, and an empty
   stopper work list are individually insufficient acknowledgments.
7. After every acknowledgment, perform one final evacuation scan for a
   pre-publication wakeup that attached late. Outsiders are now off-CPU, so
   queued migration does not require a frozen stopper. Publish ``ACTIVE`` and
   return zero on the selected CPU. Only this successful return authorizes
   the protected work.

Setup uses a budget of ``HZ`` jiffies checked between retries and acknowledgment
waits. This is not a hard syscall timeout: migration, mutex acquisition and
other blocking operations can take longer. Failure unwinds both affinity and
ownership, including reopening stopper admission when necessary.

The state invariants are:

============== ==================================================================
State          Invariant while the reservation remains published
============== ==================================================================
``DRAINING``   Admission closed to outsiders; incumbents may finish executing
``QUIESCING``  Owner/idle selection enforced; stopper work refused; wait for ack
``ACTIVE``     Same exclusion policy; successful enable has completed
``RELEASING``  Affinity restored; normal selection resumes as metadata is removed
============== ==================================================================

Two independent scheduler mechanisms enforce the reservation. Placement checks
filter wakeup, affinity, NUMA migration and load-balancing destinations. The
final selection hook in ``__pick_next_task()`` selects only the owner or idle
during ``QUIESCING`` and ``ACTIVE`` and uses normal
``put_prev_set_next_task()`` accounting. Per-CPU threads may remain queued,
but this final gate prevents them from executing.

Reading the code by responsibility
----------------------------------

Start with ``include/uapi/linux/prctl.h`` and the ``PR_SCHED_CORE_RESERVE``
case in ``kernel/sys.c``. The constants define the private interface; the
syscall dispatch passes its arguments to ``sched_core_reserve_prctl()``.
The latter validates unused arguments, implements GET, checks enable's
capability, and takes the hotplug read lock followed by the transaction mutex.
The configuration-disabled inline stub in
``include/linux/sched/core_reservation.h`` returns ``EINVAL``.

The enable call graph is::

    prctl(PR_SCHED_CORE_RESERVE, ENABLE, cpu, 0, 0)
      sched_core_reserve_prctl()
        cpus_read_lock(); mutex_lock(core_reserve_mutex)
        core_reserve_enable(cpu)
          claim siblings; save affinity; publish DRAINING
          __set_cpus_allowed_ptr(owner, singleton, SCA_CORE_RESERVE)
          close new affinity transactions from the reserved CPUs
          core_reserve_drain()
            core_reserve_evict_task() for each referenced thread
          cpu_stop_reserve(siblings)
          publish QUIESCING; core_reserve_resched(siblings)
          wait for acknowledged == siblings
          core_reserve_drain() again
          publish ACTIVE; return success

``struct sched_core_reservation`` contains the persistent state. The ``owner``
pointer identifies a thread rather than a reusable numeric PID. ``cpu`` is the
one logical CPU on which that owner may execute. ``siblings`` is the whole
physical core. ``saved_mask`` is needed for explicit disable and rollback.
``acknowledged`` records completed quiescence observations. ``closing`` closes
affinity admission before freezing stoppers; ``stop_frozen`` records that
stopper admission must later be reopened. ``release_work`` and ``rcu`` manage
exit cleanup and eventual reclamation.

``core_reserve_enable()`` checks every sibling for an existing reservation
while holding ``core_reserve_lock``. It then publishes the same object on all
siblings and installs ``current->core_reservation``. These pointers have
different purposes: the per-CPU pointers answer who owns a core, while the
per-task pointer answers which reservation belongs to a thread. A second
claim cannot independently reserve the other logical sibling.

The owner is pinned before draining. This gives the setup thread a stable
execution CPU while it coordinates the siblings. Protection still starts
only after the final successful return; moving the owner early does not
authorize any protected computation before evacuation has completed.

``core_reserve_drain()`` snapshots thread references under RCU and performs
migration outside that read-side section. ``core_reserve_evict_task()`` then
distinguishes execution states under the task's PI and runqueue locks:

* A runnable queued task moves through ``move_queued_task()``.
* A task executing on the core is displaced by ``migration_cpu_stop`` and
  checked again on a subsequent scan.
* A blocked task receives a valid ``wake_cpu`` without illegally changing its
  recorded ``task_cpu`` during blocking.
* A migration-disabled task or pending affinity request requests another
  draining pass.
* A task without any legal unreserved CPU prevents success.
* Per-CPU kernel threads remain local, with their execution subsequently
  excluded. Existing migration-disabled regions or pending affinity work
  involving them must also finish before activation.

``sched_core_reserve_allowed()`` is a placement predicate. The owner can be
placed on its chosen logical CPU; unrelated ordinary tasks cannot be placed
on any claimed sibling. It deliberately permits per-CPU kernel placement,
because queuing such a thread locally is different from allowing it to run.
It also permits an incumbent migration-disabled task to finish on its original
CPU during ``DRAINING``. Neither exception bypasses the active execution gate.

The gate itself is ``sched_core_reserve_pick()``. Its essential decision is::

    if state is neither QUIESCING nor ACTIVE:
        return NULL        # normal scheduler selection
    next = this CPU's idle task
    if this is owner_cpu and owner is queued and eligible:
        next = owner
    return next

Read the actual eligibility checks as well as this simplified pseudocode:
the owner must belong to this runqueue, must not be delayed-dequeued, and
must pass its scheduling class's throttling callback. The call at the start
of ``__pick_next_task()`` uses ``put_prev_set_next_task()`` before returning
the chosen task. It does not replace a task after another class has already
committed its selection.

There is intentionally no condition saying "only reserve while owner is
runnable". When the owner sleeps, the reservation object still exists and
the gate selects idle on every sibling. This is the code that prevents an
attacker from using the core between two parts of the computation.

``core_reserve_ack()`` is separate from task selection. It records a bit only
after observing the published gate and an actual owner/idle context.
``sched_core_reserve_finish()`` invokes it from ``finish_task_switch()`` after
the previous task has stopped executing and the runqueue lock is released.
The reschedule IPI callback can acknowledge an already safe CPU. The setup
thread waits for every sibling's bit; an IPI request alone cannot complete
the transaction. The object's lifetime also identifies the acknowledgment
round: a new claim receives a new, initially empty acknowledgment mask.

``cpu_stop_reserve()`` in ``kernel/stop_machine.c`` supplies the other half of
that barrier. Each stopper has ``reserved`` and ``running`` flags protected
by its lock. Reservation fails if a callback is queued or running. Single
requests check under one stopper lock, paired requests check both before
queueing either, and complete batches preflight their mask while holding
``stop_cpus_mutex``. This prevents an admitted CPU from waiting forever for
a reserved sibling that can no longer enter a stop-machine rendezvous.

For affinity, follow ``__set_cpus_allowed_ptr_locked()`` in ``core.c`` into
``sched_core_reserve_affinity_check()``. The reservation lock spans checking
and committing the new affinity. ``sched_core_reserve_dest()`` filters the
destination. The lock is released before migration can sleep. When a
reservation is closing, new affinity transactions involving a task recorded
on that core are refused, so they cannot later depend on a frozen stopper.

For explicit disable, follow::

    sched_core_reserve_prctl(DISABLE)
      core_reserve_restore()
        __set_cpus_allowed_ptr(owner, saved_mask, SCA_CORE_RESERVE)
      core_reserve_remove()
        publish RELEASING; reopen stopper admission
        clear per-CPU and owner pointers; decrement claim count
        reschedule siblings; call_rcu(core_reserve_free)

Restoring the saved mask does not require moving the current owner away:
the selected CPU was required to belong to that original mask. Its affinity
is restored before the core is reopened. The requested-affinity pointer was
not replaced by the internal pin operation.

For death, ``sched_core_reserve_finish()`` recognizes ``TASK_DEAD`` and queues
``core_reserve_release_work()`` on the unbound ``system_dfl_wq``. The worker
takes the sleepable locks and calls ``core_reserve_remove()``. The dying
thread needs no affinity restoration because it cannot execute again.
``core_reserve_free()`` drops the owner reference and frees the masks after
the RCU grace period. Killing a process therefore needs no userspace cleanup
handler, and stale scheduler readers cannot observe freed reservation memory.

Synchronization and lifecycle
-----------------------------

The global mutex serializes the slow transactions. The raw reservation lock
serializes publication and affinity admission, and is acquired after task and
runqueue locks. Sleeping migration operations do not hold it. Scheduler readers
use RCU; state publication uses release/acquire ordering. A reservation object
is retired through ``call_rcu()`` and holds its owner reference until reclamation.

The principal lifetime rules are:

* All external affinity changes to an owner fail with ``EBUSY``, including a
  request to reapply its current mask. Incompatible changes to other tasks
  also fail. Internal migration-enable/disable transitions remain available.
* Scheduler policy/parameter changes through ``sched_setscheduler()`` and
  ``sched_setattr()`` are rejected for the owner. This does not freeze every
  scheduling attribute: ordinary nice adjustments are a separate interface.
* Every CPU online/offline operation is rejected while any claim exists, even
  for CPUs outside the reserved core. This deliberately broad policy keeps
  topology and the saved affinity's available CPUs stable until release.
* The owner cannot fork or clone while reserved: creation returns ``EBUSY``.
  Ownership is never inherited. ``execve()`` preserves the thread's ownership.
  The supported x86 configuration does not introduce architecture-specific
  heterogeneous CPU-compatibility affinity changes for this operation.
* Explicit disable restores affinity before removing ownership and reopening
  the core. If restoration fails, the reservation is retained and the error
  is returned.
* Exit releases only after the owner's ``TASK_DEAD`` final switch. The
  post-switch hook queues unbound cleanup work; it does not sleep while
  holding a runqueue lock. Release can therefore lag an exit notification.
  Sending ``SIGKILL`` alone is not proof that release has occurred.
  An unreaped dead owner does not prevent reuse: evacuation skips dead tasks
  even though their task structures may retain the old singleton affinity.

CPU stopper integration preserves multi-CPU rendezvous semantics. A batch that
includes a reserved CPU returns ``EBUSY`` before queueing any participant.
Single and paired synchronous requests similarly fail; asynchronous
``stop_one_cpu_nowait()`` returns false. Callers that maintain pending-work
flags must undo them when queueing is refused. Otherwise an unqueued migration
could leave a waiter permanently blocked.

Architecture-specific forced-affinity operations, CPU-compatibility changes,
and kernel code that directly overrides affinity outside the checked scheduler
paths are outside the supported experimental scope. In particular, some
compatibility-affinity callers assume success or only warn on rejection. Do
not use those operations during a reservation. The exclusions and global
restrictions are part of this prototype, not claims of complete scheduler or
subsystem auditing.

Building and validating
-----------------------

For an x86 test build from the repository root::

    make O=/tmp/core-reservation-build x86_64_defconfig
    scripts/config --file /tmp/core-reservation-build/.config \
        -e EXPERT -e SMP -e SCHED_SMT -d CPUSETS \
        -d SCHED_CORE -d SCHED_CLASS_EXT -d SCHED_PROXY_EXEC -d PREEMPT_RT \
        -e SCHED_CORE_RESERVATION
    make O=/tmp/core-reservation-build olddefconfig
    make O=/tmp/core-reservation-build -j"$(nproc)" bzImage

Boot this kernel in a disposable environment with at least two SMT cores.
Confirm sibling topology through
``/sys/devices/system/cpu/cpu*/topology/thread_siblings_list``. Build and run the
focused selftest there with ``CAP_SYS_NICE``::

    mkdir -p /tmp/core-reservation-selftests
    make -C tools/testing/selftests/sched \
        OUTPUT=/tmp/core-reservation-selftests all
    sudo /tmp/core-reservation-selftests/core_reservation

The test reports TAP and skips when the feature, capability or suitable
nonboot SMT core is unavailable. It checks argument validation, pinned-task
rejection, owner placement, contention, local and remote affinity, forbidden
forks, sleeping and stopped owners, repeated intervals, exit and ``SIGKILL``.
CPU offline rejection is opt-in because it attempts a real hotplug write::

    sudo env CORE_RESERVATION_TEST_HOTPLUG=1 \
        /tmp/core-reservation-selftests/core_reservation

Attacker workers start on separate siblings, then permit evacuation. They
sample their execution CPU between two reads of a shared reservation epoch,
published only after enable succeeds and cleared before disable. The epoch
check avoids attributing a delayed pre-enable CPU sample to a protected
interval. Workers must also make progress on the remaining CPUs. These tests
exercise userspace exclusion; they are not a proof that every kernel task and
architecture path has been audited.

A QEMU test can use a static BusyBox initramfs containing the statically linked
selftest, with an ``/init`` that mounts procfs/sysfs, runs the test and powers
off. Example guest parameters are::

    qemu-system-x86_64 -accel tcg,thread=multi \
        -cpu max,vendor=GenuineIntel -smp 4,sockets=1,cores=2,threads=2 \
        -m 1024 -kernel /tmp/core-reservation-build/arch/x86/boot/bzImage \
        -initrd /path/to/test-initramfs.cpio.gz \
        -append 'console=ttyS0 panic=-1' -nographic -no-reboot

Check the guest topology rather than assuming the emulator exposed SMT.
Record the kernel configuration, TAP output and kernel warnings for each run.
Successful compilation or an unsupported-host skip does not establish runtime
isolation; runtime results must come from a boot of the modified kernel.

Validation of this implementation
---------------------------------

The modified ``7.2.0-rc4`` kernel was built and booted on an x86-64 QEMU TCG
guest exposing two physical cores with two SMT threads each. The test reserved
CPUs 2 and 3 and left CPUs 0 and 1 available for other tasks. All 18 TAP cases
passed, including the opt-in hotplug check. The final run also enabled
``PROVE_LOCKING``, ``DEBUG_LOCK_ALLOC``, ``DEBUG_ATOMIC_SLEEP`` and
``DEBUG_PREEMPT``; it reported no kernel lockdep, atomic-sleep or preemption
warnings. The death tests acquired the core again while its previous owner
was still an unreaped zombie.

The affected scheduler, syscall, CPU-hotplug and stopper compilation units
also compiled with ``SCHED_CORE_RESERVATION`` disabled under x86-64 defconfig.
The selftest compiled statically with ``-Wall -Wextra -Werror``. These checks
cover the stated guest and tested paths; they are not a proof of correctness
for every subsystem, processor or workload.
