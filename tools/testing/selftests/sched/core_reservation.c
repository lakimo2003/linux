// SPDX-License-Identifier: GPL-2.0-only
/*
 * Exercise the experimental, privileged whole-core reservation interface.
 * Run on a disposable machine with at least two SMT cores. CPU hotplug tests
 * require an explicit CORE_RESERVATION_TEST_HOTPLUG=1 environment variable.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../kselftest.h"

/* Keep the test buildable against installed headers without this private API. */
#ifndef PR_SCHED_CORE_RESERVE
#define PR_SCHED_CORE_RESERVE	0x43525356
#define PR_SCHED_CORE_RESERVE_GET	0
#define PR_SCHED_CORE_RESERVE_ENABLE	1
#define PR_SCHED_CORE_RESERVE_DISABLE	2
#endif

#define WORKERS 2
#define TESTS 18

enum command {
	CMD_NONE,
	CMD_CLAIM,
	CMD_AFFINITY,
	CMD_STOP,
	CMD_HOTPLUG,
};

struct shared_state {
	atomic_uint epoch;
	atomic_uint violations;
	atomic_ulong progress[WORKERS];
	atomic_uint ready[WORKERS];
	atomic_uint command;
	atomic_uint done;
	atomic_int result;
	atomic_uint pinned_ready;
	cpu_set_t reserved;
	pid_t owner;
	int cpu;
	int sibling;
};

static struct shared_state *shared;
static cpu_set_t original;
static pid_t workers[WORKERS], helper = -1, pinned = -1, victim = -1;
static unsigned int epoch;

static int reserve(unsigned int op, unsigned long cpu)
{
	return prctl(PR_SCHED_CORE_RESERVE, (unsigned long)op, cpu, 0UL, 0UL);
}

static void delay_ms(unsigned int ms)
{
	struct timespec ts = { .tv_sec = ms / 1000,
			       .tv_nsec = (ms % 1000) * 1000000UL };

	while (nanosleep(&ts, &ts) && errno == EINTR)
		;
}

static void kill_wait(pid_t *pid)
{
	if (*pid > 0) {
		kill(*pid, SIGKILL);
		while (waitpid(*pid, NULL, 0) < 0 && errno == EINTR)
			;
		*pid = -1;
	}
}

static void cleanup(void)
{
	int i;

	if (shared)
		atomic_store(&shared->epoch, 0);
	reserve(PR_SCHED_CORE_RESERVE_DISABLE, 0);
	kill_wait(&pinned);
	kill_wait(&victim);
	kill_wait(&helper);
	for (i = 0; i < WORKERS; i++)
		kill_wait(&workers[i]);
}

static void timed_out(int sig)
{
	static const char msg[] = "core_reservation: timed out\n";
	int i;
	ssize_t written;

	(void)sig;
	if (helper > 0)
		kill(helper, SIGKILL);
	if (pinned > 0)
		kill(pinned, SIGKILL);
	if (victim > 0)
		kill(victim, SIGKILL);
	for (i = 0; i < WORKERS; i++)
		if (workers[i] > 0)
			kill(workers[i], SIGKILL);
	written = write(STDERR_FILENO, msg, sizeof(msg) - 1);
	(void)written;
	_exit(KSFT_FAIL);
}

static bool wait_value(atomic_uint *value, unsigned int expected)
{
	unsigned int tries;

	for (tries = 0; tries < 5000; tries++) {
		if (atomic_load(value) == expected)
			return true;
		delay_ms(1);
	}
	return false;
}

static bool read_siblings(int cpu, cpu_set_t *mask)
{
	char path[160], line[4096], *p, *end;
	long first, last;
	FILE *file;

	snprintf(path, sizeof(path),
		 "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu);
	file = fopen(path, "r");
	if (!file)
		return false;
	if (!fgets(line, sizeof(line), file)) {
		fclose(file);
		return false;
	}
	fclose(file);
	CPU_ZERO(mask);
	p = line;
	while (*p && *p != '\n') {
		first = strtol(p, &end, 10);
		if (p == end || first < 0 || first >= CPU_SETSIZE)
			return false;
		p = end;
		last = first;
		if (*p == '-') {
			last = strtol(p + 1, &end, 10);
			if (end == p + 1 || last < first || last >= CPU_SETSIZE)
				return false;
			p = end;
		}
		for (; first <= last; first++)
			CPU_SET(first, mask);
		if (*p == ',')
			p++;
		else if (*p && *p != '\n')
			return false;
	}
	return true;
}

static int choose_core(cpu_set_t *siblings, int *housekeeping)
{
	cpu_set_t outside, allowed;
	int cpu, other;

	for (cpu = 1; cpu < CPU_SETSIZE; cpu++) {
		if (!CPU_ISSET(cpu, &original) || !read_siblings(cpu, siblings) ||
		    CPU_ISSET(0, siblings) || CPU_COUNT(siblings) < 2)
			continue;
		CPU_AND(&allowed, siblings, &original);
		if (!CPU_EQUAL(&allowed, siblings))
			continue;
		CPU_XOR(&outside, &original, siblings);
		for (other = 0; other < CPU_SETSIZE; other++) {
			if (CPU_ISSET(other, &outside)) {
				*housekeeping = other;
				return cpu;
			}
		}
	}
	return -1;
}

static void attacker(unsigned int index)
{
	cpu_set_t initial;
	unsigned int before, after, n = 0;
	int cpu;

	/* Start incumbents on distinct siblings, then restore their movable
	 * requested affinity before telling the supervisor they are ready.
	 */
	CPU_ZERO(&initial);
	for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
		if (CPU_ISSET(cpu, &shared->reserved) && n++ == index) {
			CPU_SET(cpu, &initial);
			break;
		}
	}
	if (sched_setaffinity(0, sizeof(initial), &initial))
		_exit(1);
	if (sched_setaffinity(0, sizeof(original), &original))
		_exit(1);
	atomic_store(&shared->ready[index], 1);
	for (;;) {
		/*
		 * Publish the epoch only after enable has returned. Sampling a CPU
		 * between two equal nonzero epochs cannot attribute a pre-enable
		 * sample to the protected interval, even if this task is migrated
		 * or descheduled between its sample and its counter update.
		 */
		before = atomic_load(&shared->epoch);
		cpu = sched_getcpu();
		after = atomic_load(&shared->epoch);
		if (before && before == after && cpu >= 0 &&
		    CPU_ISSET(cpu, &shared->reserved))
			atomic_fetch_add(&shared->violations, 1);
		atomic_fetch_add(&shared->progress[index], 1);
	}
}

static bool hotplug_rejected(void)
{
	char path[128];
	int fd, ret, saved_errno;

	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/online",
		 shared->cpu);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return false;
	ret = write(fd, "0", 1);
	saved_errno = errno;
	close(fd);
	if (ret == 1) {
		/* Restore the CPU if a broken implementation permitted removal. */
		fd = open(path, O_WRONLY);
		if (fd >= 0) {
			ret = write(fd, "1", 1);
			(void)ret;
			close(fd);
		}
		return false;
	}
	return saved_errno == EBUSY;
}

static void assistant(void)
{
	unsigned int cmd;
	int ret;

	/* Keep the target in our requested mask so contention, not affinity
	 * validation, determines the outcome of an attempted claim.
	 */
	if (sched_setaffinity(0, sizeof(original), &original))
		_exit(1);
	for (;;) {
		cmd = atomic_load(&shared->command);
		if (!cmd) {
			delay_ms(1);
			continue;
		}
		switch (cmd) {
		case CMD_CLAIM:
			ret = reserve(PR_SCHED_CORE_RESERVE_ENABLE, shared->sibling);
			ret = ret < 0 && errno == EBUSY;
			/* Also undo an unexpected successful claim. */
			reserve(PR_SCHED_CORE_RESERVE_DISABLE, 0);
			break;
		case CMD_AFFINITY:
			ret = sched_setaffinity(shared->owner, sizeof(original), &original);
			ret = ret < 0 && errno == EBUSY;
			break;
		case CMD_STOP:
			ret = kill(shared->owner, SIGSTOP);
			delay_ms(100);
			if (!ret) {
				ret = reserve(PR_SCHED_CORE_RESERVE_ENABLE, shared->sibling);
				ret = ret < 0 && errno == EBUSY;
				reserve(PR_SCHED_CORE_RESERVE_DISABLE, 0);
			} else {
				ret = false;
			}
			kill(shared->owner, SIGCONT);
			break;
		case CMD_HOTPLUG:
			ret = hotplug_rejected();
			break;
		default:
			ret = false;
		}
		atomic_store(&shared->result, ret);
		atomic_store(&shared->command, CMD_NONE);
		atomic_store(&shared->done, 1);
	}
}

static bool helper_command(unsigned int cmd)
{
	atomic_store(&shared->done, 0);
	atomic_store(&shared->command, cmd);
	return wait_value(&shared->done, 1) && atomic_load(&shared->result);
}

static bool unprivileged_rejected(void)
{
	struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3 };
	struct __user_cap_data_struct data[2] = { };
	pid_t child = fork();
	int status;

	if (child < 0)
		return false;
	if (!child) {
		if (syscall(SYS_capset, &header, data))
			_exit(1);
		_exit(reserve(PR_SCHED_CORE_RESERVE_ENABLE, shared->cpu) < 0 &&
		      errno == EPERM ? 0 : 1);
	}
	return waitpid(child, &status, 0) == child && WIFEXITED(status) &&
	       !WEXITSTATUS(status);
}

static bool pinned_rejected(void)
{
	cpu_set_t mask;
	bool ok;
	int ret;

	pinned = fork();
	if (pinned < 0)
		return false;
	if (!pinned) {
		CPU_ZERO(&mask);
		CPU_SET(shared->cpu, &mask);
		if (sched_setaffinity(0, sizeof(mask), &mask))
			_exit(1);
		atomic_store(&shared->pinned_ready, 1);
		for (;;)
			asm volatile("" ::: "memory");
	}
	if (!wait_value(&shared->pinned_ready, 1)) {
		kill_wait(&pinned);
		return false;
	}
	ret = reserve(PR_SCHED_CORE_RESERVE_ENABLE, shared->cpu);
	ok = ret < 0 && errno == EBUSY;
	if (!ret)
		reserve(PR_SCHED_CORE_RESERVE_DISABLE, 0);
	kill_wait(&pinned);
	return ok;
}

static bool lifecycle_releases(bool fatal)
{
	int pipefd[2], status = 0, tries;
	siginfo_t info = { };
	char ready = 0;
	bool ok, released = false;

	if (pipe(pipefd))
		return false;
	victim = fork();
	if (victim < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return false;
	}
	if (!victim) {
		close(pipefd[0]);
		if (reserve(PR_SCHED_CORE_RESERVE_ENABLE, shared->cpu))
			_exit(1);
		ready = 1;
		if (write(pipefd[1], &ready, 1) != 1)
			_exit(1);
		close(pipefd[1]);
		if (fatal)
			for (;;)
				pause();
		_exit(0);
	}
	close(pipefd[1]);
	ok = read(pipefd[0], &ready, 1) == 1 && ready == 1;
	close(pipefd[0]);
	if (fatal && ok)
		kill(victim, SIGKILL);
	if (!ok)
		kill(victim, SIGKILL);
	/* Reclaim before reaping: a dead owner's old affinity must not block us. */
	ok &= !waitid(P_PID, victim, &info, WEXITED | WNOWAIT);
	ok &= fatal ? info.si_code == CLD_KILLED && info.si_status == SIGKILL :
		      info.si_code == CLD_EXITED && !info.si_status;
	if (ok) {
		/* Cleanup may complete after the remote exit notification. */
		for (tries = 0; tries < 1000; tries++) {
			if (!reserve(PR_SCHED_CORE_RESERVE_ENABLE, shared->cpu)) {
				released = !reserve(PR_SCHED_CORE_RESERVE_DISABLE, 0);
				break;
			}
			if (errno != EBUSY)
				break;
			delay_ms(1);
		}
	}
	ok &= waitpid(victim, &status, 0) == victim;
	victim = -1;
	return ok && released;
}

int main(void)
{
	struct __user_cap_header_struct header = { .version = _LINUX_CAPABILITY_VERSION_3 };
	struct __user_cap_data_struct caps[2];
	cpu_set_t siblings, effective;
	unsigned long progress[WORKERS];
	int housekeeping, cpu, i, ret, status;
	unsigned int violations;
	pid_t child;
	bool ok;

	ksft_print_header();
	if (reserve(PR_SCHED_CORE_RESERVE_GET, 0) < 0 &&
	    (errno == EINVAL || errno == EOPNOTSUPP))
		ksft_exit_skip("CONFIG_SCHED_CORE_RESERVATION is unavailable\n");
	if (syscall(SYS_capget, &header, caps) ||
	    !(caps[CAP_SYS_NICE / 32].effective & (1U << (CAP_SYS_NICE % 32))))
		ksft_exit_skip("CAP_SYS_NICE is required\n");
	if (sched_getaffinity(0, sizeof(original), &original))
		ksft_exit_fail_msg("sched_getaffinity: %s\n", strerror(errno));
	cpu = choose_core(&siblings, &housekeeping);
	if (cpu < 0)
		ksft_exit_skip("need an allowed nonboot SMT core and a housekeeping CPU\n");
	shared = mmap(NULL, sizeof(*shared), PROT_READ | PROT_WRITE,
		      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shared == MAP_FAILED) {
		shared = NULL;
		ksft_exit_fail_msg("mmap: %s\n", strerror(errno));
	}
	shared->reserved = siblings;
	shared->owner = getpid();
	shared->cpu = cpu;
	for (i = 0; i < CPU_SETSIZE; i++)
		if (i != cpu && CPU_ISSET(i, &siblings)) {
			shared->sibling = i;
			break;
		}
	atexit(cleanup);
	signal(SIGALRM, timed_out);
	alarm(60);
	ksft_set_plan(TESTS);
	ksft_print_msg("reserving CPU %d, sibling %d; housekeeping CPU %d\n",
		       cpu, shared->sibling, housekeeping);
	ksft_test_result(reserve(PR_SCHED_CORE_RESERVE_GET, 0) == 0,
			 "initial reservation is absent\n");
	ok = prctl(PR_SCHED_CORE_RESERVE, 99UL, 0UL, 0UL, 0UL) < 0 && errno == EINVAL;
	ok &= prctl(PR_SCHED_CORE_RESERVE, PR_SCHED_CORE_RESERVE_GET,
		    0UL, 1UL, 0UL) < 0 && errno == EINVAL;
	ksft_test_result(ok, "invalid commands and reserved arguments are rejected\n");
	ksft_test_result(unprivileged_rejected(), "enable requires CAP_SYS_NICE\n");
	ksft_test_result(reserve(PR_SCHED_CORE_RESERVE_ENABLE, ~0UL) < 0 &&
			 errno == EINVAL, "invalid CPU is rejected\n");
	ksft_test_result(pinned_rejected(), "pinned outsider prevents reservation\n");

	helper = fork();
	if (helper < 0)
		ksft_exit_fail_msg("fork helper: %s\n", strerror(errno));
	if (!helper)
		assistant();
	for (i = 0; i < WORKERS; i++) {
		workers[i] = fork();
		if (workers[i] < 0)
			ksft_exit_fail_msg("fork attacker: %s\n", strerror(errno));
		if (!workers[i])
			attacker(i);
		if (!wait_value(&shared->ready[i], 1))
			ksft_exit_fail_msg("attacker did not start\n");
	}
	delay_ms(50);
	ret = reserve(PR_SCHED_CORE_RESERVE_ENABLE, cpu);
	if (ret) {
		ksft_test_result_fail("enable failed: %s\n", strerror(errno));
		ksft_exit_fail_msg("cannot exercise active reservation\n");
	}
	atomic_store(&shared->epoch, ++epoch);
	CPU_ZERO(&effective);
	ok = !sched_getaffinity(0, sizeof(effective), &effective) &&
	     CPU_COUNT(&effective) == 1 && CPU_ISSET(cpu, &effective) &&
	     sched_getcpu() == cpu &&
	     reserve(PR_SCHED_CORE_RESERVE_GET, 0) == cpu + 1;
	ksft_test_result(ok, "successful enable publishes owner placement and reservation\n");
	ksft_test_result(reserve(PR_SCHED_CORE_RESERVE_ENABLE, cpu) < 0 &&
			 errno == EALREADY, "owner cannot enable twice\n");
	ksft_test_result(helper_command(CMD_CLAIM), "sibling claim observes existing owner\n");
	ksft_test_result(sched_setaffinity(0, sizeof(original), &original) < 0 &&
			 errno == EBUSY, "local affinity change cannot break reservation\n");
	ksft_test_result(helper_command(CMD_AFFINITY),
			 "remote affinity change cannot break reservation\n");
	child = fork();
	status = errno;
	if (!child)
		_exit(0);
	if (child > 0) {
		/* A wrongly admitted child may be stranded on the owner CPU. */
		atomic_store(&shared->epoch, 0);
		reserve(PR_SCHED_CORE_RESERVE_DISABLE, 0);
		kill(child, SIGKILL);
		waitpid(child, NULL, 0);
		ksft_test_result_fail("fork is rejected during reservation\n");
		ksft_exit_fail_msg("unexpected fork success; reservation released\n");
	}
	ksft_test_result(child < 0 && status == EBUSY, "fork is rejected during reservation\n");
	for (i = 0; i < WORKERS; i++)
		progress[i] = atomic_load(&shared->progress[i]);
	violations = atomic_load(&shared->violations);
	delay_ms(200);
	ok = !violations && !atomic_load(&shared->violations);
	for (i = 0; i < WORKERS; i++)
		ok &= atomic_load(&shared->progress[i]) > progress[i];
	ksft_test_result(ok,
			 "attackers execute elsewhere while sleeping owner retains whole core\n");
	ksft_test_result(helper_command(CMD_STOP) &&
			 !atomic_load(&shared->violations),
			 "SIGSTOP preserves ownership and core exclusion\n");
	if (getenv("CORE_RESERVATION_TEST_HOTPLUG"))
		ksft_test_result(helper_command(CMD_HOTPLUG),
				 "CPU hotplug cannot break reservation\n");
	else
		ksft_test_result_skip("CPU hotplug test needs CORE_RESERVATION_TEST_HOTPLUG=1\n");
	atomic_store(&shared->epoch, 0);
	ok = !reserve(PR_SCHED_CORE_RESERVE_DISABLE, 0) &&
	     reserve(PR_SCHED_CORE_RESERVE_GET, 0) == 0 &&
	     !sched_getaffinity(0, sizeof(effective), &effective) &&
	     CPU_EQUAL(&original, &effective);
	ksft_test_result(ok, "disable releases core and restores exact prior affinity\n");
	ok = true;
	for (i = 0; i < 10; i++) {
		if (reserve(PR_SCHED_CORE_RESERVE_ENABLE, cpu)) {
			ok = false;
			break;
		}
		atomic_store(&shared->epoch, ++epoch);
		delay_ms(2);
		atomic_store(&shared->epoch, 0);
		if (reserve(PR_SCHED_CORE_RESERVE_DISABLE, 0)) {
			ok = false;
			break;
		}
	}
	ksft_test_result(ok && !atomic_load(&shared->violations),
			 "repeated reservations preserve exclusion and release\n");
	ksft_test_result(lifecycle_releases(false), "normal owner exit releases core\n");
	ksft_test_result(lifecycle_releases(true), "SIGKILL owner death releases core\n");
	ksft_finished();
}
