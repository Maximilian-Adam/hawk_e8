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

#define MAXN   1024
#define DEFAULT_KEYS     2
#define DEFAULT_TRIALS   100

typedef struct {
	unsigned logn;
	double sigma_sign;
	double sigma_verify;
	unsigned max_attempts;
} run_param;

typedef struct {
	uint64_t count;
	double mean;
	double m2;
	int64_t min;
	int64_t max;
} stat_i64;

typedef struct {
	uint64_t count;
	double mean;
	double m2;
} stat_u64;

typedef struct {
	unsigned logn;
	unsigned key_index;
	unsigned trials;
	uint64_t accepted;
	uint64_t total_attempts;
	unsigned max_observed_attempts;
	uint64_t coset_failures;
	uint64_t norm_mismatch_failures;
	uint64_t verify_failures;
	stat_i64 pnorm;
	stat_i64 qnorm;
	stat_i64 norm_margin;
	stat_u64 cycles_sample_total;
	stat_u64 cycles_sign_total;
	stat_u64 wall_ns_sample_total;
	stat_u64 wall_ns_sign_total;
} summary_row;

static const run_param PARAMS[] = {
	{ 8,  1.25, 1.06, 1000 },
	{ 9,  1.28, 1.42, 1000 },
	{ 10, 1.30, 1.57, 1000 }
};

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
cycles_start(void)
{
#if defined(__x86_64__)
	_mm_lfence();
	return __rdtsc();
#else
	return 0;
#endif
}

static uint64_t
cycles_end(void)
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
delta_u64(uint64_t t0, uint64_t t1)
{
	if (t0 == 0 && t1 == 0) {
		return 0;
	}
	if (t1 <= t0) {
		return 1;
	}
	return t1 - t0;
}

static void
stat_i64_add(stat_i64 *st, int64_t x)
{
	double dx = (double)x;

	if (st->count == 0) {
		st->min = x;
		st->max = x;
	} else {
		if (x < st->min) {
			st->min = x;
		}
		if (x > st->max) {
			st->max = x;
		}
	}
	st->count ++;
	double d = dx - st->mean;
	st->mean += d / (double)st->count;
	st->m2 += d * (dx - st->mean);
}

static void
stat_u64_add(stat_u64 *st, uint64_t x)
{
	double dx = (double)x;

	st->count ++;
	double d = dx - st->mean;
	st->mean += d / (double)st->count;
	st->m2 += d * (dx - st->mean);
}

static double
stat_i64_variance(const stat_i64 *st)
{
	return st->count == 0 ? 0.0 : st->m2 / (double)st->count;
}

static int64_t
stat_i64_min(const stat_i64 *st)
{
	return st->count == 0 ? 0 : st->min;
}

static int64_t
stat_i64_max(const stat_i64 *st)
{
	return st->count == 0 ? 0 : st->max;
}

static int
parse_unsigned_env(const char *name, unsigned def_value, unsigned *value)
{
	const char *env = getenv(name);

	*value = def_value;
	if (env == NULL) {
		return 1;
	}
	if (env[0] == 0) {
		fprintf(stderr, "ERR: %s must be a positive integer\n", name);
		return 0;
	}
	char *end = NULL;
	unsigned long x = strtoul(env, &end, 10);
	if (end == env || *end != 0 || x == 0 || x > UINT_MAX) {
		fprintf(stderr, "ERR: %s must be a positive integer, got '%s'\n",
			name, env);
		return 0;
	}
	*value = (unsigned)x;
	return 1;
}

static int
parse_logn_filter(unsigned *logn_filter)
{
	const char *env = getenv("E8_REJECTION_LOGN");

	*logn_filter = 0;
	if (env == NULL) {
		return 1;
	}
	if (env[0] == 0) {
		fprintf(stderr, "ERR: E8_REJECTION_LOGN must be 8, 9, or 10\n");
		return 0;
	}
	char *end = NULL;
	unsigned long x = strtoul(env, &end, 10);
	if (end == env || *end != 0 || (x != 8 && x != 9 && x != 10)) {
		fprintf(stderr, "ERR: E8_REJECTION_LOGN must be 8, 9, or 10,"
			" got '%s'\n", env);
		return 0;
	}
	*logn_filter = (unsigned)x;
	return 1;
}

static uint64_t
rng_next_u64(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

static void
summary_rng(void *ctx, void *dst, size_t len)
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

static unsigned
get_bit(const uint8_t *buf, size_t u)
{
	return (buf[u >> 3] >> (u & 7)) & 1u;
}

static unsigned
i32_bit(const int32_t *a, size_t u)
{
	return ((uint32_t)a[u]) & 1u;
}

static void
bits_to_vec(uint8_t *d, const uint8_t *h, size_t bit_off, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		d[u] = (uint8_t)get_bit(h, bit_off + u);
	}
}

static void
poly_mul_mod2_add(uint8_t *d,
	const int8_t *a, const uint8_t *b, size_t n)
{
	for (size_t v = 0; v < n; v ++) {
		if (b[v] == 0) {
			continue;
		}
		for (size_t u = 0; u < n; u ++) {
			if ((((uint8_t)a[u]) & 1u) == 0) {
				continue;
			}
			size_t w = u + v;
			if (w >= n) {
				w -= n;
			}
			d[w] ^= 1;
		}
	}
}

static void
make_message_context(shake_context *sc_data,
	unsigned logn, unsigned keynum, unsigned trial)
{
	static const char prefix[] = "experimental e8 rejection summary";
	uint8_t buf[3];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)keynum;
	buf[2] = (uint8_t)trial;
	shake_init(sc_data, 256);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

static void
hash_to_h(unsigned logn, uint8_t *h,
	const shake_context *sc_data, const void *hpub,
	const uint8_t *salt, size_t salt_len)
{
	uint8_t hm[64];
	shake_context scd;

	scd = *sc_data;
	shake_inject(&scd, hpub, (size_t)1 << (logn - 4));
	shake_flip(&scd);
	shake_extract(&scd, hm, sizeof hm);

	shake_init(&scd, 256);
	shake_inject(&scd, hm, sizeof hm);
	shake_inject(&scd, salt, salt_len);
	shake_flip(&scd);
	shake_extract(&scd, h, (size_t)1 << (logn - 2));
}

static void
compute_target_t(uint8_t *t0, uint8_t *t1, unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const shake_context *sc_data, const void *hpub,
	const uint8_t *salt, size_t salt_len)
{
	size_t n = (size_t)1 << logn;
	uint8_t h[256], h0[MAXN], h1[MAXN];

	hash_to_h(logn, h, sc_data, hpub, salt, salt_len);
	bits_to_vec(h0, h, 0, n);
	bits_to_vec(h1, h, n, n);

	memset(t0, 0, n);
	memset(t1, 0, n);
	poly_mul_mod2_add(t0, f, h0, n);
	poly_mul_mod2_add(t0, F, h1, n);
	poly_mul_mod2_add(t1, g, h0, n);
	poly_mul_mod2_add(t1, G, h1, n);
}

static void
reconstruct_w(int32_t *w0, int32_t *w1, unsigned logn,
	const void *hpub, const uint8_t *salt, size_t salt_len,
	const shake_context *sc_data, const int16_t *s0, const int16_t *s1)
{
	size_t n = (size_t)1 << logn;
	uint8_t h[256];

	hash_to_h(logn, h, sc_data, hpub, salt, salt_len);
	for (size_t u = 0; u < n; u ++) {
		w0[u] = (int32_t)get_bit(h, u) - 2 * (int32_t)s0[u];
		w1[u] = (int32_t)get_bit(h + (n >> 3), u)
			- 2 * (int32_t)s1[u];
	}
}

static int64_t
pnorm_from_apply(unsigned logn, const int32_t *z0, const int32_t *z1)
{
	size_t n = (size_t)1 << logn;
	int32_t pz0[MAXN], pz1[MAXN];
	int64_t norm = 0;

	e8_apply_P(pz0, pz1, z0, z1, logn);
	for (size_t u = 0; u < n; u ++) {
		norm += (int64_t)pz0[u] * pz0[u]
			+ (int64_t)pz1[u] * pz1[u];
	}
	return norm;
}

static int
coset_matches_target(unsigned logn,
	const uint8_t *t0, const uint8_t *t1,
	const int32_t *z0, const int32_t *z1)
{
	size_t n = (size_t)1 << logn;

	for (size_t u = 0; u < n; u ++) {
		if (i32_bit(z0, u) != t0[u] || i32_bit(z1, u) != t1[u]) {
			return 0;
		}
	}
	return 1;
}

static void
make_hpub(uint8_t *hpub, size_t hpub_len, unsigned logn, unsigned keynum)
{
	for (size_t u = 0; u < hpub_len; u ++) {
		hpub[u] = (uint8_t)(0xC3u + 13u * u
			+ 9u * logn + 5u * keynum);
	}
}

static void
make_salt(uint8_t *salt, size_t salt_len,
	unsigned logn, unsigned keynum, unsigned trial)
{
	for (size_t u = 0; u < salt_len; u ++) {
		salt[u] = (uint8_t)(0xA5u + 29u * u
			+ 11u * logn + 7u * keynum + trial);
	}
}

static int
make_basis(unsigned logn, int8_t *f, int8_t *g, int8_t *F, int8_t *G,
	uint64_t *rng_state)
{
	uint8_t tmp[HAWK_TMPSIZE_KEYGEN(10)];
	uint8_t seed[40];

	return Hawk_keygen(logn, f, g, F, G, NULL, NULL, NULL, seed,
		summary_rng, rng_state, tmp, sizeof tmp) == 0;
}

static int
collect_one_key(FILE *fp, const run_param *param,
	unsigned key_index, unsigned trials, uint64_t *rng_state)
{
	unsigned logn = param->logn;
	size_t n = (size_t)1 << logn;
	size_t hpub_len = (size_t)1 << (logn - 4);
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	int32_t q00[MAXN], q01[MAXN], q10[MAXN], q11[MAXN];
	uint8_t hpub[64];
	uint8_t sig[40 + 4 * MAXN], salt[40];
	uint8_t t0[MAXN], t1[MAXN];
	int16_t s0[MAXN], s1[MAXN];
	int32_t z0[MAXN], z1[MAXN], w0[MAXN], w1[MAXN];
	int64_t verify_bound = 0;
	summary_row row;

	if (!make_basis(logn, f, g, F, G, rng_state)) {
		fprintf(stderr, "ERR: Hawk_keygen failed logn=%u key=%u\n",
			logn, key_index);
		return 0;
	}
	make_hpub(hpub, hpub_len, logn, key_index);
	e8_compute_qform(q00, q01, q10, q11, f, g, F, G, logn);
	if (!e8_verify_bound_from_sigma(logn,
			param->sigma_verify, &verify_bound))
	{
		fprintf(stderr, "ERR: could not compute verify bound logn=%u\n",
			logn);
		return 0;
	}

	memset(&row, 0, sizeof row);
	row.logn = logn;
	row.key_index = key_index;
	row.trials = trials;

	for (unsigned trial = 0; trial < trials; trial ++) {
		shake_context sc_data;
		e8_sign_trace_timing trace;
		uint64_t sign_c0, sign_c1, sign_w0, sign_w1;
		int64_t pnorm = 0, pnorm_check = 0, qnorm = 0;
		unsigned attempts = param->max_attempts;
		int accepted;
		int have_qnorm;

		make_salt(salt, salt_len, logn, key_index, trial);
		make_message_context(&sc_data, logn, key_index, trial);

		sign_c0 = cycles_start();
		sign_w0 = wall_ns();
		accepted = e8_sign_sampler_trace_timed_uncompressed(logn,
			sig, sig_len, &sc_data, hpub, hpub_len,
			f, g, F, G, salt,
			param->sigma_sign, param->sigma_verify,
			param->max_attempts, summary_rng, rng_state,
			z0, z1, &pnorm, &attempts, &trace);
		sign_w1 = wall_ns();
		sign_c1 = cycles_end();

		row.total_attempts += attempts;
		if (attempts > row.max_observed_attempts) {
			row.max_observed_attempts = attempts;
		}

		if (!accepted) {
			continue;
		}
		row.accepted ++;
		stat_u64_add(&row.cycles_sample_total,
			trace.cycles_sample_total);
		stat_u64_add(&row.cycles_sign_total,
			delta_u64(sign_c0, sign_c1));
		stat_u64_add(&row.wall_ns_sample_total,
			trace.wall_ns_sample_total);
		stat_u64_add(&row.wall_ns_sign_total,
			delta_u64(sign_w0, sign_w1));

		make_message_context(&sc_data, logn, key_index, trial);
		compute_target_t(t0, t1, logn, f, g, F, G,
			&sc_data, hpub, salt, salt_len);
		if (!coset_matches_target(logn, t0, t1, z0, z1)) {
			row.coset_failures ++;
		}

		pnorm_check = pnorm_from_apply(logn, z0, z1);
		if (!e8_decode_sig_uncompressed(logn, NULL, s0, s1,
				sig, sig_len))
		{
			row.norm_mismatch_failures ++;
			row.verify_failures ++;
			continue;
		}
		reconstruct_w(w0, w1, logn, hpub, salt, salt_len,
			&sc_data, s0, s1);
		have_qnorm = e8_qnorm_completion(&qnorm,
			q00, q01, w0, w1, logn);
		if (!have_qnorm || pnorm != pnorm_check
			|| pnorm_check != qnorm)
		{
			row.norm_mismatch_failures ++;
		}
		if (!e8_verify_uncompressed_with_sigma(logn,
				sig, sig_len, &sc_data, hpub, hpub_len,
				q00, q01, q10, q11, param->sigma_verify))
		{
			row.verify_failures ++;
		}

		if (have_qnorm) {
			stat_i64_add(&row.pnorm, pnorm_check);
			stat_i64_add(&row.qnorm, qnorm);
			stat_i64_add(&row.norm_margin, verify_bound - qnorm);
		}
	}

	uint64_t total_rejected = row.total_attempts - row.accepted;
	double rejection_rate = row.total_attempts == 0 ? 0.0
		: (double)total_rejected / (double)row.total_attempts;

	fprintf(fp,
		"%u,%u,%u,%u,%llu,%llu,%llu,%.17g,%u,"
		"%.17g,%.17g,%lld,"
		"%lld,%.17g,%.17g,%lld,"
		"%lld,%.17g,%.17g,%lld,"
		"%lld,%.17g,%lld,"
		"%llu,%llu,%llu,"
		"%.17g,%.17g,%.17g,%.17g\n",
		logn, (unsigned)n, key_index, trials,
		(unsigned long long)row.accepted,
		(unsigned long long)row.total_attempts,
		(unsigned long long)total_rejected,
		rejection_rate, row.max_observed_attempts,
		param->sigma_sign, param->sigma_verify, (long long)verify_bound,
		(long long)stat_i64_min(&row.pnorm), row.pnorm.mean,
		stat_i64_variance(&row.pnorm),
		(long long)stat_i64_max(&row.pnorm),
		(long long)stat_i64_min(&row.qnorm), row.qnorm.mean,
		stat_i64_variance(&row.qnorm),
		(long long)stat_i64_max(&row.qnorm),
		(long long)stat_i64_min(&row.norm_margin),
		row.norm_margin.mean,
		(long long)stat_i64_max(&row.norm_margin),
		(unsigned long long)row.coset_failures,
		(unsigned long long)row.norm_mismatch_failures,
		(unsigned long long)row.verify_failures,
		row.cycles_sample_total.mean,
		row.cycles_sign_total.mean,
		row.wall_ns_sample_total.mean,
		row.wall_ns_sign_total.mean);

	return row.accepted == trials
		&& row.total_attempts >= row.accepted
		&& total_rejected == row.total_attempts - row.accepted
		&& row.coset_failures == 0
		&& row.norm_mismatch_failures == 0
		&& row.verify_failures == 0;
}

static void
write_header(FILE *fp)
{
	fprintf(fp,
		"logn,n,key_index,trials,accepted,total_attempts,"
		"total_rejected_attempts,empirical_rejection_rate,"
		"max_attempts,sigma_sign,sigma_verify,verify_bound,"
		"pnorm_min,pnorm_mean,pnorm_variance,"
		"pnorm_max,qnorm_min,qnorm_mean,qnorm_variance,qnorm_max,"
		"norm_margin_min,norm_margin_mean,norm_margin_max,"
		"coset_failures,norm_mismatch_failures,verify_failures,"
		"cycles_sample_total_mean,cycles_sign_total_mean,"
		"wall_ns_sample_total_mean,wall_ns_sign_total_mean\n");
}

int
main(void)
{
	unsigned trials, keys, logn_filter;
	int status = 0;

	if (!parse_unsigned_env("E8_REJECTION_TRIALS",
			DEFAULT_TRIALS, &trials)
		|| !parse_unsigned_env("E8_REJECTION_KEYS",
			DEFAULT_KEYS, &keys)
		|| !parse_logn_filter(&logn_filter))
	{
		return 1;
	}

	write_header(stdout);
	for (size_t pi = 0; pi < sizeof PARAMS / sizeof PARAMS[0]; pi ++) {
		const run_param *param = &PARAMS[pi];
		uint64_t rng_state;

		if (logn_filter != 0 && param->logn != logn_filter) {
			continue;
		}
		rng_state = UINT64_C(0xE8A5500000000000)
			+ (uint64_t)param->logn;
		for (unsigned key = 0; key < keys; key ++) {
			if (!collect_one_key(stdout, param, key,
					trials, &rng_state))
			{
				status = 1;
			}
		}
	}

	return status == 0 ? 0 : 1;
}
