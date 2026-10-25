#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hawk_e8_inner.h"
#include "e8_tables_geom.h"
#include "e8_tables_hot64.h"
#include "e8_tables_hot32.h"
#include "e8_tables_ref.h"

/*
 * Tests of the generated E8 sampler tables (e8/tables/) and the
 * integer-only signing parameters:
 *
 *   - the hot64 tables built at runtime in double for the three fixed
 *     sigma_sign values against the generated (exact) tables: hot values
 *     and lengths must agree; the threshold differences are counted and
 *     reported (double weights resolve only about 53 bits, so the low bits
 *     of most 64-bit thresholds differ), with a bound on their size;
 *   - the full sampler on the generated and on the runtime tables gives
 *     the same output for the same streams (the threshold differences are
 *     far too small to be hit);
 *   - the integer norm bounds equal the double computation;
 *   - a coarse distribution check of every generated 1D table (hot64,
 *     hot32, and ref for every centre class): empirical mean and
 *     variance against the values implied exactly by the integer
 *     thresholds.
 */

#define MAXN            1024
#define EQ_SAMPLES      200
#define DIST_SAMPLES    200000
#define MAX_THRESHOLD_DIFF   (UINT64_C(1) << 16)

static const double SIGMA_SIGN[3] = { 3.592, 3.631, 3.669 };

typedef struct {
	uint64_t count;
	uint64_t differ;
	uint64_t max_diff;
} diff_stat;

static void
diff_add(diff_stat *d, uint64_t a, uint64_t b)
{
	uint64_t x = a > b ? a - b : b - a;

	d->count ++;
	if (x != 0) {
		d->differ ++;
		if (x > d->max_diff) {
			d->max_diff = x;
		}
	}
}

static uint64_t
lcg_next(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

static void
lcg_rng(void *ctx, void *dst, size_t len)
{
	uint8_t *buf = dst;

	while (len -- > 0) {
		*buf ++ = (uint8_t)(lcg_next(ctx) >> 56);
	}
}

/*
 * Runtime-built against generated hot64 tables for one logn.
 */
static int
compare_hot64(unsigned logn)
{
	const e8_hot64_tables *g = e8_hot64_tables_get(logn);
	const e8_hot64_tables *r = e8_sampler_runtime_hot64(
		SIGMA_SIGN[logn - 8]);
	diff_stat tiny, tail, comp;
	unsigned tail_len_differ = 0;
	uint64_t trunc_max = 0;

	memset(&tiny, 0, sizeof tiny);
	memset(&tail, 0, sizeof tail);
	memset(&comp, 0, sizeof comp);
	if (g == NULL || r == NULL || g->sigma_sign_bits != r->sigma_sign_bits)
	{
		fprintf(stderr, "ERR: hot64 tables missing logn=%u\n", logn);
		return 0;
	}
	for (unsigned j = 0; j < E8_TAB_CLASSES; j ++) {
		const e8_hot64_row *gr = &g->rows[j], *rr = &r->rows[j];
		unsigned common;

		if (gr->tiny_len != rr->tiny_len
			|| memcmp(gr->tiny_value, rr->tiny_value,
				gr->tiny_len) != 0)
		{
			fprintf(stderr, "ERR: hot64 hot values differ logn=%u"
				" class=%u\n", logn, j);
			return 0;
		}
		for (unsigned i = 0; i < gr->tiny_len; i ++) {
			diff_add(&tiny, gr->tiny_cdf[i], rr->tiny_cdf[i]);
		}
		common = gr->tail_len < rr->tail_len
			? gr->tail_len : rr->tail_len;
		if (memcmp(gr->tail_value, rr->tail_value, common) != 0) {
			fprintf(stderr, "ERR: hot64 tail values differ logn=%u"
				" class=%u\n", logn, j);
			return 0;
		}
		if (gr->tail_len != rr->tail_len) {
			/*
			 * The shorter table ends with the maximum earlier:
			 * the targets above the longer table's threshold at
			 * that index map differently.
			 */
			const e8_hot64_row *lng = gr->tail_len > rr->tail_len
				? gr : rr;
			uint64_t gap = UINT64_MAX - lng->tail_cdf[common - 1];

			tail_len_differ ++;
			if (gap > trunc_max) {
				trunc_max = gap;
			}
			common --;
		}
		for (unsigned i = 0; i < common; i ++) {
			diff_add(&tail, gr->tail_cdf[i], rr->tail_cdf[i]);
		}
	}
	for (unsigned tau = 0; tau < 256; tau ++) {
		for (unsigned c = 0; c < 16; c ++) {
			diff_add(&comp, g->component_cdf[tau][c],
				r->component_cdf[tau][c]);
		}
	}
	printf("E8 tables hot64 n=%u: runtime vs generated thresholds:"
		" hot %llu/%llu differ (max %llu), tail %llu/%llu differ"
		" (max %llu), tail length differs in %u/%u classes"
		" (max moved mass %llu/2^64), component %llu/%llu differ"
		" (max %llu)\n", 1u << logn,
		(unsigned long long)tiny.differ,
		(unsigned long long)tiny.count,
		(unsigned long long)tiny.max_diff,
		(unsigned long long)tail.differ,
		(unsigned long long)tail.count,
		(unsigned long long)tail.max_diff,
		tail_len_differ, (unsigned)E8_TAB_CLASSES,
		(unsigned long long)trunc_max,
		(unsigned long long)comp.differ,
		(unsigned long long)comp.count,
		(unsigned long long)comp.max_diff);
	if (tiny.max_diff > MAX_THRESHOLD_DIFF
		|| tail.max_diff > MAX_THRESHOLD_DIFF
		|| comp.max_diff > MAX_THRESHOLD_DIFF
		|| trunc_max > MAX_THRESHOLD_DIFF)
	{
		fprintf(stderr, "ERR: hot64 runtime thresholds too far from"
			" the generated ones logn=%u\n", logn);
		return 0;
	}
	return 1;
}

/*
 * The full sampler gives the same output on the generated and the
 * runtime-built tables, in the bound per-worker mode, serial.
 */
static int
sampler_equivalence(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	static uint8_t t0[MAXN], t1[MAXN];
	static int32_t za0[MAXN], za1[MAXN], zb0[MAXN], zb1[MAXN];
	uint64_t bits = UINT64_C(0xE8AB1E5000000000) + logn;
	const void *g = e8_hot64_tables_get(logn);
	const void *r = e8_sampler_runtime_hot64(SIGMA_SIGN[logn - 8]);

	e8_sampler_set_thread_count(1);
	for (unsigned u = 0; u < EQ_SAMPLES; u ++) {
		uint64_t sa = UINT64_C(0xE8AB1E5E00000000) + (logn << 16) + u;
		uint64_t sb = sa;
		uint64_t na, nb;

		for (size_t v = 0; v < n; v ++) {
			t0[v] = (uint8_t)(lcg_next(&bits) >> 63);
			t1[v] = (uint8_t)(lcg_next(&bits) >> 63);
		}
		if (!e8_sample_z_hot64(g, za0, za1, &na, t0, t1, logn,
			E8_SAMPLER_RNG_PER_WORKER, NULL, 0, lcg_rng, &sa, NULL)
			|| !e8_sample_z_hot64(r, zb0, zb1, &nb, t0, t1, logn,
			E8_SAMPLER_RNG_PER_WORKER, NULL, 0, lcg_rng, &sb, NULL)
			|| na != nb
			|| memcmp(za0, zb0, n * sizeof *za0) != 0
			|| memcmp(za1, zb1, n * sizeof *za1) != 0)
		{
			fprintf(stderr, "ERR: generated and runtime tables"
				" sample differently logn=%u sample=%u\n",
				logn, u);
			return 0;
		}
	}
	return 1;
}

static int
norm_bounds(unsigned logn)
{
	double sigma_verify;
	int64_t bound;

	if (!e8_sigma_verify_default(logn, &sigma_verify)
		|| !e8_verify_bound_from_sigma(logn, sigma_verify, &bound)
		|| bound != e8_norm_bound_default(logn))
	{
		fprintf(stderr, "ERR: norm bound constant logn=%u\n", logn);
		return 0;
	}
	return 1;
}

/*
 * Exact mean and variance of a hot table from its thresholds (scale
 * 2^bits, target <= threshold): value i of the hot list has probability
 * (q_i - q_{i-1}) / 2^bits with q_{-1} = -1, the tail is reached with the
 * remaining probability and splits the same way.
 */
static void
hot_moments(unsigned bits, unsigned tiny_len, const int8_t *tiny_value,
	const uint64_t *tiny_cdf, unsigned tail_len, const int8_t *tail_value,
	const uint64_t *tail_cdf, long double *mean, long double *var)
{
	long double scale = ldexpl(1.0L, (int)bits);
	long double m1 = 0.0L, m2 = 0.0L, prev = -1.0L, p_tail;

	for (unsigned i = 0; i < tiny_len; i ++) {
		long double p = ((long double)tiny_cdf[i] - prev) / scale;

		m1 += p * tiny_value[i];
		m2 += p * tiny_value[i] * tiny_value[i];
		prev = (long double)tiny_cdf[i];
	}
	p_tail = (scale - 1.0L - prev) / scale;
	prev = -1.0L;
	for (unsigned i = 0; i < tail_len; i ++) {
		long double p = p_tail
			* ((long double)tail_cdf[i] - prev) / scale;

		m1 += p * tail_value[i];
		m2 += p * tail_value[i] * tail_value[i];
		prev = (long double)tail_cdf[i];
	}
	*mean = m1;
	*var = m2 - m1 * m1;
}

/*
 * Empirical mean and variance of samples against the exact values:
 * |mean error| below 6 standard errors, variance within 6 standard
 * errors (fourth-moment bound 3 var^2 for these tails).
 */
static int
moments_ok(const char *what, unsigned logn, unsigned cls,
	long double mean, long double var, const int32_t *x, size_t count)
{
	long double s1 = 0.0L, s2 = 0.0L, emean, evar;

	for (size_t u = 0; u < count; u ++) {
		s1 += x[u];
		s2 += (long double)x[u] * x[u];
	}
	emean = s1 / count;
	evar = s2 / count - emean * emean;
	if (fabsl(emean - mean) > 6.0L * sqrtl(var / count)
		|| fabsl(evar - var) > 6.0L * sqrtl(2.0L * var * var / count))
	{
		fprintf(stderr, "ERR: %s distribution logn=%u class=%u"
			" mean %.6Lf (exact %.6Lf) var %.6Lf (exact %.6Lf)\n",
			what, logn, cls, emean, mean, evar, var);
		return 0;
	}
	return 1;
}

static int
distribution_hot64(unsigned logn)
{
	static int32_t x[DIST_SAMPLES];
	const e8_hot64_tables *g = e8_hot64_tables_get(logn);

	for (unsigned j = 0; j < E8_TAB_CLASSES; j ++) {
		const e8_hot64_row *row = &g->rows[j];
		long double mean, var;
		cmn_rng s;
		uint8_t seed[2] = { (uint8_t)logn, (uint8_t)j };

		hot_moments(64, row->tiny_len, row->tiny_value, row->tiny_cdf,
			row->tail_len, row->tail_value, row->tail_cdf,
			&mean, &var);
		cmn_rng_init(&s, "test_e8_tables hot64", 20, seed, 2, NULL, 0);
		for (size_t u = 0; u < DIST_SAMPLES; u ++) {
			if (!e8_hot64_sample_1d(g, j, &s, &x[u])) {
				return 0;
			}
		}
		if (!moments_ok("hot64", logn, j, mean, var, x,
			DIST_SAMPLES))
		{
			return 0;
		}
	}
	return 1;
}

static int
distribution_hot32(unsigned logn)
{
	static int32_t x[DIST_SAMPLES];
	const e8_hot32_tables *g = e8_hot32_tables_get(logn);

	for (unsigned j = 0; j < E8_TAB_CLASSES; j ++) {
		const e8_hot32_row *row = &g->rows[j];
		uint64_t tiny_cdf[E8_HOT32_TINY_LEN];
		uint64_t tail_cdf[E8_HOT32_TAIL_LEN_MAX];
		long double mean, var;
		cmn_rng s;
		uint8_t seed[2] = { (uint8_t)logn, (uint8_t)j };

		for (unsigned i = 0; i < row->tiny_len; i ++) {
			tiny_cdf[i] = row->tiny_cdf[i];
		}
		for (unsigned i = 0; i < row->tail_len; i ++) {
			tail_cdf[i] = row->tail_cdf[i];
		}
		hot_moments(32, row->tiny_len, row->tiny_value, tiny_cdf,
			row->tail_len, row->tail_value, tail_cdf, &mean, &var);
		cmn_rng_init(&s, "test_e8_tables hot32", 20, seed, 2, NULL, 0);
		for (size_t u = 0; u < DIST_SAMPLES; u ++) {
			if (!e8_hot32_sample_1d(g, j, &s, &x[u])) {
				return 0;
			}
		}
		if (!moments_ok("hot32", logn, j, mean, var, x,
			DIST_SAMPLES))
		{
			return 0;
		}
	}
	return 1;
}

#define REF_DIST_SAMPLES   50000

/*
 * ref: class with centre num32 / 32 = k / 4 uses residue k mod 4, shifted
 * by floor(k / 4); value lo + j has probability (T[j-1] - T[j]) / 2^78
 * with T[-1] = 2^78 and T[j] = 0 past the table.
 */
static int
distribution_ref(unsigned logn)
{
	static int32_t x[REF_DIST_SAMPLES];
	const e8_ref_tables *g = e8_ref_tables_get(logn);

	for (unsigned j = 0; j < E8_TAB_CLASSES; j ++) {
		int64_t c = e8_tab_center_num32[j];
		unsigned kk = (unsigned)((c + 32) >> 3);
		unsigned res = kk & 3;
		int32_t lo = (int32_t)(kk >> 2) - 1 + g->lo_value[res];
		long double scale = ldexpl(1.0L, 78), prev = scale;
		long double m1 = 0.0L, m2 = 0.0L, mean, var;
		uint8_t seed[2] = { (uint8_t)logn, (uint8_t)j };
		cmn_rng s;

		for (unsigned i = 0; i <= E8_REF_1D_LEN; i ++) {
			long double t = i < E8_REF_1D_LEN
				? ldexpl((long double)g->hi[res][i], 63)
					+ (long double)g->lo[res][i]
				: 0.0L;
			long double p = (prev - t) / scale;
			int32_t v = lo + (int32_t)i;

			m1 += p * v;
			m2 += p * v * v;
			prev = t;
		}
		mean = m1;
		var = m2 - m1 * m1;
		cmn_rng_init(&s, "test_e8_tables ref", 18, seed, 2, NULL, 0);
		for (size_t u = 0; u < REF_DIST_SAMPLES; u ++) {
			if (!e8_ref_sample_1d(g, c, &s, &x[u])) {
				return 0;
			}
		}
		if (!moments_ok("ref", logn, j, mean, var, x,
			REF_DIST_SAMPLES))
		{
			return 0;
		}
	}
	return 1;
}

int
main(void)
{
	for (unsigned logn = 8; logn <= 10; logn ++) {
		if (!compare_hot64(logn)) {
			return 1;
		}
	}
	for (unsigned logn = 8; logn <= 10; logn ++) {
		printf("E8 tables n=%u: ", 1u << logn);
		fflush(stdout);
		if (!norm_bounds(logn) || !sampler_equivalence(logn)
			|| !distribution_hot64(logn)
			|| !distribution_hot32(logn)
			|| !distribution_ref(logn))
		{
			return 1;
		}
		printf("norm bound %lld, generated = runtime sampling,"
			" ref, hot64 and hot32 distributions done.\n",
			(long long)e8_norm_bound_default(logn));
	}
	return 0;
}
