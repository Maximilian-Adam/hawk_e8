#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#endif

#include "cmn_timing.h"

/*
 * Process-wide core-cycle counter state (see cmn_core_cycles_available()).
 * The first failure is recorded once: the thread that moves the state
 * from untested to unavailable also writes the reason.
 */
#define CMN_CORE_UNTESTED      (-1)
#define CMN_CORE_UNAVAILABLE   0
#define CMN_CORE_AVAILABLE     1

static volatile int cmn_core_state = CMN_CORE_UNTESTED;
static char cmn_core_reason[96];

/*
 * Counter of the calling thread: -1 if not open, -2 if this thread found
 * the counter unavailable.
 */
static __thread int cmn_core_fd = -1;

/* see cmn_timing.h */
uint64_t
cmn_wall_ns(void)
{
	struct timespec ts;

#if defined(CLOCK_MONOTONIC_RAW)
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
		return 0;
	}
#else
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
#endif
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
		+ (uint64_t)ts.tv_nsec;
}

/* see cmn_timing.h */
uint64_t
cmn_thread_cpu_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0) {
		return 0;
	}
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
		+ (uint64_t)ts.tv_nsec;
}

/* see cmn_timing.h */
uint64_t
cmn_delta(uint64_t start, uint64_t end)
{
	if (start == 0 && end == 0) {
		return 0;
	}
	if (end <= start) {
		return 1;
	}
	return end - start;
}

static void
cmn_core_mark_unavailable(const char *what, int err)
{
	if (__sync_bool_compare_and_swap(&cmn_core_state,
		CMN_CORE_UNTESTED, CMN_CORE_UNAVAILABLE))
	{
		if (err != 0) {
			snprintf(cmn_core_reason, sizeof cmn_core_reason,
				"%s: %s", what, strerror(err));
		} else {
			snprintf(cmn_core_reason, sizeof cmn_core_reason,
				"%s", what);
		}
	}
	cmn_core_fd = -2;
}

static int
cmn_core_read_fd(int fd, uint64_t *value)
{
	return read(fd, value, sizeof *value) == (ssize_t)sizeof *value;
}

/* see cmn_timing.h */
int
cmn_core_cycles_open(void)
{
#if defined(__linux__) && defined(SYS_perf_event_open)
	struct perf_event_attr attr;
	volatile uint64_t spin = 0;
	uint64_t x0, x1;
	int fd;

	if (cmn_core_fd >= 0) {
		return 1;
	}
	if (cmn_core_fd == -2 || cmn_core_state == CMN_CORE_UNAVAILABLE) {
		cmn_core_fd = -2;
		return 0;
	}
	memset(&attr, 0, sizeof attr);
	attr.size = sizeof attr;
	attr.type = PERF_TYPE_HARDWARE;
	attr.config = PERF_COUNT_HW_CPU_CYCLES;
	attr.exclude_kernel = 1;
	attr.exclude_hv = 1;
	fd = (int)syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0);
	if (fd < 0) {
		cmn_core_mark_unavailable("perf_event_open", errno);
		return 0;
	}

	/*
	 * Some virtual machines open the counter but never advance it;
	 * treat that as unavailable rather than report zero cycles.
	 */
	if (!cmn_core_read_fd(fd, &x0)) {
		close(fd);
		cmn_core_mark_unavailable("read", errno);
		return 0;
	}
	for (unsigned u = 0; u < 10000; u ++) {
		spin += u;
	}
	if (!cmn_core_read_fd(fd, &x1)) {
		close(fd);
		cmn_core_mark_unavailable("read", errno);
		return 0;
	}
	if (x1 <= x0) {
		close(fd);
		cmn_core_mark_unavailable("counter does not count", 0);
		return 0;
	}
	cmn_core_fd = fd;
	(void)__sync_bool_compare_and_swap(&cmn_core_state,
		CMN_CORE_UNTESTED, CMN_CORE_AVAILABLE);
	return 1;
#else
	cmn_core_mark_unavailable("perf_event_open not supported", 0);
	return 0;
#endif
}

/* see cmn_timing.h */
uint64_t
cmn_core_cycles_read(void)
{
	uint64_t x;

	if (cmn_core_fd < 0 && !cmn_core_cycles_open()) {
		return CMN_UNAVAILABLE;
	}
	if (!cmn_core_read_fd(cmn_core_fd, &x)) {
		return CMN_UNAVAILABLE;
	}
	return x;
}

/* see cmn_timing.h */
void
cmn_core_cycles_close(void)
{
	if (cmn_core_fd >= 0) {
		close(cmn_core_fd);
	}
	cmn_core_fd = -1;
}

/* see cmn_timing.h */
int
cmn_core_cycles_available(void)
{
	return cmn_core_state;
}

/* see cmn_timing.h */
const char *
cmn_core_cycles_status(void)
{
	static char buf[sizeof cmn_core_reason + 16];

	switch (cmn_core_state) {
	case CMN_CORE_AVAILABLE:
		return "available";
	case CMN_CORE_UNAVAILABLE:
		snprintf(buf, sizeof buf, "unavailable: %s", cmn_core_reason);
		return buf;
	default:
		return "untested";
	}
}

/* see cmn_timing.h */
void
cmn_measure_start(cmn_stamp *start)
{
	start->cpu_ns = cmn_thread_cpu_ns();
	start->core = cmn_core_cycles_read();
	start->wall_ns = cmn_wall_ns();
	start->tsc = cmn_tsc_start();
}

/* see cmn_timing.h */
void
cmn_measure_stop(cmn_measure *m, const cmn_stamp *start)
{
	uint64_t tsc = cmn_tsc_end();
	uint64_t wall = cmn_wall_ns();
	uint64_t core = cmn_core_cycles_read();
	uint64_t cpu = cmn_thread_cpu_ns();

	m->tsc_cycles = cmn_delta(start->tsc, tsc);
	m->wall_ns = cmn_delta(start->wall_ns, wall);
	if (start->core == CMN_UNAVAILABLE || core == CMN_UNAVAILABLE) {
		m->core_cycles = CMN_UNAVAILABLE;
	} else {
		m->core_cycles = cmn_delta(start->core, core);
	}
	m->cpu_ns_all_threads = cmn_delta(start->cpu_ns, cpu);
}
