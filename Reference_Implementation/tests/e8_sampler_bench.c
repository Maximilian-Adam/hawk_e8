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

/*
 * The sampler benchmark target intentionally links only the ordinary Hawk
 * objects plus e8_math/e8_sampler.  Include the experimental E8 verifier and
 * signer here so this standalone benchmark can also time the end-to-end
 * HAWK-E8-CM signing path without changing the reference Makefile.
 */
#define get_bit e8_vrfy_bench_get_bit
#define hash_to_h e8_vrfy_bench_hash_to_h
#include "../e8_vrfy.c"
#undef get_bit
#undef hash_to_h
#include "../e8_sign.c"

#define MAXN                         1024
#define E8_BLOCK_DIM                 8
#define DEFAULT_BENCH_TRIALS         16
#define MAX_BENCH_TRIALS             1000000

#if HAWK_E8_PROFILE_SIGN
typedef struct {
	uint64_t trials;
	uint64_t accepted;
	uint64_t failed;
	uint64_t attempts;
	uint64_t rejections;
	uint64_t cycles_total;
	uint64_t cycles_hash;
	uint64_t cycles_target;
	uint64_t cycles_sample;
	uint64_t cycles_reconstruct;
	uint64_t cycles_norm_check;
	uint64_t cycles_encode;
	uint64_t wall_ns_total;
	uint64_t wall_ns_hash;
	uint64_t wall_ns_target;
	uint64_t wall_ns_sample;
	uint64_t wall_ns_reconstruct;
	uint64_t wall_ns_norm_check;
	uint64_t wall_ns_encode;
} bench_sign_profile_totals;

static bench_sign_profile_totals sign_profile_totals[11];
#endif

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

static void
fill_e8_parities(uint8_t *t0, uint8_t *t1,
	unsigned logn, unsigned trial_index)
{
	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;

	for (size_t r = 0; r < k; r ++) {
		uint8_t tau = make_tau(logn, trial_index, (unsigned)r);

		for (unsigned u = 0; u < 4; u ++) {
			t0[r + u * k] = (uint8_t)((tau >> u) & 1u);
			t1[r + u * k] = (uint8_t)((tau >> (u + 4)) & 1u);
		}
	}
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

static double
e8_sigma_verify_sign(unsigned logn)
{
	switch (logn) {
	case 8: return 1.06;
	case 9: return 1.42;
	default: return 1.57;
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

static int
get_sign_only(void)
{
	const char *env = getenv("E8_SAMPLER_BENCH_SIGN_ONLY");

	return env != NULL && env[0] != 0 && strcmp(env, "0") != 0;
}

#if HAWK_E8_PROFILE_SIGN
static uint64_t
profile_avg(uint64_t total, uint64_t count)
{
	if (total == 0 || count == 0) {
		return 0;
	}
	return total / count;
}

static double
profile_percent(uint64_t part, uint64_t total)
{
	if (part == 0 || total == 0) {
		return 0.0;
	}
	return 100.0 * (double)part / (double)total;
}

static void
profile_add(unsigned logn, int accepted, unsigned attempts,
	const e8_sign_trace_timing *timing)
{
	bench_sign_profile_totals *total;

	if (logn > 10 || timing == NULL) {
		return;
	}
	total = &sign_profile_totals[logn];
	total->trials ++;
	if (accepted) {
		total->accepted ++;
	} else {
		total->failed ++;
	}
	total->attempts += timing->attempts_total != 0
		? timing->attempts_total : attempts;
	total->rejections += timing->rejections_total;
	total->cycles_total += timing->cycles_sign_total;
	total->cycles_hash += timing->cycles_hash_total;
	total->cycles_target += timing->cycles_target_total;
	total->cycles_sample += timing->cycles_sample_total;
	total->cycles_reconstruct += timing->cycles_reconstruct_total;
	total->cycles_norm_check += timing->cycles_norm_check_total;
	total->cycles_encode += timing->cycles_encode_total;
	total->wall_ns_total += timing->wall_ns_sign_total;
	total->wall_ns_hash += timing->wall_ns_hash_total;
	total->wall_ns_target += timing->wall_ns_target_total;
	total->wall_ns_sample += timing->wall_ns_sample_total;
	total->wall_ns_reconstruct += timing->wall_ns_reconstruct_total;
	total->wall_ns_norm_check += timing->wall_ns_norm_check_total;
	total->wall_ns_encode += timing->wall_ns_encode_total;
}

static void
profile_print_stage(const char *name, uint64_t cycles, uint64_t cycles_total,
	uint64_t wall_ns, uint64_t wall_ns_total)
{
	fprintf(stderr,
		"  %-18s cycles=%12llu %6.2f%%  wall_ns=%12llu %6.2f%%\n",
		name,
		(unsigned long long)cycles,
		profile_percent(cycles, cycles_total),
		(unsigned long long)wall_ns,
		profile_percent(wall_ns, wall_ns_total));
}

static void
profile_print_summary(void)
{
	fprintf(stderr,
		"\nHAWK_E8_PROFILE_SIGN summary "
		"(e8_sign_sampler_trace_timed_uncompressed)\n");
	for (unsigned logn = 8; logn <= 10; logn ++) {
		const bench_sign_profile_totals *total =
			&sign_profile_totals[logn];
		uint64_t stage_cycles, stage_wall_ns;
		uint64_t other_cycles = 0, other_wall_ns = 0;

		if (total->trials == 0) {
			continue;
		}
		stage_cycles = total->cycles_hash
			+ total->cycles_target
			+ total->cycles_sample
			+ total->cycles_reconstruct
			+ total->cycles_norm_check
			+ total->cycles_encode;
		stage_wall_ns = total->wall_ns_hash
			+ total->wall_ns_target
			+ total->wall_ns_sample
			+ total->wall_ns_reconstruct
			+ total->wall_ns_norm_check
			+ total->wall_ns_encode;
		if (total->cycles_total > stage_cycles) {
			other_cycles = total->cycles_total - stage_cycles;
		}
		if (total->wall_ns_total > stage_wall_ns) {
			other_wall_ns = total->wall_ns_total - stage_wall_ns;
		}

		fprintf(stderr,
			"logn=%u n=%u trials=%llu accepted=%llu failed=%llu "
			"attempts=%llu rejections=%llu\n",
			logn, 1u << logn,
			(unsigned long long)total->trials,
			(unsigned long long)total->accepted,
			(unsigned long long)total->failed,
			(unsigned long long)total->attempts,
			(unsigned long long)total->rejections);
		fprintf(stderr,
			"  total cycles=%llu wall_ns=%llu "
			"avg_cycles/sign=%llu avg_wall_ns/sign=%llu\n",
			(unsigned long long)total->cycles_total,
			(unsigned long long)total->wall_ns_total,
			(unsigned long long)profile_avg(total->cycles_total,
				total->trials),
			(unsigned long long)profile_avg(total->wall_ns_total,
				total->trials));
		profile_print_stage("hash_challenge",
			total->cycles_hash, total->cycles_total,
			total->wall_ns_hash, total->wall_ns_total);
		profile_print_stage("target_coset",
			total->cycles_target, total->cycles_total,
			total->wall_ns_target, total->wall_ns_total);
		profile_print_stage("e8_sampler",
			total->cycles_sample, total->cycles_total,
			total->wall_ns_sample, total->wall_ns_total);
		profile_print_stage("reconstruct_s",
			total->cycles_reconstruct, total->cycles_total,
			total->wall_ns_reconstruct, total->wall_ns_total);
		profile_print_stage("norm_rejection",
			total->cycles_norm_check, total->cycles_total,
			total->wall_ns_norm_check, total->wall_ns_total);
		profile_print_stage("encode_sig",
			total->cycles_encode, total->cycles_total,
			total->wall_ns_encode, total->wall_ns_total);
		profile_print_stage("other",
			other_cycles, total->cycles_total,
			other_wall_ns, total->wall_ns_total);
	}
}
#endif

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
	printf("sampler_type,scope,logn,n,block_index,trial_index,sigma,"
		"coset_label,accepted,attempts,cycles_total,"
		"cycles_per_unit,wall_ns_total,"
		"wall_ns_per_unit,norm,notes\n");
}

static void
write_row(const char *sampler_type, const char *scope,
	unsigned logn, unsigned block_index, unsigned trial_index,
	double sigma, uint8_t tau, int accepted, unsigned attempts,
	uint64_t cycles_total,
	uint64_t cycles_per_accepted_block, uint64_t wall_ns_total,
	uint64_t wall_ns_per_accepted_block, uint64_t norm,
	const char *notes)
{
	size_t n = (size_t)1 << logn;

	printf("%s,%s,%u,%u,%u,%u,%.17g,%u,%d,%u,"
		"%llu,%llu,%llu,%llu,%llu,%s\n",
		sampler_type, scope, logn, (unsigned)n, block_index,
		trial_index, sigma, (unsigned)tau, accepted, attempts,
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

	write_row("hawk_sampler", "amortized_block", logn, block_index,
		trial_index,
		hawk_sigma_sign(logn), tau, 1, 1, cycles_total,
		bench_per_unit(cycles_total, block_units), wall_ns_total,
		bench_per_unit(wall_ns_total, block_units), norm,
		"sig_gauss_2n_samples_amortized_to_8_scalar_unit");
}

static void
bench_e8_sampler_full(unsigned logn, unsigned trial_index,
	int warm_cache, const char *sampler_type, const char *notes)
{
	size_t n = (size_t)1 << logn;
	unsigned block_units = (unsigned)(n >> 2);
	unsigned block_index = trial_index % block_units;
	uint8_t tau = make_tau(logn, trial_index, block_index);
	uint8_t t0[MAXN], t1[MAXN];
	int32_t z0[MAXN], z1[MAXN];
	uint64_t norm = 0;
	e8_sampler_stats stats;
	bench_rng_state rng;
	uint64_t c0, c1, w0, w1, cycles_total, wall_ns_total;
	int accepted;

	memset(&stats, 0, sizeof stats);
	memset(z0, 0, sizeof z0);
	memset(z1, 0, sizeof z1);
	fill_e8_parities(t0, t1, logn, trial_index);
	bench_rng_init(&rng, 1, logn, trial_index);
	if (warm_cache && !e8_sampler_warm_cache(e8_sigma_sign(logn))) {
		write_row(sampler_type, "amortized_block", logn, block_index,
			trial_index, e8_sigma_sign(logn), tau, 0, 0,
			0, 0, 0, 0, 0, "cache_warmup_failed");
		return;
	}

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	accepted = e8_sample_z_construction_a_cm(z0, z1, &norm,
		t0, t1, logn, e8_sigma_sign(logn), bench_rng, &rng,
		&stats);
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();

	cycles_total = bench_cycles_delta(c0, c1);
	wall_ns_total = bench_wall_delta(w0, w1);
	write_row(sampler_type, "amortized_block", logn, block_index,
		trial_index, e8_sigma_sign(logn), tau, accepted, 1,
		cycles_total, accepted ? bench_per_unit(cycles_total,
			block_units) : 0, wall_ns_total,
		accepted ? bench_per_unit(wall_ns_total, block_units) : 0,
		accepted ? norm : 0, notes);
}

static void
bench_make_hawk_message_context(shake_context *sc_data,
	unsigned logn, unsigned trial_index)
{
	static const char prefix[] = "ordinary hawk signing bench";
	uint8_t buf[2];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)trial_index;
	hawk_sign_start(sc_data);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

static void
bench_hawk_sign(unsigned logn, unsigned trial_index)
{
	uint8_t priv[HAWK_PRIVKEY_SIZE(10)];
	uint8_t pub[HAWK_PUBKEY_SIZE(10)];
	uint8_t sig[HAWK_SIG_SIZE(10)];
	uint8_t tmp_keygen[HAWK_TMPSIZE_KEYGEN(10)];
	uint8_t tmp_sign[HAWK_TMPSIZE_SIGN(10)];
	shake_context sc_data;
	bench_rng_state key_rng, sign_rng;
	uint64_t c0, c1, w0, w1, cycles_total, wall_ns_total;
	int accepted;

	bench_rng_init(&key_rng, 5, logn, trial_index);
	bench_rng_init(&sign_rng, 6, logn, trial_index);
	bench_make_hawk_message_context(&sc_data, logn, trial_index);
	memset(sig, 0, sizeof sig);

	if (!hawk_keygen(logn, priv, pub, bench_rng, &key_rng,
		tmp_keygen, HAWK_TMPSIZE_KEYGEN(logn)))
	{
		write_row("hawk_sign", "signature", logn, 0, trial_index,
			hawk_sigma_sign(logn), 0, 0, 0,
			0, 0, 0, 0, 0, "keygen_setup_failed");
		return;
	}

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	accepted = hawk_sign_finish(logn, bench_rng, &sign_rng,
		sig, &sc_data, priv, tmp_sign, HAWK_TMPSIZE_SIGN(logn));
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();

	cycles_total = bench_cycles_delta(c0, c1);
	wall_ns_total = bench_wall_delta(w0, w1);
	write_row("hawk_sign", "signature", logn, 0, trial_index,
		hawk_sigma_sign(logn), 0, accepted, 1, cycles_total,
		accepted ? cycles_total : 0, wall_ns_total,
		accepted ? wall_ns_total : 0, 0,
		"ordinary_hawk_sign_finish_encoded_private_key");
}

static void
bench_make_message_context(shake_context *sc_data,
	unsigned logn, unsigned trial_index)
{
	static const char prefix[] = "experimental e8 sampler bench";
	uint8_t buf[2];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)trial_index;
	shake_init(sc_data, 256);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

static void
bench_make_hpub(uint8_t *hpub, size_t hpub_len,
	unsigned logn, unsigned trial_index)
{
	for (size_t u = 0; u < hpub_len; u ++) {
		hpub[u] = (uint8_t)(0x63u + 17u * u
			+ 11u * trial_index + 5u * logn);
	}
}

static void
bench_make_salt(uint8_t *salt, size_t salt_len,
	unsigned logn, unsigned trial_index)
{
	for (size_t u = 0; u < salt_len; u ++) {
		salt[u] = (uint8_t)(0xB5u + 29u * u
			+ trial_index + 3u * logn);
	}
}

static int
bench_make_basis(unsigned logn,
	int8_t *f, int8_t *g, int8_t *F, int8_t *G,
	bench_rng_state *rng)
{
	uint8_t tmp[HAWK_TMPSIZE_KEYGEN(10)];
	uint8_t seed[40];

	return Hawk_keygen(logn, f, g, F, G, NULL, NULL, NULL, seed,
		bench_rng, rng, tmp, sizeof tmp) == 0;
}

static void
bench_e8_sign_sampler(unsigned logn, unsigned trial_index)
{
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	size_t hpub_len = (size_t)1 << (logn - 4);
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	uint8_t hpub[64], salt[40], sig[40 + 4 * MAXN];
	shake_context sc_data;
	bench_rng_state key_rng, sign_rng;
	e8_sign_trace_timing timing;
	uint64_t c0, c1, w0, w1, cycles_total, wall_ns_total;
	int64_t pnorm = 0;
	unsigned attempts = 0;
	int accepted;

	bench_rng_init(&key_rng, 3, logn, trial_index);
	bench_rng_init(&sign_rng, 4, logn, trial_index);
	memset(&timing, 0, sizeof timing);
	memset(sig, 0, sizeof sig);
	bench_make_message_context(&sc_data, logn, trial_index);
	bench_make_hpub(hpub, hpub_len, logn, trial_index);
	bench_make_salt(salt, salt_len, logn, trial_index);

	if (!bench_make_basis(logn, f, g, F, G, &key_rng)
		|| !e8_sampler_warm_cache(e8_sigma_sign(logn)))
	{
		write_row("e8_sign_sampler_cached", "signature", logn, 0,
			trial_index, e8_sigma_sign(logn), 0, 0, 0,
			0, 0, 0, 0, 0, "setup_failed");
		return;
	}

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	accepted = e8_sign_sampler_trace_timed_uncompressed(logn,
		sig, sig_len, &sc_data, hpub, hpub_len, f, g, F, G, salt,
		e8_sigma_sign(logn), e8_sigma_verify_sign(logn), 2, 1000,
		bench_rng, &sign_rng, NULL, NULL, &pnorm, &attempts,
		&timing);
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();

	cycles_total = bench_cycles_delta(c0, c1);
	wall_ns_total = bench_wall_delta(w0, w1);
#if HAWK_E8_PROFILE_SIGN
	profile_add(logn, accepted, attempts, &timing);
#endif
	write_row("e8_sign_sampler_cached", "signature", logn, 0,
		trial_index, e8_sigma_sign(logn), 0, accepted, attempts,
		cycles_total, accepted ? cycles_total : 0, wall_ns_total,
		accepted ? wall_ns_total : 0, accepted ? (uint64_t)pnorm : 0,
		"end_to_end_signing_warm_cache");
}

int
main(void)
{
	unsigned trials = get_trials();
	int sign_only = get_sign_only();

	if (trials == 0) {
		return 1;
	}
	write_header();
	for (unsigned logn = 8; logn <= 10; logn ++) {
		if (!sign_only) {
			bench_e8_sampler_full(logn, 0, 0,
				"e8_sampler_cached_cold_full",
				"construction_a_cm_full_dimension_sampler_includes_lazy_cache_warmup");
		}
		for (unsigned trial_index = 0;
			trial_index < trials; trial_index ++)
		{
			if (!sign_only) {
				bench_hawk_sampler(logn, trial_index);
				bench_e8_sampler_full(logn, trial_index, 1,
					"e8_sampler_cached_warm_block",
					"construction_a_cm_cached_full_dimension_sampler_amortized_to_e8_block");
			}
			bench_hawk_sign(logn, trial_index);
			bench_e8_sign_sampler(logn, trial_index);
		}
	}
#if HAWK_E8_PROFILE_SIGN
	profile_print_summary();
#endif
	return 0;
}
