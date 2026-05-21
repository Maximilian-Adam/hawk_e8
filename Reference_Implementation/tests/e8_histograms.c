#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hawk_e8_inner.h"

#define MAXN   1024
#define E8_HIST_MAX_SAMPLER_BOUND   16

/*
 * Experimental E8 calibration logger.
 *
 * This program records CSV data for the current floating-point
 * Construction-A CM E8 prototype.  It does not alter signer, sampler, or
 * verifier behavior, and its parameters are not final security claims.
 */

typedef struct {
	unsigned logn;
	unsigned quick_keys;
	unsigned quick_sigs;
	unsigned long_keys;
	unsigned long_sigs;
	double sigma_sign;
	double sigma_verify;
	int quick_bound;
	unsigned max_attempts;
} hist_param;

typedef struct {
	int32_t min;
	int32_t max;
	int64_t absmax;
	unsigned bits;
} coeff_stats;

/*
 * Experimental calibration defaults only.  These are for CSV logging with
 * the Construction-A CM sampler and are not final E8 parameter claims.
 */
static const hist_param PARAMS[] = {
	{ 8,  20, 20, 2, 2, 1.25, 1.06, 2, 1000 },
	{ 9,   5, 10, 1, 2, 1.28, 1.42, 2, 1000 },
	{ 10,  3,  5, 1, 1, 1.30, 1.57, 2, 1000 }
};

static const int LONG_BOUNDS[] = { 2, 4, 6, 8 };

static int
parse_bound_override(int *bound, int *is_set)
{
	const char *env = getenv("E8_HIST_BOUND");

	*is_set = 0;
	if (env == NULL) {
		return 1;
	}
	if (env[0] == 0) {
		fprintf(stderr,
			"ERR: E8_HIST_BOUND must be a positive integer"
			" in [1,%d]\n", E8_HIST_MAX_SAMPLER_BOUND);
		return 0;
	}
	char *end = NULL;
	unsigned long x = strtoul(env, &end, 10);
	if (end == env || *end != 0 || x == 0
		|| x > E8_HIST_MAX_SAMPLER_BOUND)
	{
		fprintf(stderr,
			"ERR: E8_HIST_BOUND must be a positive integer"
			" in [1,%d], got '%s'\n",
			E8_HIST_MAX_SAMPLER_BOUND, env);
		return 0;
	}
	*bound = (int)x;
	*is_set = 1;
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
	unsigned logn, unsigned keynum, unsigned trial, unsigned bound)
{
	static const char prefix[] = "experimental e8 histogram";
	uint8_t buf[4];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)keynum;
	buf[2] = (uint8_t)trial;
	buf[3] = (uint8_t)bound;
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
	unsigned logn, unsigned keynum, unsigned trial, unsigned bound)
{
	for (size_t u = 0; u < salt_len; u ++) {
		salt[u] = (uint8_t)(0x79u + 31u * u
			+ 7u * logn + 3u * keynum + trial + bound);
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
		"rejected_attempts,sigma_sign,sigma_verify,sampler_bound,"
		"pnorm,qnorm,norm_equal,"
		"s0_min,s0_max,s0_absmax,s0_absmax_bits,"
		"s1_min,s1_max,s1_absmax,s1_absmax_bits,"
		"z0_min,z0_max,z0_absmax,z0_absmax_bits,"
		"z1_min,z1_max,z1_absmax,z1_absmax_bits\n");
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
	double sigma_sign, double sigma_verify, int sampler_bound,
	int64_t pnorm, int64_t qnorm, int norm_equal,
	const int16_t *s0, const int16_t *s1,
	const int32_t *z0, const int32_t *z1)
{
	size_t n = (size_t)1 << logn;
	unsigned rejected_attempts = accepted ? attempts - 1 : attempts;
	coeff_stats ss0, ss1, sz0, sz1;
	static const int16_t zero16[MAXN];
	static const int32_t zero32[MAXN];

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
	ss0 = stats_i16(s0, n);
	ss1 = stats_i16(s1, n);
	sz0 = stats_i32(z0, n);
	sz1 = stats_i32(z1, n);

	fprintf(fp, "%u,%u,%u,%u,%d,%u,%u,%.17g,%.17g,%d,"
		"%lld,%lld,%d,"
		"%d,%d,%lld,%u,"
		"%d,%d,%lld,%u,"
		"%d,%d,%lld,%u,"
		"%d,%d,%lld,%u\n",
		logn, (unsigned)n, key_index, trial_index, accepted,
		attempts, rejected_attempts, sigma_sign, sigma_verify,
		sampler_bound, (long long)pnorm, (long long)qnorm,
		norm_equal,
		ss0.min, ss0.max, (long long)ss0.absmax, ss0.bits,
		ss1.min, ss1.max, (long long)ss1.absmax, ss1.bits,
		sz0.min, sz0.max, (long long)sz0.absmax, sz0.bits,
		sz1.min, sz1.max, (long long)sz1.absmax, sz1.bits);
}

static int
collect_signature_row(FILE *sig_fp, const hist_param *param,
	unsigned key_index, unsigned trial_index, int sampler_bound,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const uint8_t *hpub, uint64_t *rng_state, unsigned max_attempts)
{
	unsigned logn = param->logn;
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	uint8_t sig[40 + 4 * MAXN], salt[40];
	int16_t s0[MAXN], s1[MAXN];
	int32_t z0[MAXN], z1[MAXN], w0[MAXN], w1[MAXN];
	int64_t pnorm = 0, pnorm_check = 0, qnorm = 0, direct_qnorm = 0;
	unsigned attempts = max_attempts;
	int accepted, norm_equal = 1;
	shake_context sc_data;

	make_salt(salt, salt_len, logn, key_index, trial_index,
		(unsigned)sampler_bound);
	make_message_context(&sc_data, logn, key_index, trial_index,
		(unsigned)sampler_bound);
	accepted = e8_sign_sampler_trace_uncompressed(logn,
		sig, sig_len, &sc_data, hpub, (size_t)1 << (logn - 4),
		f, g, F, G, salt, param->sigma_sign, param->sigma_verify,
		sampler_bound, max_attempts, hist_rng, rng_state,
		z0, z1, &pnorm, &attempts);
	if (!accepted) {
		write_signature_row(sig_fp, logn, key_index, trial_index,
			0, max_attempts, param->sigma_sign,
			param->sigma_verify, sampler_bound,
			0, 0, 1, NULL, NULL, NULL, NULL);
		return -1;
	}

	if (!e8_decode_sig_uncompressed(logn, NULL, s0, s1, sig, sig_len)) {
		write_signature_row(sig_fp, logn, key_index, trial_index,
			0, attempts, param->sigma_sign,
			param->sigma_verify, sampler_bound,
			0, 0, 1, NULL, NULL, NULL, NULL);
		return -1;
	}
	make_message_context(&sc_data, logn, key_index, trial_index,
		(unsigned)sampler_bound);
	reconstruct_w(w0, w1, logn, hpub, salt, salt_len, &sc_data, s0, s1);
	pnorm_check = pnorm_from_apply(logn, z0, z1);
	if (!e8_qnorm_completion(&qnorm, q00, q01, w0, w1, logn)
		|| !e8_qnorm_direct(&direct_qnorm,
			q00, q01, q10, q11, w0, w1, logn))
	{
		norm_equal = 0;
	} else if (pnorm != pnorm_check || pnorm_check != qnorm
		|| qnorm != direct_qnorm)
	{
		norm_equal = 0;
	}

	write_signature_row(sig_fp, logn, key_index, trial_index,
		1, attempts, param->sigma_sign, param->sigma_verify,
		sampler_bound, pnorm_check, qnorm, norm_equal,
		s0, s1, z0, z1);
	return norm_equal ? 1 : 0;
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
collect(int long_mode)
{
	/*
	 * If a signing trial fails, an accepted=0 row is still written and
	 * collection continues.  The final process status is nonzero for any
	 * signing failure or accepted-row norm mismatch.
	 */
	const char *pub_name = long_mode
		? "e8_hist_public_long.csv" : "e8_hist_public.csv";
	const char *sig_name = long_mode
		? "e8_hist_signatures_long.csv"
		: "e8_hist_signatures.csv";
	FILE *pub_fp;
	FILE *sig_fp;
	int status = 0;
	int bound_override = 0;
	int has_bound_override = 0;

	if (!parse_bound_override(&bound_override, &has_bound_override)) {
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
		unsigned key_count = long_mode
			? param->long_keys : param->quick_keys;
		unsigned sig_count = long_mode
			? param->long_sigs : param->quick_sigs;
		unsigned max_attempts = get_max_attempts(param);
		uint64_t rng_state = UINT64_C(0xE8A11B0000000000) + logn
			+ (long_mode ? UINT64_C(0x100000000) : 0);
		int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
		int32_t q00[MAXN], q01[MAXN], q10[MAXN], q11[MAXN];
		uint8_t hpub[64];

		for (unsigned key = 0; key < key_count; key ++) {
			if (!make_basis(logn, f, g, F, G, &rng_state)) {
				fprintf(stderr,
					"ERR: Hawk_keygen failed"
					" logn=%u key=%u\n", logn, key);
				status = 1;
				continue;
			}
			make_hpub(hpub, hpub_len, logn, key);
			e8_compute_qform(q00, q01, q10, q11,
				f, g, F, G, logn);
			write_public_row(pub_fp, logn, key, q00, q01, q11);

			if (long_mode) {
				size_t bound_count = has_bound_override ? 1
					: sizeof LONG_BOUNDS / sizeof LONG_BOUNDS[0];
				for (size_t bi = 0; bi < bound_count; bi ++) {
					int bound = has_bound_override
						? bound_override : LONG_BOUNDS[bi];
					for (unsigned trial = 0;
						trial < sig_count; trial ++)
					{
						int r = collect_signature_row(
							sig_fp, param, key, trial,
							bound,
							f, g, F, G,
							q00, q01, q10, q11,
							hpub, &rng_state,
							max_attempts);
						if (r <= 0) {
							status = 1;
						}
					}
				}
			} else {
				int bound = has_bound_override
					? bound_override : param->quick_bound;
				for (unsigned trial = 0; trial < sig_count; trial ++) {
					int r = collect_signature_row(sig_fp,
						param, key, trial,
						bound, f, g, F, G,
						q00, q01, q10, q11, hpub,
						&rng_state, max_attempts);
					if (r <= 0) {
						status = 1;
					}
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
	int long_mode = argc > 1 && strcmp(argv[1], "long") == 0;

	if (argc > 2 || (argc == 2 && !long_mode)) {
		fprintf(stderr, "usage: %s [long]\n", argv[0]);
		return 1;
	}
	return collect(long_mode) ? 0 : 1;
}
