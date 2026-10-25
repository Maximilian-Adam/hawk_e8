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
 * Idle threads spin on a generation counter (_mm_pause, with
 * sched_yield() every 1024 spins); __sync builtins order memory. A
 * dispatch publishes a job and bumps the generation; the calling thread
 * then does its own share (as worker 0) and calls cmn_pool_wait(), which
 * spins until every dispatched thread has finished. Pool thread u runs as
 * worker u + 1. The threads are joined at exit (atexit). Pool calls are
 * not reentrant: one controlling thread at a time.
 *
 * Usage:
 *
 *   cmn_pool_dispatch(job, ctx, T - 1);   threads run job(ctx, 1..T-1)
 *   job(ctx, 0);                          the caller is worker 0
 *   cmn_pool_wait();
 *
 * Accounting (off by default, cmn_pool_set_accounting()): each pool thread
 * reads its thread CPU clock and core-cycle counter around every job and
 * the caller sums them when the dispatch completes. This costs two clock
 * reads and two counter reads per thread per dispatch, inside the
 * dispatch; it is applied identically to every scheme. Without
 * accounting, a measurement spanning a dispatch reports the workers'
 * share as unavailable.
 */

#define CMN_POOL_MAX_THREADS   24

typedef void (*cmn_pool_job)(void *ctx, unsigned worker);

/*
 * Resize the pool to exactly threads pool threads (excluding the caller),
 * joining surplus threads and starting missing ones. Returns 0 if threads
 * exceeds CMN_POOL_MAX_THREADS, a dispatch is outstanding, or a thread
 * cannot be started (the pool then keeps its previous threads).
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
 * Run job(ctx, w) on pool threads for w = 1 .. active_threads, resizing
 * the pool to active_threads first (cmn_pool_ensure()). Returns 0 on
 * error (no job is then running).
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
