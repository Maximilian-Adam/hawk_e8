#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "../hawk_e8_inner.h"

/*
 * Pull in the ordinary HAWK signing sampler for this standalone benchmark.
 * The sampler helper is static in hawk_sign.c; this mirrors test_sampler.c
 * and keeps the benchmark as a thin wrapper around the real implementation
 * instead of copying sampler tables or logic.
 */
#include "../hawk_sign.c"

#define MAXN                         1024
#define E8_BLOCK_DIM                 8
#define DEFAULT_BENCH_TRIALS         16
#define MAX_BENCH_TRIALS             1000000
#define E8_EXTERNAL_ATTEMPT_LIMIT    16

typedef struct {
	uint64_t state;
} bench_rng_state;

static uint64_t
bench_wall_ns(void)
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

static uint64_t
bench_cycles_start(void)
{
#if defined(__x86_64__)
	_mm_lfence();
	return __rdtsc();
#else
	return 0;
#endif
}

static uint64_t
bench_cycles_end(void)
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

static uint64_t
bench_cycles_delta(uint64_t t0, uint64_t t1)
{
	if (t0 == 0 && t1 == 0) {
		return 0;
	}
	if (t1 <= t0) {
		return 1;
	}
	return t1 - t0;
}

static uint64_t
bench_wall_delta(uint64_t t0, uint64_t t1)
{
	if (t0 == 0 && t1 == 0) {
		return 0;
	}
	if (t1 <= t0) {
		return 1;
	}
	return t1 - t0;
}

static uint64_t
bench_per_unit(uint64_t total, unsigned units)
{
	uint64_t q;

	if (units == 0 || total == 0) {
		return 0;
	}
	q = total / units;
	return q == 0 ? 1 : q;
}

static uint64_t
bench_rng_next(bench_rng_state *rng)
{
	rng->state = rng->state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return rng->state;
}

static void
bench_rng(void *ctx, void *dst, size_t len)
{
	bench_rng_state *rng = ctx;
	uint8_t *buf = dst;

	while (len > 0) {
		uint64_t x = bench_rng_next(rng);

		for (unsigned u = 0; u < 8 && len > 0; u ++, len --) {
			*buf ++ = (uint8_t)(x >> (u << 3));
		}
	}
}

static void
bench_rng_init(bench_rng_state *rng,
	unsigned sampler_id, unsigned logn, unsigned trial_index)
{
	uint64_t seed = UINT64_C(0xE850A11B6E4C0000);

	seed ^= (uint64_t)sampler_id * UINT64_C(0x9E3779B97F4A7C15);
	seed ^= (uint64_t)logn << 48;
	seed ^= (uint64_t)trial_index * UINT64_C(0xD1B54A32D192ED03);
	rng->state = seed;
	(void)bench_rng_next(rng);
}

static void
fill_hawk_parities(uint8_t *t, size_t t_len,
	unsigned logn, unsigned trial_index, unsigned block_index,
	uint8_t tau)
{
	for (size_t u = 0; u < t_len; u ++) {
		t[u] = (uint8_t)(0xA7u + 29u * u
			+ 17u * logn + 43u * trial_index);
	}
	t[block_index] = tau;
}

static uint8_t
make_tau(unsigned logn, unsigned trial_index, unsigned block_index)
{
	return (uint8_t)(0x5Bu + 37u * trial_index
		+ 19u * logn + 11u * block_index);
}

static double
hawk_sigma_sign(unsigned logn)
{
	switch (logn) {
	case 8: return 1.010;
	case 9: return 1.278;
	default: return 1.299;
	}
}

static double
e8_sigma_sign(unsigned logn)
{
	switch (logn) {
	case 8: return 1.25;
	case 9: return 1.28;
	default: return 1.30;
	}
}

static unsigned
get_trials(void)
{
	const char *env = getenv("E8_SAMPLER_BENCH_TRIALS");
	char *end = NULL;
	unsigned long x;

	if (env == NULL || env[0] == 0) {
		return DEFAULT_BENCH_TRIALS;
	}
	x = strtoul(env, &end, 10);
	if (end == env || *end != 0 || x == 0 || x > MAX_BENCH_TRIALS) {
		fprintf(stderr,
			"ERR: E8_SAMPLER_BENCH_TRIALS must be in [1,%u]\n",
			MAX_BENCH_TRIALS);
		return 0;
	}
	return (unsigned)x;
}

static uint64_t
hawk_block_norm(const int8_t *x, unsigned block_index)
{
	size_t off = (size_t)block_index * E8_BLOCK_DIM;
	uint64_t norm = 0;

	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		int32_t y = x[off + u];

		norm += (uint64_t)((int64_t)y * y);
	}
	return norm;
}

static void
write_header(void)
{
	printf("sampler_type,logn,n,block_index,trial_index,sigma,"
		"coset_label,accepted,attempts,cycles_total,"
		"cycles_per_accepted_block,wall_ns_total,"
		"wall_ns_per_accepted_block,norm,notes\n");
}

static void
write_row(const char *sampler_type, unsigned logn, unsigned block_index,
	unsigned trial_index, double sigma, uint8_t tau, int accepted,
	unsigned attempts, uint64_t cycles_total,
	uint64_t cycles_per_accepted_block, uint64_t wall_ns_total,
	uint64_t wall_ns_per_accepted_block, uint64_t norm,
	const char *notes)
{
	size_t n = (size_t)1 << logn;

	printf("%s,%u,%u,%u,%u,%.17g,%u,%d,%u,"
		"%llu,%llu,%llu,%llu,%llu,%s\n",
		sampler_type, logn, (unsigned)n, block_index, trial_index,
		sigma, (unsigned)tau, accepted, attempts,
		(unsigned long long)cycles_total,
		(unsigned long long)cycles_per_accepted_block,
		(unsigned long long)wall_ns_total,
		(unsigned long long)wall_ns_per_accepted_block,
		(unsigned long long)norm, notes);
}

static void
bench_hawk_sampler(unsigned logn, unsigned trial_index)
{
	size_t n = (size_t)1 << logn;
	size_t t_len = n >> 2;
	unsigned block_units = (unsigned)(n >> 2);
	unsigned block_index = trial_index % block_units;
	uint8_t tau = make_tau(logn, trial_index, block_index);
	uint8_t t[MAXN >> 2];
	int8_t x[2 * MAXN];
	bench_rng_state rng;
	uint64_t c0, c1, w0, w1, cycles_total, wall_ns_total;
	uint64_t norm;

	memset(x, 0, sizeof x);
	fill_hawk_parities(t, t_len, logn, trial_index, block_index, tau);
	bench_rng_init(&rng, 0, logn, trial_index);

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	(void)sig_gauss(logn, bench_rng, &rng, NULL, x, t);
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();

	cycles_total = bench_cycles_delta(c0, c1);
	wall_ns_total = bench_wall_delta(w0, w1);
	norm = hawk_block_norm(x, block_index);

	write_row("hawk_sampler", logn, block_index, trial_index,
		hawk_sigma_sign(logn), tau, 1, 1, cycles_total,
		bench_per_unit(cycles_total, block_units), wall_ns_total,
		bench_per_unit(wall_ns_total, block_units), norm,
		"sig_gauss_2n_samples_amortized_to_8_scalar_unit");
}

static void
bench_e8_sampler(unsigned logn, unsigned trial_index)
{
	size_t n = (size_t)1 << logn;
	unsigned block_units = (unsigned)(n >> 2);
	unsigned block_index = trial_index % block_units;
	uint8_t tau = make_tau(logn, trial_index, block_index);
	int32_t zblk[E8_BLOCK_DIM];
	uint64_t norm = 0;
	e8_sampler_stats stats;
	bench_rng_state rng;
	uint64_t c0, c1, w0, w1, cycles_total, wall_ns_total;
	unsigned attempts = 0;
	int accepted = 0;

	memset(&stats, 0, sizeof stats);
	memset(zblk, 0, sizeof zblk);
	bench_rng_init(&rng, 1, logn, trial_index);

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	while (attempts < E8_EXTERNAL_ATTEMPT_LIMIT) {
		attempts ++;
		if (e8_sample_block_construction_a_cm(zblk, tau,
			e8_sigma_sign(logn), bench_rng, &rng, &norm, &stats))
		{
			accepted = 1;
			break;
		}
	}
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();

	cycles_total = bench_cycles_delta(c0, c1);
	wall_ns_total = bench_wall_delta(w0, w1);
	write_row("e8_sampler", logn, block_index, trial_index,
		e8_sigma_sign(logn), tau, accepted, attempts, cycles_total,
		accepted ? cycles_total : 0, wall_ns_total,
		accepted ? wall_ns_total : 0, accepted ? norm : 0,
		"construction_a_cm_block_sampler");
}

int
main(void)
{
	unsigned trials = get_trials();

	if (trials == 0) {
		return 1;
	}
	write_header();
	for (unsigned logn = 8; logn <= 10; logn ++) {
		for (unsigned trial_index = 0;
			trial_index < trials; trial_index ++)
		{
			bench_hawk_sampler(logn, trial_index);
			bench_e8_sampler(logn, trial_index);
		}
	}
	return 0;
}
