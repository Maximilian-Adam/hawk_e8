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
#define E8_HIST_DEFAULT_KEYS         3
#define E8_HIST_DEFAULT_TRIALS       5

/*
 * Experimental E8 calibration logger.
 *
 * This program records CSV data for the current floating-point
 * Construction-A CM E8 prototype.  It does not alter signer, sampler, or
 * verifier behavior, and its parameters are not final security claims.
 */

typedef struct {
	unsigned logn;
	double sigma_sign;
	double sigma_verify;
	unsigned max_attempts;
} hist_param;

typedef struct {
	int32_t min;
	int32_t max;
	int64_t absmax;
	unsigned bits;
} coeff_stats;

typedef struct {
	uint64_t cycles_keygen;
	uint64_t cycles_public_form;
	uint64_t cycles_sample_total;
	uint64_t cycles_sample_last;
	uint64_t cycles_sign_total;
	uint64_t cycles_verify;
	uint64_t wall_ns_keygen;
	uint64_t wall_ns_public_form;
	uint64_t wall_ns_sample_total;
	uint64_t wall_ns_sample_last;
	uint64_t wall_ns_sign_total;
	uint64_t wall_ns_verify;
} hist_timing;

typedef struct {
	int coset_check_success;
	int piM_matches_t;
	int ambient_mod2_matches_t;
	unsigned target_t_weight;
	unsigned piM_t_weight;
	unsigned ambient_t_weight;
	unsigned coset_error_count;
	unsigned ambient_error_count;
} hist_coset_diag;

/*
 * Experimental calibration defaults only.  These are for CSV logging with
 * the Construction-A CM sampler and are not final E8 parameter claims.
 */
static const hist_param PARAMS[] = {
	{ 8,  1.26,  0.73, 1000 },
	{ 9,  1.278, 0.72, 1000 },
	{ 10, 1.299, 0.71, 1000 }
};

static uint64_t
hist_wall_ns(void)
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
hist_cycles_start(void)
{
#if defined(__x86_64__)
	_mm_lfence();
	return __rdtsc();
#else
	return 0;
#endif
}

static uint64_t
hist_cycles_end(void)
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
hist_cycles_delta(uint64_t t0, uint64_t t1)
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
hist_wall_delta(uint64_t t0, uint64_t t1)
{
	if (t0 == 0 && t1 == 0) {
		return 0;
	}
	if (t1 <= t0) {
		return 1;
	}
	return t1 - t0;
}

static int
parse_count_override(const char *name, unsigned *value)
{
	const char *env = getenv(name);

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

static uint64_t
rng_next_u64(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

static void
hist_rng(void *ctx, void *dst, size_t len)
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
hash_to_h(unsigned logn, uint8_t *h,
	const shake_context *sc_data, const void *hpub,
	const uint8_t *salt, size_t salt_len);

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

static unsigned
absmax_bits(int64_t x)
{
	unsigned bits = 0;

	while (x != 0) {
		bits ++;
		x >>= 1;
	}
	return bits;
}

static coeff_stats
stats_i32(const int32_t *a, size_t n)
{
	coeff_stats st;

	st.min = a[0];
	st.max = a[0];
	st.absmax = 0;
	for (size_t u = 0; u < n; u ++) {
		int64_t x = a[u];
		int64_t ax = x < 0 ? -x : x;

		if (a[u] < st.min) {
			st.min = a[u];
		}
		if (a[u] > st.max) {
			st.max = a[u];
		}
		if (ax > st.absmax) {
			st.absmax = ax;
		}
	}
	st.bits = absmax_bits(st.absmax);
	return st;
}

static coeff_stats
stats_i16(const int16_t *a, size_t n)
{
	coeff_stats st;

	st.min = a[0];
	st.max = a[0];
	st.absmax = 0;
	for (size_t u = 0; u < n; u ++) {
		int64_t x = a[u];
		int64_t ax = x < 0 ? -x : x;

		if (a[u] < st.min) {
			st.min = a[u];
		}
		if (a[u] > st.max) {
			st.max = a[u];
		}
		if (ax > st.absmax) {
			st.absmax = ax;
		}
	}
	st.bits = absmax_bits(st.absmax);
	return st;
}

static void
make_message_context(shake_context *sc_data,
	unsigned logn, unsigned keynum, unsigned trial)
{
	static const char prefix[] = "experimental e8 histogram";
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
		salt[u] = (uint8_t)(0x79u + 31u * u
			+ 7u * logn + 3u * keynum + trial);
	}
}

static int
make_basis(unsigned logn, int8_t *f, int8_t *g, int8_t *F, int8_t *G,
	uint64_t *rng_state)
{
	uint8_t tmp[HAWK_TMPSIZE_KEYGEN(10)];
	uint8_t seed[40];

	return Hawk_keygen(logn, f, g, F, G, NULL, NULL, NULL, seed,
		hist_rng, rng_state, tmp, sizeof tmp) == 0;
}

static int64_t
pnorm_from_apply(unsigned logn, const int32_t *z0, const int32_t *z1)
{
	size_t n = (size_t)1 << logn;
	static int32_t pz0[MAXN], pz1[MAXN];
	int64_t norm = 0;

	e8_apply_P(pz0, pz1, z0, z1, logn);
	for (size_t u = 0; u < n; u ++) {
		norm += (int64_t)pz0[u] * pz0[u]
			+ (int64_t)pz1[u] * pz1[u];
	}
	return norm;
}

static hist_coset_diag
compute_coset_diag(unsigned logn,
	const uint8_t *t0, const uint8_t *t1,
	const int32_t *z0, const int32_t *z1)
{
	size_t n = (size_t)1 << logn;
	int32_t pz0[MAXN], pz1[MAXN];
	hist_coset_diag d;

	memset(&d, 0, sizeof d);
	e8_apply_P(pz0, pz1, z0, z1, logn);

	/*
	 * HAWK-E8-CM signs against the internal E8-module quotient.  The
	 * sampled vector is carried externally as z, with x_E = P_n z; the
	 * mathematically correct coset interface is therefore
	 *
	 *     pi_M(x_E) = z mod 2 = t.
	 *
	 * The ambient coefficientwise reduction x_E mod 2 is logged below as
	 * a diagnostic only.  It is not the canonical quotient check and must
	 * not replace the internal pi_M check.
	 */
	for (size_t u = 0; u < n; u ++) {
		unsigned target0 = t0[u];
		unsigned target1 = t1[u];
		unsigned zbit0 = i32_bit(z0, u);
		unsigned zbit1 = i32_bit(z1, u);
		unsigned xbit0 = i32_bit(pz0, u);
		unsigned xbit1 = i32_bit(pz1, u);

		d.target_t_weight += target0 + target1;
		d.piM_t_weight += zbit0 + zbit1;
		d.ambient_t_weight += xbit0 + xbit1;
		d.coset_error_count += (zbit0 != target0);
		d.coset_error_count += (zbit1 != target1);
		d.ambient_error_count += (xbit0 != target0);
		d.ambient_error_count += (xbit1 != target1);
	}
	d.piM_matches_t = d.coset_error_count == 0;
	d.coset_check_success = d.piM_matches_t;
	d.ambient_mod2_matches_t = d.ambient_error_count == 0;
	return d;
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

static void
write_public_header(FILE *fp)
{
	fprintf(fp, "logn,n,key_index,"
		"q00_min,q00_max,q00_absmax,q00_absmax_bits,"
		"q01_min,q01_max,q01_absmax,q01_absmax_bits,"
		"q11_min,q11_max,q11_absmax,q11_absmax_bits\n");
}

static void
write_signature_header(FILE *fp)
{
	fprintf(fp, "logn,n,key_index,trial_index,accepted,attempts,"
		"rejected_attempts,sigma_sign,sigma_verify,"
		"pnorm,qnorm,norm_equal,"
		"s0_min,s0_max,s0_absmax,s0_absmax_bits,"
		"s1_min,s1_max,s1_absmax,s1_absmax_bits,"
		"z0_min,z0_max,z0_absmax,z0_absmax_bits,"
		"z1_min,z1_max,z1_absmax,z1_absmax_bits,"
		"test_type,expected_verify,verify_success,"
		"norm_margin,verify_bound,"
		"coset_check_success,piM_matches_t,"
		"ambient_mod2_matches_t,target_t_weight,"
		"piM_t_weight,ambient_t_weight,"
		"coset_error_count,ambient_error_count,"
		"cycles_keygen,cycles_public_form,"
		"cycles_sample_total,cycles_sample_last,"
		"cycles_sign_total,cycles_verify,"
		"wall_ns_keygen,wall_ns_public_form,"
		"wall_ns_sample_total,wall_ns_sample_last,"
		"wall_ns_sign_total,wall_ns_verify\n");
}

static void
write_public_row(FILE *fp, unsigned logn, unsigned key_index,
	const int32_t *q00, const int32_t *q01, const int32_t *q11)
{
	size_t n = (size_t)1 << logn;
	coeff_stats s00 = stats_i32(q00, n);
	coeff_stats s01 = stats_i32(q01, n);
	coeff_stats s11 = stats_i32(q11, n);

	fprintf(fp, "%u,%u,%u,"
		"%d,%d,%lld,%u,"
		"%d,%d,%lld,%u,"
		"%d,%d,%lld,%u\n",
		logn, (unsigned)n, key_index,
		s00.min, s00.max, (long long)s00.absmax, s00.bits,
		s01.min, s01.max, (long long)s01.absmax, s01.bits,
		s11.min, s11.max, (long long)s11.absmax, s11.bits);
}

static void
write_signature_row(FILE *fp, unsigned logn, unsigned key_index,
	unsigned trial_index, int accepted, unsigned attempts,
	double sigma_sign, double sigma_verify,
	int64_t pnorm, int64_t qnorm, int norm_equal,
	const int16_t *s0, const int16_t *s1,
	const int32_t *z0, const int32_t *z1,
	const char *test_type, int expected_verify, int verify_success,
	int64_t norm_margin, int64_t verify_bound,
	const hist_coset_diag *coset_diag, const hist_timing *timing)
{
	size_t n = (size_t)1 << logn;
	unsigned rejected_attempts = accepted ? attempts - 1 : attempts;
	coeff_stats ss0, ss1, sz0, sz1;
	static const int16_t zero16[MAXN];
	static const int32_t zero32[MAXN];
	static const hist_coset_diag zero_coset_diag;
	static const hist_timing zero_timing;

	if (s0 == NULL) {
		s0 = zero16;
	}
	if (s1 == NULL) {
		s1 = zero16;
	}
	if (z0 == NULL) {
		z0 = zero32;
	}
	if (z1 == NULL) {
		z1 = zero32;
	}
	if (timing == NULL) {
		timing = &zero_timing;
	}
	if (coset_diag == NULL) {
		coset_diag = &zero_coset_diag;
	}
	ss0 = stats_i16(s0, n);
	ss1 = stats_i16(s1, n);
	sz0 = stats_i32(z0, n);
	sz1 = stats_i32(z1, n);

	fprintf(fp, "%u,%u,%u,%u,%d,%u,%u,%.17g,%.17g,"
		"%lld,%lld,%d,"
		"%d,%d,%lld,%u,"
		"%d,%d,%lld,%u,"
		"%d,%d,%lld,%u,"
		"%d,%d,%lld,%u,"
		"%s,%d,%d,%lld,%lld,"
		"%d,%d,%d,%u,%u,%u,%u,%u,"
		"%llu,%llu,%llu,%llu,%llu,%llu,"
		"%llu,%llu,%llu,%llu,%llu,%llu\n",
		logn, (unsigned)n, key_index, trial_index, accepted,
		attempts, rejected_attempts, sigma_sign, sigma_verify,
		(long long)pnorm, (long long)qnorm, norm_equal,
		ss0.min, ss0.max, (long long)ss0.absmax, ss0.bits,
		ss1.min, ss1.max, (long long)ss1.absmax, ss1.bits,
		sz0.min, sz0.max, (long long)sz0.absmax, sz0.bits,
		sz1.min, sz1.max, (long long)sz1.absmax, sz1.bits,
		test_type, expected_verify, verify_success,
		(long long)norm_margin, (long long)verify_bound,
		coset_diag->coset_check_success,
		coset_diag->piM_matches_t,
		coset_diag->ambient_mod2_matches_t,
		coset_diag->target_t_weight,
		coset_diag->piM_t_weight,
		coset_diag->ambient_t_weight,
		coset_diag->coset_error_count,
		coset_diag->ambient_error_count,
		(unsigned long long)timing->cycles_keygen,
		(unsigned long long)timing->cycles_public_form,
		(unsigned long long)timing->cycles_sample_total,
		(unsigned long long)timing->cycles_sample_last,
		(unsigned long long)timing->cycles_sign_total,
		(unsigned long long)timing->cycles_verify,
		(unsigned long long)timing->wall_ns_keygen,
		(unsigned long long)timing->wall_ns_public_form,
		(unsigned long long)timing->wall_ns_sample_total,
		(unsigned long long)timing->wall_ns_sample_last,
		(unsigned long long)timing->wall_ns_sign_total,
		(unsigned long long)timing->wall_ns_verify);
}

static int
compute_case_qnorm(int64_t *qnorm, int64_t *norm_margin,
	unsigned logn, double sigma_verify,
	const uint8_t *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01)
{
	size_t salt_len = e8_salt_len(logn);
	uint8_t salt[40];
	int16_t s0[MAXN], s1[MAXN];
	int32_t w0[MAXN], w1[MAXN];
	int64_t bound;

	*qnorm = 0;
	*norm_margin = 0;
	if (salt_len == 0
		|| hpub_len != ((size_t)1 << (logn - 4))
		|| !e8_verify_bound_from_sigma(logn, sigma_verify, &bound)
		|| !e8_decode_sig_uncompressed(logn,
			salt, s0, s1, sig, sig_len))
	{
		return 0;
	}
	reconstruct_w(w0, w1, logn, hpub, salt, salt_len, sc_data, s0, s1);
	if (!e8_qnorm_completion(qnorm, q00, q01, w0, w1, logn)) {
		return 0;
	}
	*norm_margin = bound - *qnorm;
	return 1;
}

static int
write_verify_case_row(FILE *sig_fp, const hist_param *param,
	unsigned key_index, unsigned trial_index, const char *test_type,
	int expected_verify,
	const uint8_t *sig, size_t sig_len,
	const shake_context *sc_data, const uint8_t *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	int accepted, unsigned attempts,
	int64_t pnorm, const int32_t *z0, const int32_t *z1,
	const hist_coset_diag *coset_diag,
	const hist_timing *base_timing)
{
	unsigned logn = param->logn;
	uint8_t salt[40];
	int16_t s0[MAXN], s1[MAXN];
	int64_t verify_bound = 0, qnorm = 0, norm_margin = 0;
	hist_timing row_timing;
	uint64_t verify_c0, verify_c1, verify_w0, verify_w1;
	int have_qnorm, norm_equal, verify_success, decoded;

	if (base_timing != NULL) {
		row_timing = *base_timing;
	} else {
		memset(&row_timing, 0, sizeof row_timing);
	}
	e8_verify_bound_from_sigma(logn, param->sigma_verify, &verify_bound);
	decoded = e8_decode_sig_uncompressed(logn,
		salt, s0, s1, sig, sig_len);
	have_qnorm = compute_case_qnorm(&qnorm, &norm_margin,
		logn, param->sigma_verify, sig, sig_len, sc_data,
		hpub, hpub_len, q00, q01);
	norm_equal = have_qnorm && pnorm == qnorm;
	verify_c0 = hist_cycles_start();
	verify_w0 = hist_wall_ns();
	verify_success = e8_verify_uncompressed_with_sigma(logn,
		sig, sig_len, sc_data, hpub, hpub_len,
		q00, q01, q10, q11, param->sigma_verify);
	verify_w1 = hist_wall_ns();
	verify_c1 = hist_cycles_end();
	row_timing.cycles_verify = hist_cycles_delta(verify_c0, verify_c1);
	row_timing.wall_ns_verify = hist_wall_delta(verify_w0, verify_w1);

	write_signature_row(sig_fp, logn, key_index, trial_index,
		accepted, attempts, param->sigma_sign, param->sigma_verify,
		pnorm, qnorm, norm_equal,
		decoded ? s0 : NULL, decoded ? s1 : NULL, z0, z1,
		test_type, expected_verify, verify_success,
		have_qnorm ? norm_margin : 0, verify_bound,
		coset_diag, &row_timing);

	return verify_success == expected_verify;
}

static int
collect_signature_row(FILE *sig_fp, const hist_param *param,
	unsigned key_index, unsigned trial_index,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const uint8_t *hpub, uint64_t *rng_state, unsigned max_attempts,
	const hist_timing *key_timing)
{
	unsigned logn = param->logn;
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	uint8_t sig[40 + 4 * MAXN], salt[40];
	uint8_t bad_sig[40 + 4 * MAXN], bad_hpub[64];
	uint8_t t0[MAXN], t1[MAXN];
	int16_t s0[MAXN], s1[MAXN];
	int32_t z0[MAXN], z1[MAXN], w0[MAXN], w1[MAXN];
	int32_t bad_q00[MAXN], bad_q01[MAXN], bad_q10[MAXN], bad_q11[MAXN];
	int64_t pnorm = 0, pnorm_check = 0, qnorm = 0;
	int64_t verify_bound = 0;
	unsigned attempts = max_attempts;
	int accepted, norm_equal = 1;
	int status_ok = 1;
	hist_coset_diag coset_diag;
	hist_timing timing;
	e8_sign_trace_timing sign_trace;
	uint64_t sign_c0, sign_c1, sign_w0, sign_w1;
	shake_context sc_data;

	if (key_timing != NULL) {
		timing = *key_timing;
	} else {
		memset(&timing, 0, sizeof timing);
	}
	memset(&sign_trace, 0, sizeof sign_trace);
	e8_verify_bound_from_sigma(logn, param->sigma_verify, &verify_bound);
	make_salt(salt, salt_len, logn, key_index, trial_index);
	make_message_context(&sc_data, logn, key_index, trial_index);
	sign_c0 = hist_cycles_start();
	sign_w0 = hist_wall_ns();
	accepted = e8_sign_sampler_trace_timed_uncompressed(logn,
		sig, sig_len, &sc_data, hpub, (size_t)1 << (logn - 4),
		f, g, F, G, salt, param->sigma_sign, param->sigma_verify,
		max_attempts, hist_rng, rng_state, z0, z1, &pnorm,
		&attempts, &sign_trace);
	sign_w1 = hist_wall_ns();
	sign_c1 = hist_cycles_end();
	timing.cycles_sample_total = sign_trace.cycles_sample_total;
	timing.cycles_sample_last = sign_trace.cycles_sample_last;
	timing.cycles_sign_total = hist_cycles_delta(sign_c0, sign_c1);
	timing.wall_ns_sample_total = sign_trace.wall_ns_sample_total;
	timing.wall_ns_sample_last = sign_trace.wall_ns_sample_last;
	timing.wall_ns_sign_total = hist_wall_delta(sign_w0, sign_w1);
	if (!accepted) {
		write_signature_row(sig_fp, logn, key_index, trial_index,
			0, max_attempts, param->sigma_sign,
			param->sigma_verify, 0, 0, 1, NULL, NULL, NULL, NULL,
			"signing_failed", 0, 0, 0, verify_bound,
			NULL, &timing);
		return -1;
	}

	if (!e8_decode_sig_uncompressed(logn, NULL, s0, s1, sig, sig_len)) {
		write_signature_row(sig_fp, logn, key_index, trial_index,
			0, attempts, param->sigma_sign,
			param->sigma_verify, 0, 0, 1, NULL, NULL, NULL, NULL,
			"decode_failed", 0, 0, 0, verify_bound,
			NULL, &timing);
		return -1;
	}
	make_message_context(&sc_data, logn, key_index, trial_index);
	compute_target_t(t0, t1, logn, f, g, F, G,
		&sc_data, hpub, salt, salt_len);
	coset_diag = compute_coset_diag(logn, t0, t1, z0, z1);
	reconstruct_w(w0, w1, logn, hpub, salt, salt_len, &sc_data, s0, s1);
	pnorm_check = pnorm_from_apply(logn, z0, z1);
	if (!e8_qnorm_completion(&qnorm, q00, q01, w0, w1, logn)) {
		norm_equal = 0;
	} else if (pnorm != pnorm_check || pnorm_check != qnorm) {
		norm_equal = 0;
	}

	if (!write_verify_case_row(sig_fp, param, key_index, trial_index,
		"valid_signature", 1,
		sig, sig_len, &sc_data, hpub, (size_t)1 << (logn - 4),
		q00, q01, q10, q11, 1, attempts, pnorm_check, z0, z1,
		&coset_diag, &timing))
	{
		status_ok = 0;
	}
	if (!norm_equal) {
		status_ok = 0;
	}
	if (!coset_diag.piM_matches_t || !coset_diag.coset_check_success
		|| coset_diag.coset_error_count != 0)
	{
		status_ok = 0;
	}

	/*
	 * Negative verification rows.  These keep the original signing
	 * metrics and record the actual verifier result for each mutation.
	 */
	make_message_context(&sc_data, logn, key_index, trial_index ^ 0x80u);
	if (!write_verify_case_row(sig_fp, param, key_index, trial_index,
		"tamper_message", 0,
		sig, sig_len, &sc_data, hpub, (size_t)1 << (logn - 4),
		q00, q01, q10, q11, 1, attempts, pnorm_check, z0, z1,
		&coset_diag, &timing))
	{
		status_ok = 0;
	}

	memcpy(bad_sig, sig, sig_len);
	bad_sig[salt_len + 0] = 0xFF;
	bad_sig[salt_len + 1] = 0x7F;
	make_message_context(&sc_data, logn, key_index, trial_index);
	if (!write_verify_case_row(sig_fp, param, key_index, trial_index,
		"tamper_s0", 0,
		bad_sig, sig_len, &sc_data, hpub, (size_t)1 << (logn - 4),
		q00, q01, q10, q11, 1, attempts, pnorm_check, z0, z1,
		&coset_diag, &timing))
	{
		status_ok = 0;
	}

	memcpy(bad_sig, sig, sig_len);
	bad_sig[salt_len + (2 << logn) + 0] = 0xFF;
	bad_sig[salt_len + (2 << logn) + 1] = 0x7F;
	if (!write_verify_case_row(sig_fp, param, key_index, trial_index,
		"tamper_s1", 0,
		bad_sig, sig_len, &sc_data, hpub, (size_t)1 << (logn - 4),
		q00, q01, q10, q11, 1, attempts, pnorm_check, z0, z1,
		&coset_diag, &timing))
	{
		status_ok = 0;
	}

	memcpy(bad_sig, sig, sig_len);
	bad_sig[0] ^= 0x80u;
	if (!write_verify_case_row(sig_fp, param, key_index, trial_index,
		"tamper_salt", 0,
		bad_sig, sig_len, &sc_data, hpub, (size_t)1 << (logn - 4),
		q00, q01, q10, q11, 1, attempts, pnorm_check, z0, z1,
		&coset_diag, &timing))
	{
		status_ok = 0;
	}

	memcpy(bad_hpub, hpub, (size_t)1 << (logn - 4));
	bad_hpub[0] ^= 0x80u;
	if (!write_verify_case_row(sig_fp, param, key_index, trial_index,
		"tamper_hpub", 0,
		sig, sig_len, &sc_data, bad_hpub, (size_t)1 << (logn - 4),
		q00, q01, q10, q11, 1, attempts, pnorm_check, z0, z1,
		&coset_diag, &timing))
	{
		status_ok = 0;
	}

	size_t n = (size_t)1 << logn;
	memcpy(bad_q00, q00, n * sizeof *q00);
	memcpy(bad_q01, q01, n * sizeof *q01);
	memcpy(bad_q10, q10, n * sizeof *q10);
	memcpy(bad_q11, q11, n * sizeof *q11);
	memset(bad_q00, 0, n * sizeof *bad_q00);
	if (!write_verify_case_row(sig_fp, param, key_index, trial_index,
		"tamper_public_form", 0,
		sig, sig_len, &sc_data, hpub, (size_t)1 << (logn - 4),
		bad_q00, bad_q01, bad_q10, bad_q11,
		1, attempts, pnorm_check, z0, z1, &coset_diag, &timing))
	{
		status_ok = 0;
	}

	return status_ok ? 1 : 0;
}

static unsigned
get_max_attempts(const hist_param *param)
{
	const char *env = getenv("E8_HIST_MAX_ATTEMPTS");

	if (env != NULL && env[0] != 0) {
		char *end = NULL;
		unsigned long x = strtoul(env, &end, 10);
		if (end != env && *end == 0 && x > 0 && x <= UINT_MAX) {
			return (unsigned)x;
		}
	}
	return param->max_attempts;
}

static int
collect(void)
{
	/*
	 * If a signing trial fails, an accepted=0 row is still written and
	 * collection continues.  The final process status is nonzero for any
	 * signing failure or accepted-row norm mismatch.
	 */
	const char *pub_name = "e8_hist_public.csv";
	const char *sig_name = "e8_hist_signatures.csv";
	FILE *pub_fp;
	FILE *sig_fp;
	int status = 0;
	unsigned key_count = E8_HIST_DEFAULT_KEYS;
	unsigned sig_count = E8_HIST_DEFAULT_TRIALS;

	if (!parse_count_override("E8_HIST_KEYS", &key_count)
		|| !parse_count_override("E8_HIST_TRIALS", &sig_count))
	{
		return 0;
	}

	pub_fp = fopen(pub_name, "w");
	sig_fp = fopen(sig_name, "w");
	if (pub_fp == NULL || sig_fp == NULL) {
		fprintf(stderr, "ERR: could not open E8 histogram CSV outputs\n");
		if (pub_fp != NULL) {
			fclose(pub_fp);
		}
		if (sig_fp != NULL) {
			fclose(sig_fp);
		}
		return 0;
	}

	write_public_header(pub_fp);
	write_signature_header(sig_fp);

	for (size_t pi = 0; pi < sizeof PARAMS / sizeof PARAMS[0]; pi ++) {
		const hist_param *param = &PARAMS[pi];
		unsigned logn = param->logn;
		size_t hpub_len = (size_t)1 << (logn - 4);
		unsigned max_attempts = get_max_attempts(param);
		uint64_t rng_state = UINT64_C(0xE8A11B0000000000) + logn;
		int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
		int32_t q00[MAXN], q01[MAXN], q10[MAXN], q11[MAXN];
		uint8_t hpub[64];

		for (unsigned key = 0; key < key_count; key ++) {
			hist_timing key_timing;
			uint64_t key_c0, key_c1, key_w0, key_w1;
			uint64_t public_c0, public_c1, public_w0, public_w1;
			int key_ok;

			memset(&key_timing, 0, sizeof key_timing);
			key_c0 = hist_cycles_start();
			key_w0 = hist_wall_ns();
			key_ok = make_basis(logn, f, g, F, G, &rng_state);
			key_w1 = hist_wall_ns();
			key_c1 = hist_cycles_end();
			key_timing.cycles_keygen =
				hist_cycles_delta(key_c0, key_c1);
			key_timing.wall_ns_keygen =
				hist_wall_delta(key_w0, key_w1);
			if (!key_ok) {
				fprintf(stderr,
					"ERR: Hawk_keygen failed"
					" logn=%u key=%u\n", logn, key);
				status = 1;
				continue;
			}
			make_hpub(hpub, hpub_len, logn, key);
			public_c0 = hist_cycles_start();
			public_w0 = hist_wall_ns();
			e8_compute_qform(q00, q01, q10, q11,
				f, g, F, G, logn);
			public_w1 = hist_wall_ns();
			public_c1 = hist_cycles_end();
			key_timing.cycles_public_form =
				hist_cycles_delta(public_c0, public_c1);
			key_timing.wall_ns_public_form =
				hist_wall_delta(public_w0, public_w1);
			write_public_row(pub_fp, logn, key, q00, q01, q11);

			for (unsigned trial = 0; trial < sig_count; trial ++) {
				int r = collect_signature_row(sig_fp,
					param, key, trial,
					f, g, F, G, q00, q01, q10, q11, hpub,
					&rng_state, max_attempts,
					&key_timing);
				if (r <= 0) {
					status = 1;
				}
			}
		}
	}

	fclose(pub_fp);
	fclose(sig_fp);
	printf("wrote %s\n", pub_name);
	printf("wrote %s\n", sig_name);
	if (status != 0) {
		fprintf(stderr,
			"ERR: E8 histogram collection saw signing failure"
			" or norm mismatch; CSV rows were still written\n");
	}
	return status == 0;
}

int
main(int argc, char **argv)
{
	if (argc != 1) {
		fprintf(stderr, "usage: %s\n", argv[0]);
		return 1;
	}
	return collect() ? 0 : 1;
}
