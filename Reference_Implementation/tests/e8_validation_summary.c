#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../hawk_e8_inner.h"

/*
 * Pull in the ordinary HAWK signing sampler for the serial stability
 * comparison.  The helper is static in hawk_sign.c; this mirrors
 * test_sampler.c and e8_sampler_bench.c, so no sampler logic is copied here.
 */
#include "../hawk_sign.c"

#define MAXN                    1024
#define SAMPLE_SIGMA            1.26
#define SHELL_TAU_LABELS        256u
#define SHELL_SAMPLES_PER_TAU   100000u
#define SHELL_ENUM_LO           (-3)
#define SHELL_ENUM_HI           3
#define SHELL_MAX_NORM2         65535u
#define SHELL_MIN_EXPECTED      0.00025
#define SHELL_MIN_OBSERVED      5u
#define COSET_SAMPLES_PER_TAU   8u
#define FULL_TRIALS             1000u
#define STABILITY_TRIALS        1000u
#define SUMMARY_CSV             "e8_validation_summary.csv"
#define SHELL_CSV               "e8_validation_shell.csv"
#define COSET_CSV               "e8_validation_coset.csv"
#define GLOBAL_CSV              "e8_validation_global.csv"
#define STABILITY_CSV           "e8_validation_stability.csv"

typedef struct {
	double mass[SHELL_MAX_NORM2 + 1u];
	double total;
	uint64_t points;
	uint64_t overflow;
} shell_reference;

typedef struct {
	double mean;
	double median;
	double stddev;
	double cv;
	double p95;
	double p99;
	double max;
} stability_stats;

static uint64_t
rng_next_u64(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

static void
test_rng(void *ctx, void *dst, size_t len)
{
	uint64_t *state = ctx;
	uint8_t *buf = dst;

	while (len > 0) {
		uint64_t x = rng_next_u64(state);
		for (unsigned u = 0; u < 8 && len > 0; u ++, len --) {
			*buf ++ = (uint8_t)(x >> (u << 3));
		}
	}
}

static void
make_random_bits(uint8_t *t0, uint8_t *t1, size_t n, uint64_t *rng_state)
{
	for (size_t u = 0; u < n; u ++) {
		t0[u] = (uint8_t)(rng_next_u64(rng_state) & 1u);
		t1[u] = (uint8_t)(rng_next_u64(rng_state) & 1u);
	}
}

static void
fill_hawk_parities(uint8_t *t, size_t t_len,
	unsigned logn, unsigned trial_index)
{
	for (size_t u = 0; u < t_len; u ++) {
		t[u] = (uint8_t)(0xA7u + 29u * u
			+ 17u * logn + 43u * trial_index);
	}
}

static double
e8_stability_sigma(unsigned logn)
{
	switch (logn) {
	case 8: return 1.260;
	case 9: return 1.278;
	default: return 1.299;
	}
}

static void
make_tau_block(int32_t *taublk, uint8_t tau)
{
	for (unsigned u = 0; u < 8; u ++) {
		taublk[u] = (int32_t)((tau >> u) & 1u);
	}
}

static int
check_block_parity(const int32_t *zblk, uint8_t tau)
{
	for (unsigned u = 0; u < 8; u ++) {
		if ((((uint32_t)zblk[u]) & 1u) != ((tau >> u) & 1u)) {
			return 0;
		}
	}
	return 1;
}

static int
check_block_coset_membership(const int32_t *zblk, uint8_t tau)
{
	int32_t taublk[8], xblk[8], xtau[8], yblk[8], py[8];

	make_tau_block(taublk, tau);
	e8_block_apply_P(xblk, zblk);
	e8_block_apply_P(xtau, taublk);
	for (unsigned u = 0; u < 8; u ++) {
		int32_t d = zblk[u] - taublk[u];

		if ((d & 1) != 0) {
			return 0;
		}
		yblk[u] = d / 2;
	}
	e8_block_apply_P(py, yblk);
	for (unsigned u = 0; u < 8; u ++) {
		if (xblk[u] - xtau[u] != 2 * py[u]) {
			return 0;
		}
	}
	return 1;
}

static int
check_block_norm(const int32_t *zblk, uint64_t sampler_norm2)
{
	int32_t xblk[8];
	int64_t xnorm2 = 0;
	int64_t block_norm2;

	e8_block_apply_P(xblk, zblk);
	for (unsigned u = 0; u < 8; u ++) {
		xnorm2 += (int64_t)xblk[u] * xblk[u];
	}
	block_norm2 = e8_block_norm2(zblk);
	return xnorm2 >= 0
		&& block_norm2 >= 0
		&& sampler_norm2 == (uint64_t)xnorm2
		&& sampler_norm2 == (uint64_t)block_norm2;
}

static uint64_t
block_norm_sum(unsigned logn, const int32_t *z0, const int32_t *z1)
{
	size_t k = (size_t)1 << (logn - 2);
	uint64_t sum = 0;

	for (size_t r = 0; r < k; r ++) {
		int32_t zblk[8];
		int64_t n2;

		e8_read_block(zblk, z0, z1, r, logn);
		n2 = e8_block_norm2(zblk);
		if (n2 < 0 || UINT64_MAX - sum < (uint64_t)n2) {
			return UINT64_MAX;
		}
		sum += (uint64_t)n2;
	}
	return sum;
}

static uint64_t
full_norm_from_apply(unsigned logn, const int32_t *z0, const int32_t *z1)
{
	size_t n = (size_t)1 << logn;
	static int32_t pz0[MAXN], pz1[MAXN];
	uint64_t norm2 = 0;

	e8_apply_P(pz0, pz1, z0, z1, logn);
	for (size_t u = 0; u < n; u ++) {
		uint64_t a = (uint64_t)((int64_t)pz0[u] * pz0[u]);
		uint64_t b = (uint64_t)((int64_t)pz1[u] * pz1[u]);

		if (UINT64_MAX - norm2 < a
			|| UINT64_MAX - norm2 - a < b)
		{
			return UINT64_MAX;
		}
		norm2 += a + b;
	}
	return norm2;
}

static uint64_t
wall_ns(void)
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
wall_delta(uint64_t t0, uint64_t t1)
{
	if (t0 == 0 && t1 == 0) {
		return 0;
	}
	return t1 > t0 ? t1 - t0 : 1;
}

static int
compare_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static uint64_t
percentile_u64(const uint64_t *sorted, unsigned len, unsigned pct)
{
	uint64_t idx;

	if (len == 0) {
		return 0;
	}
	idx = ((uint64_t)pct * (len - 1u) + 99u) / 100u;
	return sorted[idx];
}

static int
compute_stability_stats(stability_stats *stats, uint64_t *samples,
	unsigned len)
{
	double mean = 0.0;
	double variance = 0.0;

	if (stats == NULL || samples == NULL || len == 0) {
		return 0;
	}
	for (unsigned u = 0; u < len; u ++) {
		mean += (double)samples[u];
	}
	mean /= (double)len;
	for (unsigned u = 0; u < len; u ++) {
		double d = (double)samples[u] - mean;

		variance += d * d;
	}
	variance /= (double)len;
	qsort(samples, len, sizeof *samples, compare_u64);

	stats->mean = mean;
	stats->median = (len & 1u) != 0
		? (double)samples[len >> 1]
		: 0.5 * ((double)samples[(len >> 1) - 1u]
			+ (double)samples[len >> 1]);
	stats->stddev = sqrt(variance);
	stats->cv = mean > 0.0 ? stats->stddev / mean : 0.0;
	stats->p95 = (double)percentile_u64(samples, len, 95);
	stats->p99 = (double)percentile_u64(samples, len, 99);
	stats->max = (double)samples[len - 1u];
	return 1;
}

static void
write_shell_header(FILE *out)
{
	fprintf(out, "tau,norm2,observed_count,observed_prob,"
		"expected_prob,abs_error,sigma,samples_per_tau,"
		"enum_m_lo,enum_m_hi,note\n");
}

static void
write_shell_row(FILE *out, uint8_t tau, unsigned norm2,
	uint32_t observed_count,
	double observed_prob, double expected_prob, double abs_error)
{
	fprintf(out, "%u,%u,%u,%.8f,%.8f,%.8f,%.6f,%u,%d,%d,"
		"finite_range_sanity_diagnostic_not_formal_proof\n",
		tau, norm2, observed_count, observed_prob, expected_prob,
		abs_error, SAMPLE_SIGMA, SHELL_SAMPLES_PER_TAU,
		SHELL_ENUM_LO, SHELL_ENUM_HI);
}

static void
write_coset_header(FILE *out)
{
	fprintf(out, "tested_cosets,samples_per_coset,parity_failures,"
		"coset_failures,norm_failures,note\n");
}

static void
write_coset_row(FILE *out, uint64_t parity_failures, uint64_t coset_failures,
	uint64_t norm_failures)
{
	fprintf(out, "256,%u,%llu,%llu,%llu,all_256_tau_labels\n",
		COSET_SAMPLES_PER_TAU,
		(unsigned long long)parity_failures,
		(unsigned long long)coset_failures,
		(unsigned long long)norm_failures);
}

static void
write_global_header(FILE *out)
{
	fprintf(out, "n,blocks,trials,block_sum_failures,"
		"full_norm_failures,note\n");
}

static void
write_global_row(FILE *out, unsigned n, unsigned blocks,
	uint64_t block_sum_failures,
	uint64_t full_norm_failures)
{
	fprintf(out, "%u,%u,%u,%llu,%llu,serial_full_sampler\n",
		n, blocks, FULL_TRIALS,
		(unsigned long long)block_sum_failures,
		(unsigned long long)full_norm_failures);
}

static void
write_stability_header(FILE *out)
{
	fprintf(out, "sampler,n,trials,mean,median,stddev,cv,p95,p99,max\n");
}

static void
write_stability_row_csv(FILE *out, const char *sampler, unsigned n,
	const stability_stats *stats)
{
	fprintf(out, "%s,%u,%u,%.3f,%.3f,%.3f,%.6f,%.3f,%.3f,%.3f\n",
		sampler, n, STABILITY_TRIALS,
		stats->mean, stats->median, stats->stddev, stats->cv,
		stats->p95, stats->p99, stats->max);
}

static void
enumerate_shell_reference_rec(shell_reference *ref,
	int32_t *zblk, uint8_t tau, unsigned coord)
{
	if (coord == 8) {
		int64_t n2 = e8_block_norm2(zblk);
		double weight;

		ref->points ++;
		if (n2 < 0 || (uint64_t)n2 > SHELL_MAX_NORM2) {
			ref->overflow ++;
			return;
		}
		weight = exp(-(double)n2 / (2.0 * SAMPLE_SIGMA * SAMPLE_SIGMA));
		ref->mass[n2] += weight;
		ref->total += weight;
		return;
	}

	int32_t bit = (int32_t)((tau >> coord) & 1u);
	for (int m = SHELL_ENUM_LO; m <= SHELL_ENUM_HI; m ++) {
		zblk[coord] = bit + 2 * m;
		enumerate_shell_reference_rec(ref, zblk, tau, coord + 1);
	}
}

static int
build_shell_reference(shell_reference *ref, uint8_t tau)
{
	int32_t zblk[8];

	memset(ref, 0, sizeof *ref);
	enumerate_shell_reference_rec(ref, zblk, tau, 0);
	return ref->total > 0.0;
}

static int
run_shell_validation(FILE *out, unsigned *rows_out)
{
	static shell_reference ref;
	static uint32_t observed[SHELL_MAX_NORM2 + 1u];

	if (out == NULL || rows_out == NULL) {
		return 0;
	}
	*rows_out = 0;
	if (!e8_sampler_warm_cache(SAMPLE_SIGMA)) {
		fprintf(stderr, "ERR: E8 sampler cache warm-up failed\n");
		return 0;
	}

	write_shell_header(out);
	for (unsigned tau_label = 0;
		tau_label < SHELL_TAU_LABELS; tau_label ++)
	{
		uint8_t tau = (uint8_t)tau_label;
		uint64_t rng_state = UINT64_C(0xE8DA700000000000)
			+ (uint64_t)tau;

		if (!build_shell_reference(&ref, tau)) {
			fprintf(stderr,
				"ERR: shell reference enumeration failed tau=%u\n",
				tau);
			return 0;
		}
		memset(observed, 0, sizeof observed);
		for (unsigned sample = 0;
			sample < SHELL_SAMPLES_PER_TAU; sample ++)
		{
			int32_t zblk[8];
			uint64_t norm2;

			if (!e8_sample_block_construction_a_cm(zblk, tau,
				SAMPLE_SIGMA, test_rng, &rng_state,
				&norm2, NULL))
			{
				fprintf(stderr,
					"ERR: shell sampler failed tau=%u"
					" sample=%u\n", tau, sample);
				return 0;
			}
			if (norm2 <= SHELL_MAX_NORM2) {
				observed[norm2] ++;
			}
		}
		for (unsigned n2 = 0; n2 <= SHELL_MAX_NORM2; n2 ++) {
			double expected_prob = ref.mass[n2] / ref.total;
			double observed_prob =
				(double)observed[n2] / SHELL_SAMPLES_PER_TAU;

			if (expected_prob >= SHELL_MIN_EXPECTED
				|| observed[n2] >= SHELL_MIN_OBSERVED)
			{
				write_shell_row(out, tau, n2, observed[n2],
					observed_prob, expected_prob,
					fabs(observed_prob - expected_prob));
				(*rows_out) ++;
			}
		}
	}
	return 1;
}

static int
run_coset_validation(FILE *out, unsigned *rows_out)
{
	uint64_t rng_state = UINT64_C(0xE8C05E7000000000);
	uint64_t parity_failures = 0;
	uint64_t coset_failures = 0;
	uint64_t norm_failures = 0;

	if (out == NULL || rows_out == NULL) {
		return 0;
	}
	*rows_out = 0;
	if (!e8_sampler_warm_cache(SAMPLE_SIGMA)) {
		fprintf(stderr, "ERR: E8 sampler cache warm-up failed\n");
		return 0;
	}

	for (unsigned tau = 0; tau < 256; tau ++) {
		for (unsigned sample = 0;
			sample < COSET_SAMPLES_PER_TAU; sample ++)
		{
			int32_t zblk[8];
			uint64_t norm2;

			if (!e8_sample_block_construction_a_cm(zblk,
				(uint8_t)tau, SAMPLE_SIGMA,
				test_rng, &rng_state, &norm2, NULL))
			{
				fprintf(stderr,
					"ERR: coset sampler failed tau=%u"
					" sample=%u\n", tau, sample);
				return 0;
			}
			if (!check_block_parity(zblk, (uint8_t)tau)) {
				parity_failures ++;
			}
			if (!check_block_coset_membership(zblk, (uint8_t)tau)) {
				coset_failures ++;
			}
			if (!check_block_norm(zblk, norm2)) {
				norm_failures ++;
			}
		}
	}

	write_coset_header(out);
	write_coset_row(out, parity_failures, coset_failures, norm_failures);
	*rows_out = 1;
	return parity_failures == 0
		&& coset_failures == 0
		&& norm_failures == 0;
}

static int
run_global_factorisation(FILE *out, unsigned *rows_out)
{
	static uint8_t t0[MAXN], t1[MAXN];
	static int32_t z0[MAXN], z1[MAXN];
	int ok = 1;

	if (out == NULL || rows_out == NULL) {
		return 0;
	}
	*rows_out = 0;
	write_global_header(out);
	e8_sampler_set_thread_count(1);
	e8_sampler_set_rng_mode(E8_SAMPLER_RNG_PER_BLOCK);
	for (unsigned logn = 8; logn <= 10; logn ++) {
		size_t n = (size_t)1 << logn;
		size_t k = n >> 2;
		uint64_t bit_rng = UINT64_C(0xE8FAC70000000000)
			+ logn;
		uint64_t sample_rng = UINT64_C(0xE8FAC75100000000)
			+ logn;
		uint64_t block_sum_failures = 0;
		uint64_t full_norm_failures = 0;

		for (unsigned trial = 0; trial < FULL_TRIALS; trial ++) {
			uint64_t returned_norm2;
			uint64_t sum_norm2;
			uint64_t full_norm2;

			make_random_bits(t0, t1, n, &bit_rng);
			if (!e8_sample_z_construction_a_cm(z0, z1,
				&returned_norm2, t0, t1, logn,
				SAMPLE_SIGMA, test_rng, &sample_rng, NULL))
			{
				fprintf(stderr,
					"ERR: full sampler failed n=%u"
					" trial=%u\n", (unsigned)n, trial);
				return 0;
			}
			sum_norm2 = block_norm_sum(logn, z0, z1);
			full_norm2 = full_norm_from_apply(logn, z0, z1);
			if (returned_norm2 != sum_norm2) {
				block_sum_failures ++;
			}
			if (returned_norm2 != full_norm2) {
				full_norm_failures ++;
			}
		}
		write_global_row(out, (unsigned)n, (unsigned)k,
			block_sum_failures, full_norm_failures);
		(*rows_out) ++;
		ok &= block_sum_failures == 0 && full_norm_failures == 0;
	}
	return ok;
}

static int
collect_hawk_stability(unsigned logn, uint64_t *samples, unsigned trials)
{
	size_t n = (size_t)1 << logn;
	uint8_t t[MAXN >> 2];
	int8_t x[2 * MAXN];

	for (unsigned trial = 0; trial < trials; trial ++) {
		uint64_t rng_state = UINT64_C(0xE87A811700000000)
			+ ((uint64_t)logn << 32) + trial;
		uint64_t t0, t1;

		memset(x, 0, sizeof x);
		fill_hawk_parities(t, n >> 2, logn, trial);
		t0 = wall_ns();
		(void)sig_gauss(logn, test_rng, &rng_state, NULL, x, t);
		t1 = wall_ns();
		samples[trial] = wall_delta(t0, t1);
	}
	return 1;
}

static int
collect_e8_serial_stability(unsigned logn, uint64_t *samples, unsigned trials)
{
	size_t n = (size_t)1 << logn;
	static uint8_t t0[MAXN], t1[MAXN];
	static int32_t z0[MAXN], z1[MAXN];
	double sigma = e8_stability_sigma(logn);

	if (!e8_sampler_warm_cache(sigma)) {
		fprintf(stderr, "ERR: E8 sampler cache warm-up failed\n");
		return 0;
	}
	e8_sampler_set_thread_count(1);
	e8_sampler_set_rng_mode(E8_SAMPLER_RNG_PER_BLOCK);
	for (unsigned trial = 0; trial < trials; trial ++) {
		uint64_t bit_rng = UINT64_C(0xE87A8117B1700000)
			+ ((uint64_t)logn << 32) + trial;
		uint64_t sample_rng = UINT64_C(0xE87A81175A000000)
			+ ((uint64_t)logn << 32) + trial;
		uint64_t norm2;
		uint64_t start, end;

		make_random_bits(t0, t1, n, &bit_rng);
		start = wall_ns();
		if (!e8_sample_z_construction_a_cm(z0, z1, &norm2,
			t0, t1, logn, sigma,
			test_rng, &sample_rng, NULL))
		{
			fprintf(stderr,
				"ERR: E8 serial stability sampler failed"
				" n=%u trial=%u\n", (unsigned)n, trial);
			return 0;
		}
		end = wall_ns();
		samples[trial] = wall_delta(start, end);
	}
	return 1;
}

static int
write_stability_row(FILE *out, const char *sampler, unsigned logn,
	uint64_t *samples, unsigned trials)
{
	stability_stats stats;

	if (!compute_stability_stats(&stats, samples, trials)) {
		return 0;
	}
	write_stability_row_csv(out, sampler, 1u << logn, &stats);
	return 1;
}

static int
run_serial_stability_summary(FILE *out, unsigned *rows_out)
{
	uint64_t *samples = malloc(STABILITY_TRIALS * sizeof *samples);
	int ok = 1;

	if (out == NULL || rows_out == NULL) {
		return 0;
	}
	*rows_out = 0;
	if (samples == NULL) {
		fprintf(stderr, "ERR: stability allocation failed\n");
		return 0;
	}
	write_stability_header(out);
	for (unsigned logn = 8; logn <= 10; logn ++) {
		if (!collect_hawk_stability(logn, samples, STABILITY_TRIALS)
			|| !write_stability_row(out, "hawk", logn,
				samples, STABILITY_TRIALS))
		{
			ok = 0;
		} else {
			(*rows_out) ++;
		}
		if (!collect_e8_serial_stability(logn, samples,
			STABILITY_TRIALS)
			|| !write_stability_row(out, "e8_serial", logn,
				samples, STABILITY_TRIALS))
		{
			ok = 0;
		} else {
			(*rows_out) ++;
		}
	}
	free(samples);
	return ok;
}

static int
open_csv(FILE **out, const char *name)
{
	if (out == NULL || name == NULL) {
		return 0;
	}
	*out = fopen(name, "w");
	if (*out == NULL) {
		fprintf(stderr, "ERR: could not open %s for writing\n", name);
		return 0;
	}
	return 1;
}

static void
close_csv(FILE **out)
{
	if (out != NULL && *out != NULL) {
		fclose(*out);
		*out = NULL;
	}
}

int
main(void)
{
	FILE *summary = NULL;
	FILE *shell = NULL;
	FILE *coset = NULL;
	FILE *global = NULL;
	FILE *stability = NULL;
	unsigned shell_rows = 0;
	unsigned coset_rows = 0;
	unsigned global_rows = 0;
	unsigned stability_rows = 0;
	int ok;

	if (!open_csv(&summary, SUMMARY_CSV)
		|| !open_csv(&shell, SHELL_CSV)
		|| !open_csv(&coset, COSET_CSV)
		|| !open_csv(&global, GLOBAL_CSV)
		|| !open_csv(&stability, STABILITY_CSV))
	{
		close_csv(&summary);
		close_csv(&shell);
		close_csv(&coset);
		close_csv(&global);
		close_csv(&stability);
		return 1;
	}

	e8_sampler_set_thread_count(1);
	e8_sampler_set_rng_mode(E8_SAMPLER_RNG_PER_BLOCK);
	ok = run_shell_validation(shell, &shell_rows)
		&& run_coset_validation(coset, &coset_rows)
		&& run_global_factorisation(global, &global_rows)
		&& run_serial_stability_summary(stability, &stability_rows);

	fprintf(summary, "table,file,rows,note\n");
	fprintf(summary, "shell_validation,%s,%u,"
		"all_256_tau_labels_finite_range_sanity_diagnostic_not_formal_proof\n",
		SHELL_CSV, shell_rows);
	fprintf(summary, "coset_validation,%s,%u,all_256_tau_labels\n",
		COSET_CSV, coset_rows);
	fprintf(summary, "global_factorisation,%s,%u,"
		"serial_full_sampler_1000_trials\n",
		GLOBAL_CSV, global_rows);
	fprintf(summary, "serial_stability_wall_ns,%s,%u,"
		"full_sampler_call_wall_ns\n",
		STABILITY_CSV, stability_rows);

	close_csv(&summary);
	close_csv(&shell);
	close_csv(&coset);
	close_csv(&global);
	close_csv(&stability);
	return ok ? 0 : 1;
}
