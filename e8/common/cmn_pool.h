#ifndef CMN_POOL_H__
#define CMN_POOL_H__

#include <stdint.h>

#include "cmn_timing.h"

/*
 * Shared spin/yield worker pool, moved unchanged from the E8 sampler so
 * that HAWK and E8 parallel variants pay identical dispatch and wait
 * costs.
 *
 * The pool is process-global and holds up to CMN_POOL_MAX_THREADS threads.
 * Idle threads spin on a generation counter (pause, with
 * sched_yield() every 1024 spins); __sync builtins order memory. A
 * dispatch publishes a job and bumps the generation; the calling thread
 * then does its own share (as worker 0) and calls cmn_pool_wait(), which
 * spins until every dispatched thread has finished. Pool thread u runs as
 * worker u + 1. The threads are joined at exit (atexit). Pool calls are
 * not reentrant: one controlling thread at a time.
 *
 * Usage:
 *
 *   cmn_pool_ensure(T - 1);               outside timed regions
 *   cmn_pool_dispatch(job, ctx, T - 1);   threads run job(ctx, 1..T-1)
 *   job(ctx, 0);                          the caller is worker 0
 *   cmn_pool_wait();
 *   cmn_pool_shrink(0);                   outside timed regions
 *
 * Sizing: cmn_pool_dispatch() only grows the pool (it starts missing
 * threads and never joins any), so a dispatch to fewer threads than the
 * pool holds leaves the surplus threads idle (spinning) and a pool that
 * is already large enough creates no thread inside the dispatch. Threads
 * are joined only by cmn_pool_shrink(), cmn_pool_ensure() and
 * cmn_pool_shutdown(). Harnesses size the pool with cmn_pool_ensure()
 * before a timed block and shrink it after, so that no timed call
 * creates or joins a thread; every scheme follows the same rule.
 *
 * Accounting (off by default, cmn_pool_set_accounting()): each pool thread
 * reads its thread CPU clock and core-cycle counter around every job and
 * the caller sums them when the dispatch completes. This costs two clock
 * reads and two counter reads per thread per dispatch, inside the
 * dispatch; it is applied identically to every scheme. Without
 * accounting, a measurement spanning a dispatch reports the workers'
 * share as unavailable.
 *
 * Affinity (off by default, cmn_pool_set_affinity()): with a CPU list
 * c[0..count-1], pool thread u (worker u + 1) is pinned to
 * c[(u + 1) % count], so that a caller pinned to c[0] with
 * cmn_pin_current_thread() and T <= count workers run on T distinct
 * CPUs. Pinning happens outside dispatches (when the list is set, and
 * when a thread is created) and costs nothing per dispatch; it applies
 * to every scheme alike.
 */

#define CMN_POOL_MAX_THREADS   24

typedef void (*cmn_pool_job)(void *ctx, unsigned worker);

/*
 * Resize the pool to exactly threads pool threads (excluding the caller),
 * joining surplus threads and starting missing ones; the explicit resize
 * for harnesses, outside timed regions. Returns 0 if threads exceeds
 * CMN_POOL_MAX_THREADS, a dispatch is outstanding, or a thread cannot be
 * started (the pool then keeps the threads it had after any shrink).
 */
int cmn_pool_ensure(unsigned threads);

/*
 * Join pool threads beyond the first threads; no-op if the pool is
 * smaller. cmn_pool_shrink(0) joins every thread.
 */
void cmn_pool_shrink(unsigned threads);

/*
 * Join every pool thread and reset the pool (also registered with
 * atexit()).
 */
void cmn_pool_shutdown(void);

/*
 * Current number of pool threads (excluding the caller).
 */
unsigned cmn_pool_thread_count(void);

/*
 * Run job(ctx, w) on pool threads for w = 1 .. active_threads, first
 * starting pool threads up to active_threads if the pool is smaller
 * (grow only; surplus threads stay idle). Returns 0 on error (no job is
 * then running).
 */
int cmn_pool_dispatch(cmn_pool_job job, void *ctx, unsigned active_threads);

/*
 * Wait for the outstanding dispatch to finish. Returns 0 if the pool was
 * shut down meanwhile.
 */
int cmn_pool_wait(void);

/*
 * Enable or disable per-worker accounting; returns 0 (and changes
 * nothing) while a dispatch is outstanding.
 */
int cmn_pool_set_accounting(int enable);

int cmn_pool_get_accounting(void);

/*
 * Pin pool threads to the CPU list cpus[0..count-1] as described above:
 * the live threads at once, later threads at their creation. count = 0
 * clears the list (threads created afterwards are not pinned; live
 * threads keep their affinity). Returns 0, and changes nothing, if count
 * exceeds CMN_POOL_MAX_THREADS + 1, a CPU number is not below the number
 * of configured CPUs, or a dispatch is outstanding; returns 0 if a live
 * thread cannot be pinned (the list is then set and the other live
 * threads pinned). A CPU outside the process's allowed set makes pinning,
 * and so cmn_pool_ensure() or a growing dispatch, fail.
 */
int cmn_pool_set_affinity(const unsigned *cpus, unsigned count);

/*
 * The current CPU list: writes up to max entries to cpus and returns the
 * list length (0 if none is set).
 */
unsigned cmn_pool_get_affinity(unsigned *cpus, unsigned max);

/*
 * Pin the calling thread to one CPU; returns 0 on failure (the affinity
 * is then unchanged).
 */
int cmn_pin_current_thread(unsigned cpu);

/*
 * Pool threads' share of the last completed dispatch: worker count, CPU
 * time and core cycles summed over the dispatched pool threads (not the
 * caller). cpu_ns and core_cycles are CMN_UNAVAILABLE without accounting;
 * core_cycles is also CMN_UNAVAILABLE without core-cycle counters.
 */
typedef struct {
	unsigned workers;
	uint64_t cpu_ns;
	uint64_t core_cycles;
} cmn_pool_dispatch_acct;

void cmn_pool_last_dispatch(cmn_pool_dispatch_acct *acct);

/*
 * Cumulative pool statistics since start or cmn_pool_stats_reset():
 *
 *   dispatches              completed dispatches
 *   unaccounted_dispatches  completed dispatches without accounting
 *   threads_started         pool threads created
 *   threads_joined          pool threads joined (shrink or shutdown)
 *   job_cpu_ns              pool threads' CPU time inside jobs
 *   job_core_cycles         pool threads' core cycles inside jobs, or
 *                           CMN_UNAVAILABLE
 *   thread_cpu_ns           all CPU time of pool threads, live and joined
 *   idle_cpu_ns             thread_cpu_ns - job_cpu_ns: CPU spent spinning
 *                           between jobs, meaningful when accounting was on
 *                           for every dispatch
 *
 * thread_cpu_ns and idle_cpu_ns read every live thread's CPU clock; call
 * cmn_pool_stats_get() outside timed regions.
 */
typedef struct {
	uint64_t dispatches;
	uint64_t unaccounted_dispatches;
	uint64_t threads_started;
	uint64_t threads_joined;
	uint64_t job_cpu_ns;
	uint64_t job_core_cycles;
	uint64_t thread_cpu_ns;
	uint64_t idle_cpu_ns;
} cmn_pool_stats;

void cmn_pool_stats_get(cmn_pool_stats *stats);

void cmn_pool_stats_reset(void);

/*
 * Measurement of one call that may dispatch to the pool (cmn_timing.h):
 * cpu_ns_all_threads is the calling thread's CPU time over the call plus
 * the pool threads' job CPU time for the dispatches completed during it,
 * and likewise for core_cycles. Spinning between dispatches is excluded
 * (see idle_cpu_ns). If a dispatch during the call ran without
 * accounting, both are CMN_UNAVAILABLE.
 */
typedef struct {
	cmn_stamp stamp;
	uint64_t dispatches;
	uint64_t unaccounted_dispatches;
	uint64_t job_cpu_ns;
	uint64_t job_core_cycles;
} cmn_pool_stamp;

void cmn_pool_measure_start(cmn_pool_stamp *start);

void cmn_pool_measure_stop(cmn_measure *m, const cmn_pool_stamp *start);

#endif
