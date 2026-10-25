#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "cmn_pool.h"

/*
 * The mechanics below (spin/yield policy, memory ordering, generation
 * protocol, thread limit, atexit shutdown and error paths) are those of
 * the former E8 sampler pool (e8_sampler_spin_pool_* in e8_sampler.c),
 * with the job generalised from the sampler's worker arguments to a
 * function and context, plus the optional accounting.
 */

typedef struct {
	unsigned index;
	volatile int stopping;

	/* Written by the worker; read by the caller after a dispatch. */
	uint64_t job_cpu_ns;
	uint64_t job_core_cycles;

	/* Written by the worker when it exits; read after the join. */
	uint64_t exit_cpu_ns;
} cmn_pool_worker;

typedef struct {
	pthread_t threads[CMN_POOL_MAX_THREADS];
	cmn_pool_worker workers[CMN_POOL_MAX_THREADS];
	cmn_pool_job job;
	void *ctx;
	volatile unsigned generation;
	volatile unsigned thread_count;
	volatile unsigned active_threads;
	volatile unsigned remaining;
	volatile int initialized;
	volatile int stopping;
	volatile int accounting;

	/* Caller-side state. */
	int dispatch_accounting;
	cmn_pool_dispatch_acct last;
	cmn_pool_stats stats;
	uint64_t joined_cpu_ns;
	uint64_t cpu_base_ns;
} cmn_pool_state;

static cmn_pool_state cmn_pool;

static int cmn_pool_atexit_registered = 0;

static void
cmn_pool_spin_relax(unsigned *spins)
{
#if defined(__x86_64__)
	_mm_pause();
#endif
	(*spins) ++;
	if ((*spins & 1023u) == 0) {
		sched_yield();
	}
}

static void
cmn_pool_run_job(cmn_pool_state *pool, cmn_pool_worker *worker)
{
	cmn_pool_job job = pool->job;
	void *ctx = pool->ctx;

	if (pool->accounting) {
		uint64_t c0, c1, k0, k1;

		c0 = cmn_thread_cpu_ns();
		k0 = cmn_core_cycles_read();
		job(ctx, worker->index + 1);
		k1 = cmn_core_cycles_read();
		c1 = cmn_thread_cpu_ns();
		worker->job_cpu_ns = cmn_delta(c0, c1);
		worker->job_core_cycles =
			k0 == CMN_UNAVAILABLE || k1 == CMN_UNAVAILABLE
			? CMN_UNAVAILABLE : cmn_delta(k0, k1);
	} else {
		job(ctx, worker->index + 1);
	}
}

static void *
cmn_pool_thread_main(void *arg)
{
	cmn_pool_worker *worker = arg;
	cmn_pool_state *pool = &cmn_pool;
	unsigned seen_generation = 0;

	if (pool->accounting) {
		(void)cmn_core_cycles_open();
	}
	for (;;) {
		unsigned spins = 0;

		while (!pool->stopping
			&& !worker->stopping
			&& pool->generation == seen_generation)
		{
			cmn_pool_spin_relax(&spins);
		}
		if (pool->stopping || worker->stopping) {
			break;
		}
		seen_generation = pool->generation;
		__sync_synchronize();
		if (worker->index < pool->active_threads) {
			cmn_pool_run_job(pool, worker);
			(void)__sync_fetch_and_sub(&pool->remaining, 1);
		}
	}
	worker->exit_cpu_ns = cmn_thread_cpu_ns();
	cmn_core_cycles_close();
	return NULL;
}

/*
 * Join pool thread u and keep its CPU time for the statistics.
 */
static void
cmn_pool_join(cmn_pool_state *pool, unsigned u)
{
	(void)pthread_join(pool->threads[u], NULL);
	pool->joined_cpu_ns += pool->workers[u].exit_cpu_ns;
	pool->stats.threads_joined ++;
}

/* see cmn_pool.h */
void
cmn_pool_shutdown(void)
{
	cmn_pool_state *pool = &cmn_pool;
	unsigned thread_count;

	if (!pool->initialized) {
		return;
	}
	pool->stopping = 1;
	__sync_synchronize();
	pool->generation ++;
	thread_count = pool->thread_count;
	for (unsigned u = 0; u < thread_count; u ++) {
		cmn_pool_join(pool, u);
		pool->threads[u] = (pthread_t)0;
		pool->workers[u].stopping = 0;
	}
	pool->job = NULL;
	pool->ctx = NULL;
	pool->generation = 0;
	pool->thread_count = 0;
	pool->active_threads = 0;
	pool->remaining = 0;
	pool->initialized = 0;
	pool->stopping = 0;
}

static int
cmn_pool_register_atexit(void)
{
	if (cmn_pool_atexit_registered) {
		return 1;
	}
	if (atexit(cmn_pool_shutdown) != 0) {
		return 0;
	}
	cmn_pool_atexit_registered = 1;
	return 1;
}

static void
cmn_pool_reset_state(cmn_pool_state *pool)
{
	pool->job = NULL;
	pool->ctx = NULL;
	pool->generation = 0;
	pool->thread_count = 0;
	pool->active_threads = 0;
	pool->remaining = 0;
	pool->stopping = 0;
	for (unsigned u = 0; u < CMN_POOL_MAX_THREADS; u ++) {
		pool->threads[u] = (pthread_t)0;
		pool->workers[u].index = u;
		pool->workers[u].stopping = 0;
	}
	pool->initialized = 1;
}

/* see cmn_pool.h */
void
cmn_pool_shrink(unsigned threads)
{
	cmn_pool_state *pool = &cmn_pool;
	unsigned old_count;

	if (!pool->initialized || threads >= pool->thread_count) {
		return;
	}
	old_count = pool->thread_count;
	for (unsigned u = threads; u < old_count; u ++) {
		pool->workers[u].stopping = 1;
	}
	__sync_synchronize();
	pool->generation ++;
	for (unsigned u = threads; u < old_count; u ++) {
		cmn_pool_join(pool, u);
	}
	for (unsigned u = threads; u < old_count; u ++) {
		pool->threads[u] = (pthread_t)0;
		pool->workers[u].stopping = 0;
	}
	pool->thread_count = threads;
}

/* see cmn_pool.h */
int
cmn_pool_ensure(unsigned threads)
{
	cmn_pool_state *pool = &cmn_pool;
	unsigned old_count;
	unsigned started;

	if (threads > CMN_POOL_MAX_THREADS) {
		return 0;
	}
	if (!pool->initialized) {
		cmn_pool_reset_state(pool);
		if (!cmn_pool_register_atexit()) {
			pool->initialized = 0;
			return 0;
		}
	}
	if (pool->remaining != 0) {
		return 0;
	}
	cmn_pool_shrink(threads);
	old_count = pool->thread_count;
	started = old_count;
	for (unsigned u = old_count; u < threads; u ++) {
		pool->workers[u].index = u;
		pool->workers[u].stopping = 0;
		if (pthread_create(&pool->threads[u], NULL,
			cmn_pool_thread_main, &pool->workers[u]) != 0)
		{
			for (unsigned v = old_count; v < started; v ++) {
				pool->workers[v].stopping = 1;
			}
			__sync_synchronize();
			pool->generation ++;
			for (unsigned v = old_count; v < started; v ++) {
				cmn_pool_join(pool, v);
				pool->threads[v] = (pthread_t)0;
				pool->workers[v].stopping = 0;
			}
			pool->thread_count = old_count;
			return 0;
		}
		pool->stats.threads_started ++;
		started ++;
		pool->thread_count = started;
	}
	return 1;
}

/* see cmn_pool.h */
unsigned
cmn_pool_thread_count(void)
{
	return cmn_pool.initialized ? cmn_pool.thread_count : 0;
}

/* see cmn_pool.h */
int
cmn_pool_dispatch(cmn_pool_job job, void *ctx, unsigned active_threads)
{
	cmn_pool_state *pool = &cmn_pool;

	if (active_threads == 0 || active_threads > CMN_POOL_MAX_THREADS
		|| job == NULL || !cmn_pool_ensure(active_threads))
	{
		return 0;
	}
	pool->job = job;
	pool->ctx = ctx;
	pool->active_threads = active_threads;
	pool->dispatch_accounting = pool->accounting;
	pool->remaining = active_threads;
	__sync_synchronize();
	pool->generation ++;
	return 1;
}

/*
 * Caller-side accounting of a completed dispatch.
 */
static void
cmn_pool_account(cmn_pool_state *pool, unsigned active_threads)
{
	cmn_pool_dispatch_acct *last = &pool->last;
	cmn_pool_stats *st = &pool->stats;

	last->workers = active_threads;
	st->dispatches ++;
	if (!pool->dispatch_accounting) {
		last->cpu_ns = CMN_UNAVAILABLE;
		last->core_cycles = CMN_UNAVAILABLE;
		st->unaccounted_dispatches ++;
		return;
	}
	last->cpu_ns = 0;
	last->core_cycles = 0;
	for (unsigned u = 0; u < active_threads; u ++) {
		const cmn_pool_worker *w = &pool->workers[u];

		last->cpu_ns += w->job_cpu_ns;
		if (last->core_cycles != CMN_UNAVAILABLE) {
			last->core_cycles = w->job_core_cycles == CMN_UNAVAILABLE
				? CMN_UNAVAILABLE
				: last->core_cycles + w->job_core_cycles;
		}
	}
	st->job_cpu_ns += last->cpu_ns;
	if (st->job_core_cycles != CMN_UNAVAILABLE) {
		st->job_core_cycles = last->core_cycles == CMN_UNAVAILABLE
			? CMN_UNAVAILABLE
			: st->job_core_cycles + last->core_cycles;
	}
}

/* see cmn_pool.h */
int
cmn_pool_wait(void)
{
	cmn_pool_state *pool = &cmn_pool;
	unsigned spins = 0;

	while (pool->remaining != 0 && !pool->stopping) {
		cmn_pool_spin_relax(&spins);
	}
	if (pool->stopping) {
		return 0;
	}
	__sync_synchronize();
	cmn_pool_account(pool, pool->active_threads);
	pool->job = NULL;
	pool->ctx = NULL;
	pool->active_threads = 0;
	return 1;
}

/* see cmn_pool.h */
int
cmn_pool_set_accounting(int enable)
{
	cmn_pool_state *pool = &cmn_pool;

	if (pool->remaining != 0) {
		return 0;
	}
	if (enable) {
		(void)cmn_core_cycles_open();
	}
	pool->accounting = enable != 0;
	__sync_synchronize();
	return 1;
}

/* see cmn_pool.h */
int
cmn_pool_get_accounting(void)
{
	return cmn_pool.accounting;
}

/* see cmn_pool.h */
void
cmn_pool_last_dispatch(cmn_pool_dispatch_acct *acct)
{
	*acct = cmn_pool.last;
}

/* see cmn_pool.h */
void
cmn_pool_stats_get(cmn_pool_stats *stats)
{
	cmn_pool_state *pool = &cmn_pool;
	uint64_t cpu = pool->joined_cpu_ns;

	*stats = pool->stats;
	if (pool->initialized) {
		for (unsigned u = 0; u < pool->thread_count; u ++) {
			clockid_t cid;
			struct timespec ts;

			if (pthread_getcpuclockid(pool->threads[u], &cid) == 0
				&& clock_gettime(cid, &ts) == 0)
			{
				cpu += (uint64_t)ts.tv_sec
					* UINT64_C(1000000000)
					+ (uint64_t)ts.tv_nsec;
			}
		}
	}
	stats->thread_cpu_ns = cpu - pool->cpu_base_ns;
	stats->idle_cpu_ns = stats->thread_cpu_ns > stats->job_cpu_ns
		? stats->thread_cpu_ns - stats->job_cpu_ns : 0;
}

/* see cmn_pool.h */
void
cmn_pool_stats_reset(void)
{
	cmn_pool_state *pool = &cmn_pool;
	cmn_pool_stats st;

	/*
	 * Thread CPU clocks cannot be reset; thread_cpu_ns is reported
	 * relative to the total at the reset.
	 */
	cmn_pool_stats_get(&st);
	pool->cpu_base_ns += st.thread_cpu_ns;
	memset(&pool->stats, 0, sizeof pool->stats);
}

/* see cmn_pool.h */
void
cmn_pool_measure_start(cmn_pool_stamp *start)
{
	const cmn_pool_stats *st = &cmn_pool.stats;

	start->dispatches = st->dispatches;
	start->unaccounted_dispatches = st->unaccounted_dispatches;
	start->job_cpu_ns = st->job_cpu_ns;
	start->job_core_cycles = st->job_core_cycles;
	cmn_measure_start(&start->stamp);
}

/* see cmn_pool.h */
void
cmn_pool_measure_stop(cmn_measure *m, const cmn_pool_stamp *start)
{
	const cmn_pool_stats *st = &cmn_pool.stats;

	cmn_measure_stop(m, &start->stamp);
	if (st->dispatches == start->dispatches) {
		return;
	}
	if (st->unaccounted_dispatches != start->unaccounted_dispatches) {
		m->cpu_ns_all_threads = CMN_UNAVAILABLE;
		m->core_cycles = CMN_UNAVAILABLE;
		return;
	}
	m->cpu_ns_all_threads += st->job_cpu_ns - start->job_cpu_ns;
	if (m->core_cycles != CMN_UNAVAILABLE) {
		m->core_cycles = start->job_core_cycles == CMN_UNAVAILABLE
			|| st->job_core_cycles == CMN_UNAVAILABLE
			? CMN_UNAVAILABLE
			: m->core_cycles
				+ (st->job_core_cycles - start->job_core_cycles);
	}
}
