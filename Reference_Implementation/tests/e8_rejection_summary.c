#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <limits.h>
#include <math.h>
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
#define DEFAULT_SIGMA_VERIFY_SWEEP_TRIALS   16
#define SIGMA_VERIFY_SWEEP_MAX_ATTEMPTS     1000
#define SIGMA_VERIFY_SWEEP_DEFAULT_SEED     UINT64_C(0xE8516A5A5A000000)
#define DEFAULT_SIGMA_SIGN_SWEEP_BLOCK_TRIALS_PER_LABEL   64
#define DEFAULT_SIGMA_SIGN_SWEEP_SIGN_TRIALS              16
#define SIGMA_SIGN_SWEEP_MAX_ATTEMPTS                     1000
#define SIGMA_SIGN_SWEEP_VERIFY_LOOSE                     10.0
#define SIGMA_SIGN_SWEEP_DEFAULT_SEED     UINT64_C(0xE8516A6D1A600000)
#define E8_TAU_LABELS    256
#define E8_COMPONENTS    16

#define ARRAY_LEN(x)   (sizeof (x) / sizeof (x)[0])

typedef struct {
	unsigned logn;
	double sigma_sign;
	double sigma_verify;
	unsigned max_attempts;
} run_param;

typedef struct {
	unsigned logn;
	double sigma_sign;
	const double *sigma_verify;
	size_t sigma_verify_len;
} sigma_verify_sweep_grid;

typedef struct {
	unsigned logn;
	const double *sigma_sign;
	size_t sigma_sign_len;
} sigma_sign_sweep_grid;

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
	stat_u64 cycles_sign_call;
	stat_u64 wall_ns_sign_call;
} summary_row;

static const run_param PARAMS[] = {
	{ 8,  1.26,  0.73, 1000 },
	{ 9,  1.278, 0.72, 1000 },
	{ 10, 1.299, 0.71, 1000 }
};

static const double SIGMA_VERIFY_SWEEP_8[] = {
	0.73
};
static const double SIGMA_VERIFY_SWEEP_9[] = {
	0.72
};
static const double SIGMA_VERIFY_SWEEP_10[] = {
	0.71
};

static const double SIGMA_VERIFY_SWEEP_FALLBACK_8[] = { 0.73 };
static const double SIGMA_VERIFY_SWEEP_FALLBACK_9[] = { 0.72 };
static const double SIGMA_VERIFY_SWEEP_FALLBACK_10[] = { 0.71 };

static const sigma_verify_sweep_grid SIGMA_VERIFY_SWEEP_DEFAULT_GRID[] = {
	{ 8, 1.26, SIGMA_VERIFY_SWEEP_8,
		ARRAY_LEN(SIGMA_VERIFY_SWEEP_8) },
	{ 9, 1.278, SIGMA_VERIFY_SWEEP_9,
		ARRAY_LEN(SIGMA_VERIFY_SWEEP_9) },
	{ 10, 1.299, SIGMA_VERIFY_SWEEP_10,
		ARRAY_LEN(SIGMA_VERIFY_SWEEP_10) }
};

static const sigma_verify_sweep_grid SIGMA_VERIFY_SWEEP_FALLBACK_GRID[] = {
	{ 8, 1.26, SIGMA_VERIFY_SWEEP_FALLBACK_8,
		ARRAY_LEN(SIGMA_VERIFY_SWEEP_FALLBACK_8) },
	{ 9, 1.278, SIGMA_VERIFY_SWEEP_FALLBACK_9,
		ARRAY_LEN(SIGMA_VERIFY_SWEEP_FALLBACK_9) },
	{ 10, 1.299, SIGMA_VERIFY_SWEEP_FALLBACK_10,
		ARRAY_LEN(SIGMA_VERIFY_SWEEP_FALLBACK_10) }
};

static const double SIGMA_SIGN_SWEEP_SIGN_8[] = {
	1.26
};
static const double SIGMA_SIGN_SWEEP_SIGN_9[] = {
	1.278
};
static const double SIGMA_SIGN_SWEEP_SIGN_10[] = {
	1.299
};

static const sigma_sign_sweep_grid SIGMA_SIGN_SWEEP_GRID[] = {
	{ 8, SIGMA_SIGN_SWEEP_SIGN_8, ARRAY_LEN(SIGMA_SIGN_SWEEP_SIGN_8) },
	{ 9, SIGMA_SIGN_SWEEP_SIGN_9, ARRAY_LEN(SIGMA_SIGN_SWEEP_SIGN_9) },
	{ 10, SIGMA_SIGN_SWEEP_SIGN_10, ARRAY_LEN(SIGMA_SIGN_SWEEP_SIGN_10) }
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

static double
stat_u64_variance(const stat_u64 *st)
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
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static int
cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a;
	double y = *(const double *)b;

	return x < y ? -1 : x > y ? 1 : 0;
}

static uint64_t
quantile_u64_sorted(const uint64_t *x, size_t len, double q)
{
	if (x == NULL || len == 0) {
		return 0;
	}
	size_t idx = (size_t)ceil(q * (double)len);
	if (idx == 0) {
		idx = 1;
	}
	if (idx > len) {
		idx = len;
	}
	return x[idx - 1];
}

static double
median_double_sorted(const double *x, size_t len)
{
	if (x == NULL || len == 0) {
		return 0.0;
	}
	if ((len & 1) != 0) {
		return x[len >> 1];
	}
	return 0.5 * (x[(len >> 1) - 1] + x[len >> 1]);
}

static int
parse_logn_filter_env(const char *name, int allow_all,
	unsigned *logn_filter)
{
	const char *env = getenv(name);
	const char *choices = allow_all ? "8, 9, 10, or all" : "8, 9, or 10";

	*logn_filter = 0;
	if (env == NULL || (allow_all && strcmp(env, "all") == 0)) {
		return 1;
	}
	if (env[0] == 0) {
		fprintf(stderr, "ERR: %s must be %s\n", name, choices);
		return 0;
	}
	char *end = NULL;
	unsigned long x = strtoul(env, &end, 10);
	if (end == env || *end != 0 || (x != 8 && x != 9 && x != 10)) {
		fprintf(stderr, "ERR: %s must be %s, got '%s'\n",
			name, choices, env);
		return 0;
	}
	*logn_filter = (unsigned)x;
	return 1;
}

static int
parse_sigma_verify_sweep_grid(const sigma_verify_sweep_grid **grid, size_t *grid_len)
{
	const char *env = getenv("E8_SIGMA_VERIFY_SWEEP_GRID");

	if (env == NULL || strcmp(env, "default") == 0) {
		*grid = SIGMA_VERIFY_SWEEP_DEFAULT_GRID;
		*grid_len = ARRAY_LEN(SIGMA_VERIFY_SWEEP_DEFAULT_GRID);
		return 1;
	}
	if (strcmp(env, "fallback") == 0) {
		*grid = SIGMA_VERIFY_SWEEP_FALLBACK_GRID;
		*grid_len = ARRAY_LEN(SIGMA_VERIFY_SWEEP_FALLBACK_GRID);
		return 1;
	}
	fprintf(stderr,
		"ERR: E8_SIGMA_VERIFY_SWEEP_GRID must be default or fallback,"
		" got '%s'\n", env);
	return 0;
}

static int
parse_seed_env(const char *name, uint64_t def_value, uint64_t *value)
{
	const char *env = getenv(name);

	*value = def_value;
	if (env == NULL) {
		return 1;
	}
	if (env[0] == 0) {
		fprintf(stderr, "ERR: %s must be an unsigned integer\n", name);
		return 0;
	}
	errno = 0;
	char *end = NULL;
	unsigned long long x = strtoull(env, &end, 0);
	if (end == env || *end != 0 || errno == ERANGE) {
		fprintf(stderr, "ERR: %s must be an unsigned integer, got '%s'\n",
			name, env);
		return 0;
	}
	*value = (uint64_t)x;
	return 1;
}

static int
env_flag_requested(const char *name)
{
	const char *env = getenv(name);

	return env != NULL && env[0] != 0 && strcmp(env, "0") != 0;
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

static unsigned
eta_for_logn(unsigned logn)
{
	return 1u << (logn - 7);
}

static int
selected_e8_sampler_config(unsigned logn,
	unsigned *threads, unsigned *rng_mode)
{
	if (threads == NULL || rng_mode == NULL) {
		return 0;
	}
	switch (logn) {
	case 8:
		*threads = 12;
		*rng_mode = E8_SAMPLER_RNG_PER_WORKER;
		return 1;
	case 9:
		*threads = 16;
		*rng_mode = E8_SAMPLER_RNG_PER_WORKER;
		return 1;
	case 10:
		*threads = 16;
		*rng_mode = E8_SAMPLER_RNG_PER_WORKER;
		return 1;
	default:
		return 0;
	}
}

static double
entropy_bits_from_probs(const double *p, size_t len)
{
	double h = 0.0;

	for (size_t u = 0; u < len; u ++) {
		if (p[u] > 0.0) {
			h -= p[u] * (log(p[u]) / log(2.0));
		}
	}
	return h;
}

static double
entropy_bits_from_counts(const uint64_t *counts, size_t len, uint64_t total)
{
	double h = 0.0;

	if (total == 0) {
		return 0.0;
	}
	for (size_t u = 0; u < len; u ++) {
		if (counts[u] != 0) {
			double p = (double)counts[u] / (double)total;
			h -= p * (log(p) / log(2.0));
		}
	}
	return h;
}

static void
csv_double(FILE *fp, double x, int available)
{
	if (available) {
		fprintf(fp, "%.17g", x);
	} else {
		fputs("NA", fp);
	}
}

static void
csv_u64(FILE *fp, uint64_t x, int available)
{
	if (available) {
		fprintf(fp, "%llu", (unsigned long long)x);
	} else {
		fputs("NA", fp);
	}
}

static int
collect_one_key_stats(const run_param *param, unsigned key_index,
	unsigned trials, uint64_t *rng_state, summary_row *row,
	int64_t *verify_bound_out)
{
	unsigned logn = param->logn;
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

	memset(row, 0, sizeof *row);
	row->trials = trials;
	if (verify_bound_out != NULL) {
		*verify_bound_out = verify_bound;
	}

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

		row->total_attempts += attempts;
		if (attempts > row->max_observed_attempts) {
			row->max_observed_attempts = attempts;
		}
		stat_u64_add(&row->cycles_sign_call,
			delta_u64(sign_c0, sign_c1));
		stat_u64_add(&row->wall_ns_sign_call,
			delta_u64(sign_w0, sign_w1));

		if (!accepted) {
			continue;
		}
		row->accepted ++;
		stat_u64_add(&row->cycles_sample_total,
			trace.cycles_sample_total);
		stat_u64_add(&row->cycles_sign_total,
			delta_u64(sign_c0, sign_c1));
		stat_u64_add(&row->wall_ns_sample_total,
			trace.wall_ns_sample_total);
		stat_u64_add(&row->wall_ns_sign_total,
			delta_u64(sign_w0, sign_w1));

		make_message_context(&sc_data, logn, key_index, trial);
		compute_target_t(t0, t1, logn, f, g, F, G,
			&sc_data, hpub, salt, salt_len);
		if (!coset_matches_target(logn, t0, t1, z0, z1)) {
			row->coset_failures ++;
		}

		pnorm_check = pnorm_from_apply(logn, z0, z1);
		if (!e8_decode_sig_uncompressed(logn, NULL, s0, s1,
				sig, sig_len))
		{
			row->norm_mismatch_failures ++;
			row->verify_failures ++;
			continue;
		}
		reconstruct_w(w0, w1, logn, hpub, salt, salt_len,
			&sc_data, s0, s1);
		have_qnorm = e8_qnorm_completion(&qnorm,
			q00, q01, w0, w1, logn);
		if (!have_qnorm || pnorm != pnorm_check
			|| pnorm_check != qnorm)
		{
			row->norm_mismatch_failures ++;
		}
		if (!e8_verify_uncompressed_with_sigma(logn,
				sig, sig_len, &sc_data, hpub, hpub_len,
				q00, q01, q10, q11, param->sigma_verify))
		{
			row->verify_failures ++;
		}

		if (have_qnorm) {
			stat_i64_add(&row->pnorm, pnorm_check);
			stat_i64_add(&row->qnorm, qnorm);
			stat_i64_add(&row->norm_margin, verify_bound - qnorm);
		}
	}

	return 1;
}

static int
collect_one_key(FILE *fp, const run_param *param,
	unsigned key_index, unsigned trials, uint64_t *rng_state)
{
	unsigned logn = param->logn;
	size_t n = (size_t)1 << logn;
	summary_row row;
	int64_t verify_bound = 0;

	if (!collect_one_key_stats(param, key_index, trials, rng_state,
			&row, &verify_bound))
	{
		return 0;
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

static void
write_sigma_verify_sweep_header(FILE *fp)
{
	fprintf(fp,
		"logn,n,eta,sigma_sign,sigma_verify,trials,"
		"success_count,failure_count,verification_reject_count,"
		"pnorm_mean,e8_verify_norm_mean,pnorm_min,pnorm_max,"
		"threshold,mean_attempts,max_attempts,"
		"mean_signing_cycles,mean_signing_ns,status\n");
}

static const char *
sigma_verify_sweep_status(const summary_row *row)
{
	unsigned classes = 0;
	const char *status = "ok";

	if (row->accepted < row->trials) {
		status = "signing_failed";
		classes ++;
	}
	if (row->verify_failures != 0) {
		status = "verification_reject";
		classes ++;
	}
	if (row->coset_failures != 0 || row->norm_mismatch_failures != 0) {
		status = "internal_check_failure";
		classes ++;
	}
	return classes > 1 ? "mixed_failures" : status;
}

static void
write_sigma_verify_sweep_row(FILE *fp, const run_param *param,
	const summary_row *row, int64_t verify_bound, const char *status)
{
	size_t n = (size_t)1 << param->logn;
	uint64_t verification_reject_count = row->verify_failures;
	uint64_t success_count = row->accepted >= verification_reject_count
		? row->accepted - verification_reject_count : 0;
	uint64_t accounted = success_count + verification_reject_count;
	uint64_t failure_count = row->trials >= accounted
		? row->trials - accounted : 0;
	double mean_attempts = row->trials == 0 ? 0.0
		: (double)row->total_attempts / (double)row->trials;

	fprintf(fp,
		"%u,%u,%u,%.3f,%.3f,%u,"
		"%llu,%llu,%llu,"
		"%.17g,%.17g,%lld,%lld,"
		"%lld,%.17g,%u,"
		"%.17g,%.17g,%s\n",
		param->logn, (unsigned)n, eta_for_logn(param->logn),
		param->sigma_sign, param->sigma_verify, row->trials,
		(unsigned long long)success_count,
		(unsigned long long)failure_count,
		(unsigned long long)verification_reject_count,
		row->pnorm.mean, row->qnorm.mean,
		(long long)stat_i64_min(&row->pnorm),
		(long long)stat_i64_max(&row->pnorm),
		(long long)verify_bound,
		mean_attempts, row->max_observed_attempts,
		row->cycles_sign_call.mean,
		row->wall_ns_sign_call.mean,
		status);
}

static uint64_t
sigma_verify_sweep_seed(uint64_t base_seed,
	unsigned logn, size_t verify_index)
{
	uint64_t x = base_seed;

	x ^= (uint64_t)logn * UINT64_C(0x9E3779B97F4A7C15);
	x ^= ((uint64_t)verify_index + 1) * UINT64_C(0x94D049BB133111EB);
	return x == 0 ? SIGMA_VERIFY_SWEEP_DEFAULT_SEED : x;
}

static int
run_sigma_verify_sweep(void)
{
	unsigned trials, logn_filter;
	uint64_t base_seed;
	const sigma_verify_sweep_grid *grid;
	size_t grid_len;
	int setup_status = 0;

	if (!parse_unsigned_env("E8_SIGMA_VERIFY_SWEEP_TRIALS",
			DEFAULT_SIGMA_VERIFY_SWEEP_TRIALS, &trials)
		|| !parse_sigma_verify_sweep_grid(&grid, &grid_len)
		|| !parse_logn_filter_env("E8_SIGMA_VERIFY_SWEEP_LOGN",
			1, &logn_filter)
		|| !parse_seed_env("E8_SIGMA_VERIFY_SWEEP_SEED",
			SIGMA_VERIFY_SWEEP_DEFAULT_SEED, &base_seed))
	{
		return 1;
	}

	write_sigma_verify_sweep_header(stdout);
	for (size_t gi = 0; gi < grid_len; gi ++) {
		if (logn_filter != 0 && grid[gi].logn != logn_filter) {
			continue;
		}
		for (size_t vi = 0; vi < grid[gi].sigma_verify_len; vi ++) {
			run_param param;
			summary_row row;
			int64_t verify_bound = 0;
			uint64_t rng_state;

			param.logn = grid[gi].logn;
			param.sigma_sign = grid[gi].sigma_sign;
			param.sigma_verify = grid[gi].sigma_verify[vi];
			param.max_attempts = SIGMA_VERIFY_SWEEP_MAX_ATTEMPTS;
			memset(&row, 0, sizeof row);
			row.trials = trials;
			(void)e8_verify_bound_from_sigma(param.logn,
				param.sigma_verify, &verify_bound);
			rng_state = sigma_verify_sweep_seed(base_seed, param.logn, vi);
			if (!collect_one_key_stats(&param, 0, trials,
					&rng_state, &row, &verify_bound))
			{
				write_sigma_verify_sweep_row(stdout, &param,
					&row, verify_bound, "setup_failed");
				setup_status = 1;
				continue;
			}
			write_sigma_verify_sweep_row(stdout, &param, &row,
				verify_bound, sigma_verify_sweep_status(&row));
		}
	}

	return setup_status;
}

typedef struct {
	unsigned logn;
	double sigma_sign;
	unsigned block_trials_per_label;
	unsigned sign_trials;
	uint64_t block_samples;
	stat_u64 block_norm;
	uint64_t block_norm_p50;
	uint64_t block_norm_p90;
	uint64_t block_norm_p99;
	uint64_t block_norm_max;
	double block_min_shell_mass;
	double block_first2_shell_mass;
	double block_first3_shell_mass;
	double per_label_mean_min;
	double per_label_mean_median;
	double per_label_mean_max;
	double per_label_mean_std;
	double per_label_variance_median;
	double per_label_variance_max;
	double per_label_worst_min_shell_mass;
	double per_label_worst_first3_shell_mass;
	int pred_component_available;
	double pred_component_entropy_bits;
	double emp_component_entropy_bits;
	double component_relerr_max;
	stat_u64 sign_norm;
	uint64_t sign_norm_p50;
	uint64_t sign_norm_p90;
	uint64_t sign_norm_p99;
	uint64_t sign_norm_max;
	stat_u64 sig_coeff_abs;
	uint64_t sig_coeff_abs_p99;
	uint64_t sig_coeff_abs_max;
	stat_u64 profile_cycles_block_sample;
	stat_u64 profile_wall_ns_block_sample;
	char status[64];
} sigma_sign_sweep_result;

static uint64_t
sigma_sign_sweep_seed(uint64_t base_seed,
	unsigned logn, size_t sigma_index, uint64_t stream)
{
	uint64_t x = base_seed;

	x ^= (uint64_t)logn * UINT64_C(0xD6E8FEB86659FD93);
	x += ((uint64_t)sigma_index + 1) * UINT64_C(0x9E3779B97F4A7C15);
	x ^= (stream + 1) * UINT64_C(0xBF58476D1CE4E5B9);
	return x == 0 ? SIGMA_SIGN_SWEEP_DEFAULT_SEED : x;
}

static int
compute_pred_component_freq(double *freq, double *entropy_bits,
	double sigma_sign)
{
	double sum[E8_COMPONENTS];

	memset(sum, 0, sizeof sum);
	if (!e8_sampler_warm_cache(sigma_sign)) {
		return 0;
	}
	for (unsigned tau = 0; tau < E8_TAU_LABELS; tau ++) {
		double mass[E8_COMPONENTS];
		double total = 0.0;

		for (unsigned component = 0; component < E8_COMPONENTS;
			component ++)
		{
			double reference, cached;

			if (!e8_sampler_cache_compare_component_mass(
					(uint8_t)tau, (uint8_t)component,
					sigma_sign, &reference, &cached))
			{
				return 0;
			}
			(void)reference;
			mass[component] = cached;
			total += cached;
		}
		if (!(total > 0.0)) {
			return 0;
		}
		for (unsigned component = 0; component < E8_COMPONENTS;
			component ++)
		{
			sum[component] += mass[component] / total;
		}
	}
	for (unsigned component = 0; component < E8_COMPONENTS; component ++) {
		freq[component] = sum[component] / (double)E8_TAU_LABELS;
	}
	*entropy_bits = entropy_bits_from_probs(freq, E8_COMPONENTS);
	return 1;
}

static void
compute_low_shell_masses(const uint64_t *norms, size_t len,
	double *min_mass, double *first2_mass, double *first3_mass)
{
	size_t pos = 0;
	uint64_t cumulative = 0;
	double out[3] = { 0.0, 0.0, 0.0 };

	*min_mass = 0.0;
	*first2_mass = 0.0;
	*first3_mass = 0.0;
	if (norms == NULL || len == 0) {
		return;
	}
	for (unsigned shell = 0; shell < 3; shell ++) {
		if (pos >= len) {
			out[shell] = cumulative / (double)len;
			continue;
		}
		uint64_t value = norms[pos];
		size_t start = pos;

		while (pos < len && norms[pos] == value) {
			pos ++;
		}
		cumulative += (uint64_t)(pos - start);
		out[shell] = cumulative / (double)len;
	}
	*min_mass = out[0];
	*first2_mass = out[1] > 0.0 ? out[1] : out[0];
	*first3_mass = out[2] > 0.0 ? out[2]
		: (*first2_mass > 0.0 ? *first2_mass : out[0]);
}

static int
collect_sigma_sign_block_stats(sigma_sign_sweep_result *res,
	uint64_t *rng_state)
{
	size_t total_capacity;
	size_t trials_per_label;
	uint64_t *norms = NULL;
	uint64_t component_counts[E8_COMPONENTS];
	double pred_component_freq[E8_COMPONENTS];
	double per_label_means[E8_TAU_LABELS];
	double per_label_vars[E8_TAU_LABELS];
	size_t per_label_count = 0;
	double mean_of_means = 0.0;
	double mean_m2 = 0.0;
	int ok = 1;

	if (res->block_trials_per_label == 0) {
		return 0;
	}
	trials_per_label = (size_t)res->block_trials_per_label;
	if (SIZE_MAX / trials_per_label < E8_TAU_LABELS) {
		return 0;
	}
	total_capacity = trials_per_label * E8_TAU_LABELS;
	if (total_capacity > SIZE_MAX / sizeof *norms) {
		return 0;
	}
	norms = malloc(total_capacity * sizeof *norms);
	if (norms == NULL || !e8_sampler_warm_cache(res->sigma_sign))
	{
		free(norms);
		return 0;
	}
	memset(component_counts, 0, sizeof component_counts);

	for (unsigned tau = 0; tau < E8_TAU_LABELS; tau ++) {
		stat_u64 tau_norm;
		size_t tau_start = (size_t)res->block_samples;
		size_t tau_count = 0;

		memset(&tau_norm, 0, sizeof tau_norm);
		for (unsigned sample = 0;
			sample < res->block_trials_per_label; sample ++)
		{
			int32_t zblk[8];
			uint64_t norm2 = 0;
			e8_ca_sample_trace trace;

			memset(&trace, 0, sizeof trace);
			if (!e8_sample_block_construction_a_cm_trace(zblk,
					(uint8_t)tau, res->sigma_sign,
					summary_rng, rng_state,
					&norm2, NULL, &trace))
			{
				ok = 0;
				continue;
			}
			norms[res->block_samples] = norm2;
			tau_count ++;
			res->block_samples ++;
			stat_u64_add(&res->block_norm, norm2);
			if (trace.component < E8_COMPONENTS) {
				component_counts[trace.component] ++;
			}
			stat_u64_add(&tau_norm, norm2);
		}
		if (tau_norm.count != 0) {
			double d;
			double min_mass, first2_mass, first3_mass;

			per_label_means[per_label_count] = tau_norm.mean;
			per_label_vars[per_label_count] =
				stat_u64_variance(&tau_norm);
			qsort(&norms[tau_start], tau_count,
				sizeof *norms, cmp_u64);
			compute_low_shell_masses(&norms[tau_start], tau_count,
				&min_mass, &first2_mass, &first3_mass);
			(void)first2_mass;
			if (per_label_count == 0
				|| min_mass < res->per_label_worst_min_shell_mass)
			{
				res->per_label_worst_min_shell_mass = min_mass;
			}
			if (per_label_count == 0
				|| first3_mass
					< res->per_label_worst_first3_shell_mass)
			{
				res->per_label_worst_first3_shell_mass =
					first3_mass;
			}
			per_label_count ++;
			d = tau_norm.mean - mean_of_means;
			mean_of_means += d / (double)per_label_count;
			mean_m2 += d * (tau_norm.mean - mean_of_means);
		}
	}

	if (res->block_samples != 0) {
		qsort(norms, (size_t)res->block_samples,
			sizeof *norms, cmp_u64);
		res->block_norm_p50 = quantile_u64_sorted(norms,
			(size_t)res->block_samples, 0.50);
		res->block_norm_p90 = quantile_u64_sorted(norms,
			(size_t)res->block_samples, 0.90);
		res->block_norm_p99 = quantile_u64_sorted(norms,
			(size_t)res->block_samples, 0.99);
		res->block_norm_max = norms[res->block_samples - 1];
		compute_low_shell_masses(norms, (size_t)res->block_samples,
			&res->block_min_shell_mass,
			&res->block_first2_shell_mass,
			&res->block_first3_shell_mass);
		res->emp_component_entropy_bits =
			entropy_bits_from_counts(component_counts,
				E8_COMPONENTS, res->block_samples);
	}
	free(norms);

	if (per_label_count != 0) {
		qsort(per_label_means, per_label_count,
			sizeof *per_label_means, cmp_double);
		qsort(per_label_vars, per_label_count,
			sizeof *per_label_vars, cmp_double);
		res->per_label_mean_min = per_label_means[0];
		res->per_label_mean_median =
			median_double_sorted(per_label_means, per_label_count);
		res->per_label_mean_max =
			per_label_means[per_label_count - 1];
		res->per_label_mean_std =
			sqrt(mean_m2 / (double)per_label_count);
		res->per_label_variance_median =
			median_double_sorted(per_label_vars, per_label_count);
		res->per_label_variance_max =
			per_label_vars[per_label_count - 1];
	}

	res->pred_component_available = compute_pred_component_freq(
		pred_component_freq, &res->pred_component_entropy_bits,
		res->sigma_sign);
	if (res->pred_component_available && res->block_samples != 0) {
		for (unsigned component = 0; component < E8_COMPONENTS;
			component ++)
		{
			double pred = pred_component_freq[component];

			if (pred > 0.0) {
				double empirical = (double)component_counts[component]
					/ (double)res->block_samples;
				double rel = fabs(empirical - pred) / pred;
				if (rel > res->component_relerr_max) {
					res->component_relerr_max = rel;
				}
			}
		}
	}
	return ok;
}

static int
collect_sigma_sign_signature_stats(sigma_sign_sweep_result *res,
	uint64_t *rng_state)
{
	unsigned logn = res->logn;
	size_t n = (size_t)1 << logn;
	size_t hpub_len = (size_t)1 << (logn - 4);
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	uint64_t *sign_norms = NULL;
	uint64_t *coeff_abs = NULL;
	size_t coeff_cap, coeff_count = 0;
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	uint8_t hpub[64], sig[40 + 4 * MAXN], salt[40];
	int32_t z0[MAXN], z1[MAXN];
	int16_t s0[MAXN], s1[MAXN];
	unsigned threads, rng_mode;
	int ok = 1;

	if (res->sign_trials == 0) {
		return 1;
	}
	if (res->sign_trials > SIZE_MAX / (2 * n)) {
		return 0;
	}
	sign_norms = malloc((size_t)res->sign_trials * sizeof *sign_norms);
	coeff_cap = (size_t)res->sign_trials * 2 * n;
	coeff_abs = malloc(coeff_cap * sizeof *coeff_abs);
	if (sign_norms == NULL || coeff_abs == NULL) {
		free(sign_norms);
		free(coeff_abs);
		return 0;
	}
	if (!make_basis(logn, f, g, F, G, rng_state)) {
		free(sign_norms);
		free(coeff_abs);
		return 0;
	}
	make_hpub(hpub, hpub_len, logn, 0);
	if (!selected_e8_sampler_config(logn, &threads, &rng_mode))
	{
		free(sign_norms);
		free(coeff_abs);
		return 0;
	}
	e8_sampler_set_thread_count(threads);
	e8_sampler_set_rng_mode(rng_mode);

	for (unsigned trial = 0; trial < res->sign_trials; trial ++) {
		shake_context sc_data;
		e8_sign_trace_timing trace;
		e8_sampler_profile profile;
		int64_t pnorm = 0;
		unsigned attempts = SIGMA_SIGN_SWEEP_MAX_ATTEMPTS;
		int accepted;

		make_salt(salt, salt_len, logn, 0, trial);
		make_message_context(&sc_data, logn, 0, trial);
		memset(&trace, 0, sizeof trace);
		e8_sampler_profile_reset();
		accepted = e8_sign_sampler_trace_timed_uncompressed(logn,
			sig, sig_len, &sc_data, hpub, hpub_len,
			f, g, F, G, salt,
			res->sigma_sign, SIGMA_SIGN_SWEEP_VERIFY_LOOSE,
			SIGMA_SIGN_SWEEP_MAX_ATTEMPTS, summary_rng,
			rng_state, z0, z1, &pnorm, &attempts, &trace);
		(void)attempts;
		if (e8_sampler_profile_get(&profile)) {
			stat_u64_add(&res->profile_cycles_block_sample,
				profile.cycles_block_sample);
			stat_u64_add(&res->profile_wall_ns_block_sample,
				profile.wall_ns_block_sample);
		}
		if (!accepted) {
			ok = 0;
			continue;
		}
		sign_norms[res->sign_norm.count] = (uint64_t)pnorm;
		stat_u64_add(&res->sign_norm, (uint64_t)pnorm);
		if (e8_decode_sig_uncompressed(logn, NULL, s0, s1,
				sig, sig_len))
		{
			for (size_t u = 0; u < n; u ++) {
				uint64_t a0 = s0[u] < 0
					? (uint64_t)(-(int)s0[u])
					: (uint64_t)s0[u];
				uint64_t a1 = s1[u] < 0
					? (uint64_t)(-(int)s1[u])
					: (uint64_t)s1[u];
				if (coeff_count + 2 <= coeff_cap) {
					coeff_abs[coeff_count ++] = a0;
					coeff_abs[coeff_count ++] = a1;
				}
				stat_u64_add(&res->sig_coeff_abs, a0);
				stat_u64_add(&res->sig_coeff_abs, a1);
			}
		}
	}

	if (res->sign_norm.count != 0) {
		qsort(sign_norms, (size_t)res->sign_norm.count,
			sizeof *sign_norms, cmp_u64);
		res->sign_norm_p50 = quantile_u64_sorted(sign_norms,
			(size_t)res->sign_norm.count, 0.50);
		res->sign_norm_p90 = quantile_u64_sorted(sign_norms,
			(size_t)res->sign_norm.count, 0.90);
		res->sign_norm_p99 = quantile_u64_sorted(sign_norms,
			(size_t)res->sign_norm.count, 0.99);
		res->sign_norm_max = sign_norms[res->sign_norm.count - 1];
	}
	if (coeff_count != 0) {
		qsort(coeff_abs, coeff_count, sizeof *coeff_abs, cmp_u64);
		res->sig_coeff_abs_p99 = quantile_u64_sorted(coeff_abs,
			coeff_count, 0.99);
		res->sig_coeff_abs_max = coeff_abs[coeff_count - 1];
	}
	free(sign_norms);
	free(coeff_abs);
	return ok;
}

static void
write_sigma_sign_sweep_header(FILE *fp)
{
	fprintf(fp,
		"logn,n,eta,sigma_sign,block_trials_per_label,"
		"block_samples,block_norm_mean,block_norm_variance,block_norm_std,"
		"block_norm_p50,block_norm_p90,block_norm_p99,block_norm_max,"
		"block_min_shell_mass,block_first2_shell_mass,"
		"block_first3_shell_mass,"
		"per_label_mean_min,per_label_mean_median,"
		"per_label_mean_max,per_label_mean_std,"
		"per_label_variance_median,per_label_variance_max,"
		"per_label_worst_min_shell_mass,"
		"per_label_worst_first3_shell_mass,"
		"pred_component_entropy_bits,emp_component_entropy_bits,"
		"component_relerr_max,sign_trials,sign_norm_mean,sign_norm_std,"
		"sign_norm_p50,sign_norm_p90,sign_norm_p99,sign_norm_max,"
		"sig_coeff_abs_mean,sig_coeff_abs_p99,sig_coeff_abs_max,"
		"sampler_profile_cycles_block_sample_mean,"
		"sampler_profile_wall_ns_block_sample_mean,status\n");
}

static void
write_sigma_sign_sweep_row(FILE *fp, const sigma_sign_sweep_result *res)
{
	size_t n = (size_t)1 << res->logn;
	int have_blocks = res->block_samples != 0;
	int have_signs = res->sign_norm.count != 0;
	int have_coeffs = res->sig_coeff_abs.count != 0;
	double block_var = stat_u64_variance(&res->block_norm);
	double sign_var = stat_u64_variance(&res->sign_norm);

	fprintf(fp, "%u,%u,%u,%.3f,%u",
		res->logn, (unsigned)n, eta_for_logn(res->logn),
		res->sigma_sign, res->block_trials_per_label);
	fprintf(fp, ",%llu", (unsigned long long)res->block_samples);
	fputc(',', fp); csv_double(fp, res->block_norm.mean, have_blocks);
	fputc(',', fp); csv_double(fp, block_var, have_blocks);
	fputc(',', fp); csv_double(fp, sqrt(block_var), have_blocks);
	fputc(',', fp); csv_u64(fp, res->block_norm_p50, have_blocks);
	fputc(',', fp); csv_u64(fp, res->block_norm_p90, have_blocks);
	fputc(',', fp); csv_u64(fp, res->block_norm_p99, have_blocks);
	fputc(',', fp); csv_u64(fp, res->block_norm_max, have_blocks);
	fputc(',', fp); csv_double(fp, res->block_min_shell_mass, have_blocks);
	fputc(',', fp); csv_double(fp, res->block_first2_shell_mass, have_blocks);
	fputc(',', fp); csv_double(fp, res->block_first3_shell_mass, have_blocks);
	fputc(',', fp); csv_double(fp, res->per_label_mean_min, have_blocks);
	fputc(',', fp); csv_double(fp, res->per_label_mean_median, have_blocks);
	fputc(',', fp); csv_double(fp, res->per_label_mean_max, have_blocks);
	fputc(',', fp); csv_double(fp, res->per_label_mean_std, have_blocks);
	fputc(',', fp); csv_double(fp,
		res->per_label_variance_median, have_blocks);
	fputc(',', fp); csv_double(fp, res->per_label_variance_max, have_blocks);
	fputc(',', fp); csv_double(fp,
		res->per_label_worst_min_shell_mass, have_blocks);
	fputc(',', fp); csv_double(fp,
		res->per_label_worst_first3_shell_mass, have_blocks);
	fputc(',', fp); csv_double(fp, res->pred_component_entropy_bits,
		res->pred_component_available);
	fputc(',', fp); csv_double(fp, res->emp_component_entropy_bits,
		have_blocks);
	fputc(',', fp); csv_double(fp, res->component_relerr_max,
		res->pred_component_available && have_blocks);
	fprintf(fp, ",%u", res->sign_trials);
	fputc(',', fp); csv_double(fp, res->sign_norm.mean, have_signs);
	fputc(',', fp); csv_double(fp, sqrt(sign_var), have_signs);
	fputc(',', fp); csv_u64(fp, res->sign_norm_p50, have_signs);
	fputc(',', fp); csv_u64(fp, res->sign_norm_p90, have_signs);
	fputc(',', fp); csv_u64(fp, res->sign_norm_p99, have_signs);
	fputc(',', fp); csv_u64(fp, res->sign_norm_max, have_signs);
	fputc(',', fp); csv_double(fp, res->sig_coeff_abs.mean, have_coeffs);
	fputc(',', fp); csv_u64(fp, res->sig_coeff_abs_p99, have_coeffs);
	fputc(',', fp); csv_u64(fp, res->sig_coeff_abs_max, have_coeffs);
	fputc(',', fp); csv_double(fp, res->profile_cycles_block_sample.mean,
		res->profile_cycles_block_sample.count != 0);
	fputc(',', fp); csv_double(fp, res->profile_wall_ns_block_sample.mean,
		res->profile_wall_ns_block_sample.count != 0);
	fprintf(fp, ",%s\n", res->status);
}

static int
run_sigma_sign_sweep(void)
{
	unsigned block_trials_per_label, sign_trials, logn_filter;
	uint64_t base_seed;
	int status = 0;

	if (!parse_unsigned_env("E8_SIGMA_SIGN_SWEEP_BLOCK_TRIALS_PER_LABEL",
			DEFAULT_SIGMA_SIGN_SWEEP_BLOCK_TRIALS_PER_LABEL,
			&block_trials_per_label)
		|| !parse_unsigned_env("E8_SIGMA_SIGN_SWEEP_SIGN_TRIALS",
			DEFAULT_SIGMA_SIGN_SWEEP_SIGN_TRIALS, &sign_trials)
		|| !parse_logn_filter_env("E8_SIGMA_SIGN_SWEEP_LOGN",
			1, &logn_filter)
		|| !parse_seed_env("E8_SIGMA_SIGN_SWEEP_SEED",
			SIGMA_SIGN_SWEEP_DEFAULT_SEED, &base_seed))
	{
		return 1;
	}

	write_sigma_sign_sweep_header(stdout);
	for (size_t gi = 0; gi < ARRAY_LEN(SIGMA_SIGN_SWEEP_GRID); gi ++) {
		const sigma_sign_sweep_grid *grid = &SIGMA_SIGN_SWEEP_GRID[gi];

		if (logn_filter != 0 && grid->logn != logn_filter) {
			continue;
		}
		for (size_t si = 0; si < grid->sigma_sign_len; si ++) {
			sigma_sign_sweep_result res;
			uint64_t block_rng;
			uint64_t sign_rng;
			int block_ok, sign_ok;

			memset(&res, 0, sizeof res);
			res.logn = grid->logn;
			res.sigma_sign = grid->sigma_sign[si];
			res.block_trials_per_label = block_trials_per_label;
			res.sign_trials = sign_trials;
			strcpy(res.status, "ok");
			block_rng = sigma_sign_sweep_seed(base_seed,
				grid->logn, si, 0);
			sign_rng = sigma_sign_sweep_seed(base_seed,
				grid->logn, si, 1);
			block_ok = collect_sigma_sign_block_stats(&res,
				&block_rng);
			sign_ok = collect_sigma_sign_signature_stats(&res,
				&sign_rng);
			if (!block_ok && !sign_ok) {
				strcpy(res.status, "block_and_sign_sweep_failed");
				status = 1;
			} else if (!block_ok) {
				strcpy(res.status, "block_sweep_failed");
				status = 1;
			} else if (!sign_ok) {
				strcpy(res.status, "signing_sweep_failed");
			}
			write_sigma_sign_sweep_row(stdout, &res);
		}
	}
	return status;
}

static int
sigma_verify_sweep_mode_requested(void)
{
	return env_flag_requested("E8_SIGMA_VERIFY_SWEEP")
		|| getenv("E8_SIGMA_VERIFY_SWEEP_TRIALS") != NULL
		|| getenv("E8_SIGMA_VERIFY_SWEEP_GRID") != NULL
		|| getenv("E8_SIGMA_VERIFY_SWEEP_LOGN") != NULL
		|| getenv("E8_SIGMA_VERIFY_SWEEP_SEED") != NULL;
}

static int
sigma_sign_sweep_mode_requested(void)
{
	return env_flag_requested("E8_SIGMA_SIGN_SWEEP")
		|| getenv("E8_SIGMA_SIGN_SWEEP_BLOCK_TRIALS_PER_LABEL") != NULL
		|| getenv("E8_SIGMA_SIGN_SWEEP_SIGN_TRIALS") != NULL
		|| getenv("E8_SIGMA_SIGN_SWEEP_LOGN") != NULL
		|| getenv("E8_SIGMA_SIGN_SWEEP_SEED") != NULL;
}

int
main(void)
{
	unsigned trials, keys, logn_filter;
	int status = 0;

	if (sigma_sign_sweep_mode_requested()) {
		return run_sigma_sign_sweep();
	}
	if (sigma_verify_sweep_mode_requested()) {
		return run_sigma_verify_sweep();
	}

	if (!parse_unsigned_env("E8_REJECTION_TRIALS",
			DEFAULT_TRIALS, &trials)
		|| !parse_unsigned_env("E8_REJECTION_KEYS",
			DEFAULT_KEYS, &keys)
		|| !parse_logn_filter_env("E8_REJECTION_LOGN",
			0, &logn_filter))
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
