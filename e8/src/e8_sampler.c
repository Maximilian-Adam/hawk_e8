#include "e8_sampler_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Shared, integer-only part of the experimental E8 samplers: block
 * geometry, thread and RNG-mode controls, stream derivations, worker
 * dispatch, profile and stream counters. The variant samplers
 * (e8_sampler_hot.c) and the test/tool-only runtime table builder
 * (e8_sampler_runtime.c) build on it. No side-channel hardening is
 * claimed for the hot variants.
 *
 * The external sampler contract is the HAWK-E8-CM contract: tau is the
 * internal P-coordinate label, the returned block is zblk in 2Z^8 + tau,
 * and the measured E8-side vector is xblk = P zblk.
 */

static const uint8_t RM13_CHECK_ROWS[E8_CA_COSETS >> 2] = {
	0xFF, 0xAA, 0xCC, 0xF0
};

static unsigned e8_sampler_thread_override = UINT_MAX;
static unsigned e8_sampler_rng_mode = E8_SAMPLER_RNG_PER_WORKER;
static e8_sampler_profile e8_sampler_last_profile;
static cmn_rng_counters e8_sampler_rng_total;

/*
 * The sampler streams use the shared RNG module's buffer, which has the
 * sampler's original 128-byte size.
 */
typedef char e8_rng_buffer_size_check[CMN_RNG_BUF_BYTES == 128 ? 1 : -1];

/*
 * The caller runs worker 0, so the pool holds at most
 * E8_SAMPLER_MAX_THREADS - 1 threads.
 */
typedef char e8_pool_size_check[
	E8_SAMPLER_MAX_THREADS - 1 <= CMN_POOL_MAX_THREADS ? 1 : -1];

static int32_t
block_shift_get(const int32_t *a, unsigned u, unsigned shift)
{
	assert(shift < 4);
	if (shift == 0) {
		return a[u];
	}
	if (u >= shift) {
		return a[u - shift];
	}
	return -a[4 + u - shift];
}

/* see hawk_e8_inner.h */
void
e8_sampler_profile_reset(void)
{
	memset(&e8_sampler_last_profile, 0, sizeof e8_sampler_last_profile);
}

/* see hawk_e8_inner.h */
int
e8_sampler_profile_get(e8_sampler_profile *profile)
{
	if (profile == NULL) {
		return 0;
	}
	*profile = e8_sampler_last_profile;
	return 1;
}

/* see hawk_e8_inner.h */
void
e8_sampler_rng_counters_get(cmn_rng_counters *total)
{
	if (total != NULL) {
		*total = e8_sampler_rng_total;
	}
}

/* see hawk_e8_inner.h */
void
e8_sampler_rng_counters_reset(void)
{
	memset(&e8_sampler_rng_total, 0, sizeof e8_sampler_rng_total);
}

static void
e8_enc64le(uint8_t *dst, uint64_t x)
{
	for (unsigned u = 0; u < 8; u ++) {
		dst[u] = (uint8_t)(x >> (u << 3));
	}
}

/* see e8_sampler_inner.h */
void
e8_sampler_block_rng_init(cmn_rng *rng, const uint8_t *seed,
	unsigned logn, uint64_t block_index, uint8_t tau,
	uint64_t sigma_sign_bits)
{
	static const char domain[] = "HAWK-E8-CM block sampler v1";
	uint8_t label[18];

	label[0] = (uint8_t)logn;
	label[1] = tau;
	e8_enc64le(label + 2, block_index);
	e8_enc64le(label + 10, sigma_sign_bits);
	cmn_rng_init(rng, domain, sizeof domain - 1,
		seed, E8_MASTER_SEED_BYTES, label, sizeof label);
}

/*
 * Per-worker sampler stream, bound to the message, key and attempt as
 * upstream HAWK's sampler streams are: prefix is the absorbing SHAKE256
 * state of hm || K || enc32le(attempt + 1) (see e8_sampler_prefix_init()),
 * seed is 40 bytes from the signing RNG, and
 *
 *   SHAKE256(prefix || "HAWK-E8-CM worker sampler v2" || seed
 *            || u8 logn || u8 worker_id || u64le(range_start)
 *            || u64le(range_end) || u64le(sigma_sign bits))
 *
 * Without a prefix (sampler-only callers with no message) the stream
 * starts from an empty SHAKE256 state.
 */
static void
e8_worker_rng_init(cmn_rng *rng, const shake_context *prefix,
	const uint8_t seed[E8_BOUND_SEED_BYTES],
	unsigned logn, unsigned worker_id, uint64_t range_start,
	uint64_t range_end, uint64_t sigma_sign_bits)
{
	static const char domain[] = "HAWK-E8-CM worker sampler v2";
	uint8_t label[26];

	label[0] = (uint8_t)logn;
	label[1] = (uint8_t)worker_id;
	e8_enc64le(label + 2, range_start);
	e8_enc64le(label + 10, range_end);
	e8_enc64le(label + 18, sigma_sign_bits);
	if (prefix != NULL) {
		cmn_rng_init_prefixed(rng, prefix, domain, sizeof domain - 1,
			seed, E8_BOUND_SEED_BYTES, label, sizeof label);
	} else {
		cmn_rng_init(rng, domain, sizeof domain - 1,
			seed, E8_BOUND_SEED_BYTES, label, sizeof label);
	}
}

/* see hawk_e8_inner.h */
uint64_t
e8_sampler_prefix_init(shake_context *prefix, const uint8_t *hm,
	const void *key, size_t key_len, uint32_t counter)
{
	uint8_t tbuf[4];
	uint64_t perms;

	enc32le(tbuf, counter);
	shake_init(prefix, 256);
	perms = cmn_rng_absorb(prefix, hm, 64);
	perms += cmn_rng_absorb(prefix, key, key_len);
	perms += cmn_rng_absorb(prefix, tbuf, sizeof tbuf);
	return perms;
}

static unsigned
e8_sampler_auto_thread_count(void)
{
#if defined(_SC_NPROCESSORS_ONLN)
	long count = sysconf(_SC_NPROCESSORS_ONLN);

	if (count > 0) {
		return count > (long)UINT_MAX ? UINT_MAX : (unsigned)count;
	}
#endif
	return 1;
}

static unsigned
e8_sampler_compile_thread_count(void)
{
#if HAWK_E8_SAMPLER_THREADS < 0
	return 1;
#else
	return (unsigned)HAWK_E8_SAMPLER_THREADS;
#endif
}

static unsigned
e8_sampler_parse_thread_count(const char *text, unsigned fallback)
{
	char *end = NULL;
	unsigned long x;

	if (text == NULL || text[0] == 0) {
		return fallback;
	}
	if (strcmp(text, "auto") == 0 || strcmp(text, "AUTO") == 0) {
		return 0;
	}
	if (strcmp(text, "default") == 0 || strcmp(text, "DEFAULT") == 0) {
		return fallback;
	}
	x = strtoul(text, &end, 10);
	if (end == text || *end != 0) {
		return fallback;
	}
	return x > (unsigned long)UINT_MAX ? UINT_MAX : (unsigned)x;
}

static unsigned
e8_sampler_configured_thread_count(void)
{
	if (e8_sampler_thread_override != UINT_MAX) {
		return e8_sampler_thread_override;
	}
	return e8_sampler_parse_thread_count(
		getenv("HAWK_E8_SAMPLER_THREADS"),
		e8_sampler_compile_thread_count());
}

static unsigned
e8_sampler_resolve_thread_count(size_t block_count, unsigned logn)
{
	unsigned threads;

	if (block_count == 0) {
		return 0;
	}
	(void)logn;
	threads = e8_sampler_configured_thread_count();
	if (threads == 0) {
		threads = e8_sampler_auto_thread_count();
		if (threads > E8_SAMPLER_MAX_THREADS) {
			threads = E8_SAMPLER_MAX_THREADS;
		}
	}
	if (threads == 0) {
		threads = 1;
	}
	if (threads > E8_SAMPLER_MAX_THREADS) {
		threads = E8_SAMPLER_MAX_THREADS;
	}
	if ((uint64_t)threads > (uint64_t)block_count) {
		threads = (unsigned)block_count;
	}
	return threads;
}

static void
e8_sampler_apply_auto_policy(size_t block_count,
	unsigned configured_threads, unsigned *thread_count,
	unsigned *rng_mode)
{
	/*
	 * Automatic thread counts use the bound per-worker streams, also
	 * when they resolve to one thread (one stream).
	 */
	if (configured_threads != 0) {
		return;
	}
	if (*thread_count <= 1 || block_count < 2) {
		*thread_count = 1;
	}
	*rng_mode = E8_SAMPLER_RNG_PER_WORKER;
}

/* see hawk_e8_inner.h */
void
e8_sampler_set_thread_count(unsigned threads)
{
	e8_sampler_thread_override = threads;
}

/* see hawk_e8_inner.h */
unsigned
e8_sampler_get_thread_count(unsigned logn)
{
	if (logn < 8 || logn > 10) {
		return 0;
	}
	return e8_sampler_resolve_thread_count(
		(size_t)1 << (logn - 2), logn);
}

/* see hawk_e8_inner.h */
void
e8_sampler_set_rng_mode(unsigned mode)
{
	e8_sampler_rng_mode = mode == E8_SAMPLER_RNG_PER_BLOCK
		? E8_SAMPLER_RNG_PER_BLOCK : E8_SAMPLER_RNG_PER_WORKER;
}

/* see hawk_e8_inner.h */
unsigned
e8_sampler_get_rng_mode(void)
{
	return e8_sampler_rng_mode;
}

/* see e8_sampler_inner.h */
void
e8_sampler_stats_add(e8_sampler_stats *dst, const e8_sampler_stats *src)
{
	if (dst == NULL || src == NULL) {
		return;
	}
	dst->blocks += src->blocks;
	dst->one_dim_samples += src->one_dim_samples;
	for (unsigned u = 0; u < E8_CA_COSETS; u ++) {
		dst->construction_a_cosets[u] +=
			src->construction_a_cosets[u];
	}
	dst->norm2_sum += src->norm2_sum;
	if (src->norm2_max > dst->norm2_max) {
		dst->norm2_max = src->norm2_max;
	}
}

/* see hawk_e8_inner.h */
uint8_t
e8_rm13_syndrome(uint8_t p)
{
	uint8_t s = 0;

	for (unsigned u = 0; u < sizeof RM13_CHECK_ROWS; u ++) {
		s |= (uint8_t)(e8_parity8(p & RM13_CHECK_ROWS[u]) << u);
	}
	return s;
}

/* see hawk_e8_inner.h */
uint8_t
e8_rm13_codeword(unsigned u)
{
	return u < E8_CA_COSETS ? E8_RM13_CODEWORDS[u] : 0;
}

/* see hawk_e8_inner.h */
uint8_t
e8_extract_tau(const uint8_t *t0, const uint8_t *t1,
	size_t r, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t k = (size_t)1 << (logn - 2);
	uint8_t tau = 0;
	for (unsigned u = 0; u < 4; u ++) {
		tau |= (uint8_t)((t0[r + u * k] & 1u) << u);
		tau |= (uint8_t)((t1[r + u * k] & 1u) << (u + 4));
	}
	return tau;
}

/* see hawk_e8_inner.h */
void
e8_write_block(int32_t *z0, int32_t *z1,
	size_t r, const int32_t *zblk, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t k = (size_t)1 << (logn - 2);
	for (unsigned u = 0; u < 4; u ++) {
		z0[r + u * k] = zblk[u];
		z1[r + u * k] = zblk[u + 4];
	}
}

/* see hawk_e8_inner.h */
void
e8_read_block(int32_t *zblk,
	const int32_t *z0, const int32_t *z1, size_t r, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t k = (size_t)1 << (logn - 2);
	for (unsigned u = 0; u < 4; u ++) {
		zblk[u] = z0[r + u * k];
		zblk[u + 4] = z1[r + u * k];
	}
}

/* see hawk_e8_inner.h */
void
e8_block_apply_P(int32_t *xblk, const int32_t *zblk)
{
	const int32_t *z0 = zblk;
	const int32_t *z1 = zblk + 4;

	for (unsigned u = 0; u < 4; u ++) {
		int32_t z0_y0 = z0[u];
		int32_t z0_y1 = block_shift_get(z0, u, 1);
		int32_t z1_y0 = z1[u];
		int32_t z1_y1 = block_shift_get(z1, u, 1);
		int32_t z1_y2 = block_shift_get(z1, u, 2);
		int32_t z1_y3 = block_shift_get(z1, u, 3);

		xblk[u] = 2 * (z0_y0 + z0_y1)
			+ z1_y0 - z1_y1 + z1_y2 + z1_y3;
		xblk[u + 4] = z1_y0 + z1_y1 + z1_y2 + z1_y3;
	}
}

/* see hawk_e8_inner.h */
int64_t
e8_block_norm2(const int32_t *zblk)
{
	int32_t xblk[8];
	int64_t norm2 = 0;

	e8_block_apply_P(xblk, zblk);
	for (unsigned u = 0; u < 8; u ++) {
		norm2 += (int64_t)xblk[u] * xblk[u];
	}
	return norm2;
}

static const int32_t E8_PINV_NUM[E8_BLOCK_DIM][E8_BLOCK_DIM] = {
	{  1,  1, -1,  1,  1, -3,  3, -1 },
	{ -1,  1,  1, -1,  1,  1, -3,  3 },
	{  1, -1,  1,  1, -3,  1,  1, -3 },
	{ -1,  1, -1,  1,  3, -3,  1,  1 },
	{  0,  0,  0,  0,  2,  0,  0,  2 },
	{  0,  0,  0,  0, -2,  2,  0,  0 },
	{  0,  0,  0,  0,  0, -2,  2,  0 },
	{  0,  0,  0,  0,  0,  0, -2,  2 }
};

/* see hawk_e8_inner.h */
int
e8_block_solve_P_checked(int32_t *zblk,
	const int32_t *xblk, uint8_t tau)
{
	int32_t check[E8_BLOCK_DIM];

	if (zblk == NULL || xblk == NULL) {
		return 0;
	}

	for (unsigned i = 0; i < E8_BLOCK_DIM; i ++) {
		int64_t num = 0;

		for (unsigned j = 0; j < E8_BLOCK_DIM; j ++) {
			num += (int64_t)E8_PINV_NUM[i][j] * xblk[j];
		}
		if ((num % 4) != 0) {
			return 0;
		}
		num /= 4;
		if (num < INT32_MIN || num > INT32_MAX) {
			return 0;
		}
		zblk[i] = (int32_t)num;
		if ((((uint32_t)zblk[i]) & 1u) != ((tau >> i) & 1u)) {
			return 0;
		}
	}

	e8_block_apply_P(check, zblk);
	for (unsigned i = 0; i < E8_BLOCK_DIM; i ++) {
		if (check[i] != xblk[i]) {
			return 0;
		}
	}
	return 1;
}

static void
e8_sampler_worker_run(e8_sampler_worker *w)
{
	cmn_rng worker_stream;
	uint64_t c0, w0;

	w->ok = 0;
	w->norm2 = 0;
	memset(&w->stats, 0, sizeof w->stats);
	memset(&w->profile, 0, sizeof w->profile);
	memset(&w->rng, 0, sizeof w->rng);
	if (w->rng_mode == E8_SAMPLER_RNG_PER_WORKER) {
		c0 = e8_profile_cycles_start();
		w0 = e8_profile_wall_ns();
		e8_worker_rng_init(&worker_stream, w->prefix, w->seed,
			w->logn, w->worker_id, (uint64_t)w->start,
			(uint64_t)w->end, w->sigma_sign_bits);
		e8_profile_add(&w->profile.cycles_block_rng_init,
			&w->profile.wall_ns_block_rng_init, c0, w0);
		w->ok = w->range_fn(w, &worker_stream);
		cmn_rng_counters_get(&worker_stream, &w->rng);
	} else {
		w->ok = w->range_fn(w, NULL);
	}
}

/*
 * Pool job (cmn_pool.h): worker k runs the arguments w[k].
 */
static void
e8_sampler_worker_job(void *ctx, unsigned worker)
{
	e8_sampler_worker *w = ctx;

	e8_sampler_worker_run(&w[worker]);
}

/* see e8_sampler_inner.h */
int
e8_sampler_run(e8_sample_range_fn range_fn, const void *tables,
	uint64_t sigma_sign_bits, int32_t *z0, int32_t *z1,
	uint64_t *pnorm_out, const uint8_t *t0, const uint8_t *t1,
	unsigned logn, unsigned rng_mode,
	const shake_context *prefix, uint64_t prefix_permutations,
	hawk_rng rng, void *rng_context, e8_sampler_stats *stats)
{
	if (logn < 8 || logn > 10 || z0 == NULL || z1 == NULL
		|| pnorm_out == NULL || t0 == NULL || t1 == NULL
		|| range_fn == NULL || tables == NULL || rng == NULL)
	{
		return 0;
	}

	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	unsigned configured_threads = e8_sampler_configured_thread_count();
	unsigned thread_count = e8_sampler_resolve_thread_count(k, logn);
	uint8_t seed[E8_BOUND_SEED_BYTES];
	size_t seed_len;
	uint64_t acc = 0;
	e8_sampler_worker workers[E8_SAMPLER_MAX_THREADS];
	e8_sampler_profile profile;
	uint64_t c0, w0;

	if (thread_count == 0 || thread_count > E8_SAMPLER_MAX_THREADS) {
		return 0;
	}
	rng_mode = rng_mode == E8_SAMPLER_RNG_PER_BLOCK
		? E8_SAMPLER_RNG_PER_BLOCK : E8_SAMPLER_RNG_PER_WORKER;
	e8_sampler_apply_auto_policy(k, configured_threads,
		&thread_count, &rng_mode);
	seed_len = rng_mode == E8_SAMPLER_RNG_PER_WORKER
		? E8_BOUND_SEED_BYTES : E8_MASTER_SEED_BYTES;
	memset(&profile, 0, sizeof profile);
	profile.worker_mode = thread_count == 1
		? E8_SAMPLER_WORKER_SERIAL : E8_SAMPLER_WORKER_SPIN;
	profile.rng_mode = rng_mode;
	c0 = e8_profile_cycles_start();
	w0 = e8_profile_wall_ns();
	rng(rng_context, seed, seed_len);
	e8_profile_add(&profile.cycles_master_seed,
		&profile.wall_ns_master_seed, c0, w0);
	memset(z0, 0, n * sizeof *z0);
	memset(z1, 0, n * sizeof *z1);

	for (unsigned u = 0; u < thread_count; u ++) {
		e8_sampler_worker *w = &workers[u];

		memset(w, 0, sizeof *w);
		w->z0 = z0;
		w->z1 = z1;
		w->t0 = t0;
		w->t1 = t1;
		w->seed = seed;
		w->prefix = rng_mode == E8_SAMPLER_RNG_PER_WORKER
			? prefix : NULL;
		w->tables = tables;
		w->range_fn = range_fn;
		w->logn = logn;
		w->worker_id = u;
		w->rng_mode = rng_mode;
		w->start = (k * (size_t)u) / thread_count;
		w->end = (k * (size_t)(u + 1)) / thread_count;
		w->sigma_sign_bits = sigma_sign_bits;
	}

	/*
	 * A serial call runs worker 0 directly and does not touch the
	 * pool. A threaded call dispatches to the pool, which only grows
	 * (cmn_pool.h); harnesses size it outside timed regions.
	 */
	if (thread_count == 1) {
		e8_sampler_worker_run(&workers[0]);
	} else {
		int dispatch_ok;
		int wait_ok;

		c0 = e8_profile_cycles_start();
		w0 = e8_profile_wall_ns();
		dispatch_ok = cmn_pool_dispatch(e8_sampler_worker_job,
			workers, thread_count - 1);
		e8_profile_add(&profile.cycles_worker_dispatch,
			&profile.wall_ns_worker_dispatch, c0, w0);
		if (!dispatch_ok) {
			e8_sampler_last_profile = profile;
			return 0;
		}

		e8_sampler_worker_run(&workers[0]);

		c0 = e8_profile_cycles_start();
		w0 = e8_profile_wall_ns();
		wait_ok = cmn_pool_wait();
		e8_profile_add(&profile.cycles_worker_wait,
			&profile.wall_ns_worker_wait, c0, w0);
		if (!wait_ok) {
			e8_sampler_last_profile = profile;
			return 0;
		}
	}

	c0 = e8_profile_cycles_start();
	w0 = e8_profile_wall_ns();
	for (unsigned u = 0; u < thread_count; u ++) {
		profile.cycles_block_rng_init +=
			workers[u].profile.cycles_block_rng_init;
		profile.wall_ns_block_rng_init +=
			workers[u].profile.wall_ns_block_rng_init;
		profile.cycles_block_sample +=
			workers[u].profile.cycles_block_sample;
		profile.wall_ns_block_sample +=
			workers[u].profile.wall_ns_block_sample;
		cmn_rng_counters_add(&profile.rng, &workers[u].rng);
		if (!workers[u].ok
			|| UINT64_MAX - acc < workers[u].norm2)
		{
			e8_profile_add(&profile.cycles_reduction,
				&profile.wall_ns_reduction, c0, w0);
			cmn_rng_counters_add(&e8_sampler_rng_total, &profile.rng);
			e8_sampler_last_profile = profile;
			return 0;
		}
		acc += workers[u].norm2;
	}
	for (unsigned u = 0; u < thread_count; u ++) {
		e8_sampler_stats_add(stats, &workers[u].stats);
	}
	e8_profile_add(&profile.cycles_reduction,
		&profile.wall_ns_reduction, c0, w0);

	*pnorm_out = acc;
	if (rng_mode == E8_SAMPLER_RNG_PER_WORKER && prefix != NULL) {
		profile.rng.permutations += prefix_permutations;
	}
	cmn_rng_counters_add(&e8_sampler_rng_total, &profile.rng);
	e8_sampler_last_profile = profile;
	return 1;
}

#endif
