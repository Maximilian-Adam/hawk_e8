#ifndef CMN_TIMING_H__
#define CMN_TIMING_H__

#include <stdint.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

/*
 * Shared timing module for the HAWK and HAWK-E8-CM measurements.
 *
 * Four clocks:
 *
 *   TSC          start = lfence; rdtsc    end = rdtscp; lfence
 *   wall clock   CLOCK_MONOTONIC_RAW (CLOCK_MONOTONIC if unavailable)
 *   thread CPU   CLOCK_THREAD_CPUTIME_ID of the calling thread
 *   core cycles  perf_event_open(PERF_TYPE_HARDWARE,
 *                PERF_COUNT_HW_CPU_CYCLES), user space only
 *                (exclude_kernel = 1, exclude_hv = 1), one counter per
 *                thread, opened in and read by that thread with read()
 *
 * Core cycles are optional. If the counter cannot be opened, or opens but
 * does not count, the failure is recorded once (see
 * cmn_core_cycles_status()), no further thread tries again, and every
 * reading is CMN_UNAVAILABLE. Nothing aborts and the TSC is never
 * substituted. Readers write CMN_UNAVAILABLE as an empty CSV field.
 */

/*
 * Marker for an unavailable reading or measurement.
 */
#define CMN_UNAVAILABLE   UINT64_MAX

/*
 * Fenced TSC readings. They are inline so that taking a timestamp adds no
 * call to the measured interval. They return 0 on platforms without a
 * TSC.
 */
static inline uint64_t
cmn_tsc_start(void)
{
#if defined(__x86_64__)
	_mm_lfence();
	return __rdtsc();
#else
	return 0;
#endif
}

static inline uint64_t
cmn_tsc_end(void)
{
#if defined(__x86_64__)
	unsigned int aux;
	uint64_t x;

	x = __rdtscp(&aux);
	_mm_lfence();
	return x;
#else
	return 0;
#endif
}

/*
 * Wall clock and calling-thread CPU time, in nanoseconds; 0 on failure.
 */
uint64_t cmn_wall_ns(void);

uint64_t cmn_thread_cpu_ns(void);

/*
 * Difference end - start of two readings of one clock: 0 if both readings
 * are 0 (clock unavailable), 1 if end <= start, so that a successful
 * measurement is never reported as zero.
 */
uint64_t cmn_delta(uint64_t start, uint64_t end);

/*
 * Per-thread core-cycle counter. cmn_core_cycles_open() opens the counter
 * of the calling thread (it is a no-op if already open) and returns 1 if
 * it is available; call it outside any timed region. cmn_core_cycles_read()
 * returns the counter of the calling thread, opening it on first use, or
 * CMN_UNAVAILABLE. cmn_core_cycles_close() releases the counter of the
 * calling thread; threads that exit must call it.
 */
int cmn_core_cycles_open(void);

uint64_t cmn_core_cycles_read(void);

void cmn_core_cycles_close(void);

/*
 * Process-wide counter state: 1 if a counter has been opened and counts,
 * 0 if the counter is unavailable, -1 if no thread has tried yet.
 * cmn_core_cycles_status() describes it ("available", "untested", or
 * "unavailable: <reason>").
 */
int cmn_core_cycles_available(void);

const char *cmn_core_cycles_status(void);

/*
 * A measurement of one call on the calling thread. cmn_measure_start()
 * takes the four readings (thread CPU, core cycles, wall clock, then TSC
 * last); cmn_measure_stop() takes them in the reverse order and stores the
 * differences. cpu_ns_all_threads is then the calling thread's CPU time
 * only; cmn_pool_measure_stop() (cmn_pool.h) adds the pool workers' share.
 * core_cycles is CMN_UNAVAILABLE if the counter is unavailable.
 */
typedef struct {
	uint64_t tsc;
	uint64_t core;
	uint64_t wall_ns;
	uint64_t cpu_ns;
} cmn_stamp;

typedef struct {
	uint64_t tsc_cycles;
	uint64_t core_cycles;
	uint64_t wall_ns;
	uint64_t cpu_ns_all_threads;
} cmn_measure;

void cmn_measure_start(cmn_stamp *start);

void cmn_measure_stop(cmn_measure *m, const cmn_stamp *start);

#endif
