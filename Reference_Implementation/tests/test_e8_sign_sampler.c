#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hawk_e8_inner.h"

#define MAXN   1024
#define RANGE_DIAGNOSTIC_TRIALS   1000

typedef struct {
	unsigned logn;
	unsigned keygen_trials;
	unsigned sign_trials;
	double sigma_sign;
	double sigma_verify;
	unsigned max_attempts;
} e8_sign_param;

typedef struct {
	unsigned logn;
	uint8_t digest[32];
} fixed_vector_digest;

typedef struct {
	unsigned signatures;
	unsigned total_attempts;
	unsigned max_seen_attempts;
	uint64_t max_abs_z0;
	uint64_t max_abs_z1;
	uint64_t max_abs_w0;
	uint64_t max_abs_w1;
} range_diagnostic_stats;

/*
 * Prototype-only values for the experimental Construction-A CM sampler.  They
 * are chosen to make this uncompressed algebraic integration test reliable;
 * they are not final parameter calibration claims.
 */
static const e8_sign_param PARAMS[] = {
	{ 8,  3, 3, 3.592, 2.03, 1000 },
	{ 9,  2, 2, 3.631, 1.99, 1000 },
	{ 10, 2, 2, 3.669, 1.95, 1000 }
};

static const fixed_vector_digest FIXED_VECTOR_DIGESTS[] = {
	{ 8, {
		0x3C, 0x8E, 0xD6, 0x8E, 0xD4, 0x78, 0x3E, 0xD8,
		0x7C, 0xE5, 0xC8, 0x98, 0x0B, 0x95, 0xED, 0xD8,
		0x1D, 0xC2, 0xFE, 0x52, 0x3A, 0xCC, 0x92, 0x09,
		0x86, 0x83, 0x92, 0xD0, 0xDC, 0x8E, 0xBB, 0x6A
	} },
	{ 9, {
		0x48, 0x98, 0x86, 0x54, 0x57, 0x94, 0x1A, 0xF0,
		0x42, 0x39, 0xF6, 0x2B, 0xA0, 0x8F, 0xE9, 0xB8,
		0x11, 0x92, 0xFD, 0x53, 0xD4, 0x32, 0xAD, 0xCA,
		0x3E, 0x81, 0x68, 0x46, 0xA4, 0x8B, 0xE1, 0x07
	} },
	{ 10, {
		0x85, 0xE9, 0x3B, 0xE4, 0x47, 0xDB, 0x1C, 0x65,
		0x30, 0xC4, 0xF8, 0xBF, 0xB5, 0x64, 0x4F, 0x5B,
		0x8C, 0x56, 0xE1, 0x7F, 0xD8, 0x2D, 0x59, 0x52,
		0xB3, 0x59, 0xFA, 0xBE, 0x1A, 0xF8, 0x12, 0x24
	} }
};

static uint64_t
rng_next_u64(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

static uint64_t
abs_i32_u64(int32_t x)
{
	return x < 0 ? (uint64_t)(-(int64_t)x) : (uint64_t)x;
}

static uint64_t
abs_i64_u64(int64_t x)
{
	return x < 0 ? (uint64_t)(-(x + 1)) + 1u : (uint64_t)x;
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

static unsigned
get_bit(const uint8_t *buf, size_t u)
{
	return (buf[u >> 3] >> (u & 7)) & 1u;
}

static void
make_message_context(shake_context *sc_data,
	unsigned logn, unsigned keynum, unsigned trial, unsigned variant)
{
	static const char prefix[] = "experimental e8 sampled signer test";
	uint8_t buf[4];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)keynum;
	buf[2] = (uint8_t)trial;
	buf[3] = (uint8_t)variant;
	shake_init(sc_data, 256);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

static void
make_range_message_context(shake_context *sc_data,
	unsigned logn, unsigned trial)
{
	static const char prefix[] = "experimental e8 range diagnostic";
	uint8_t buf[5];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)trial;
	buf[2] = (uint8_t)(trial >> 8);
	buf[3] = (uint8_t)(trial >> 16);
	buf[4] = (uint8_t)(trial >> 24);
	shake_init(sc_data, 256);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

static void
hash_to_h(unsigned logn, uint8_t *h, const shake_context *sc_data,
	const void *hpub, const uint8_t *salt, size_t salt_len)
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
bits_to_poly(int32_t *d, uint8_t *db,
	const uint8_t *h, size_t bit_off, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		uint8_t b = (uint8_t)get_bit(h, bit_off + u);
		db[u] = b;
		d[u] = b;
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
compute_t_mod2(uint8_t *t0, uint8_t *t1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const uint8_t *h0, const uint8_t *h1, size_t n)
{
	memset(t0, 0, n);
	memset(t1, 0, n);
	poly_mul_mod2_add(t0, f, h0, n);
	poly_mul_mod2_add(t0, F, h1, n);
	poly_mul_mod2_add(t1, g, h0, n);
	poly_mul_mod2_add(t1, G, h1, n);
}

static void
poly_mul_i8_i32_add_i64(int64_t *d,
	const int8_t *a, const int32_t *b, size_t n, int sign)
{
	for (size_t v = 0; v < n; v ++) {
		if (b[v] == 0) {
			continue;
		}
		for (size_t u = 0; u < n; u ++) {
			if (a[u] == 0) {
				continue;
			}
			size_t w = u + v;
			int64_t x = (int64_t)a[u] * b[v];
			if (w >= n) {
				w -= n;
				x = -x;
			}
			d[w] += sign < 0 ? -x : x;
		}
	}
}

static void
compute_inverse_w(int64_t *w0, int64_t *w1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *z0, const int32_t *z1, size_t n)
{
	memset(w0, 0, n * sizeof *w0);
	memset(w1, 0, n * sizeof *w1);
	poly_mul_i8_i32_add_i64(w0, G, z0, n, 1);
	poly_mul_i8_i32_add_i64(w0, F, z1, n, -1);
	poly_mul_i8_i32_add_i64(w1, g, z0, n, -1);
	poly_mul_i8_i32_add_i64(w1, f, z1, n, 1);
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
make_hpub(uint8_t *hpub, size_t hpub_len, unsigned logn, unsigned keynum)
{
	for (size_t u = 0; u < hpub_len; u ++) {
		hpub[u] = (uint8_t)(0x63u + 17u * u
			+ 11u * keynum + 5u * logn);
	}
}

static void
make_salt(uint8_t *salt, size_t salt_len,
	unsigned logn, unsigned keynum, unsigned trial)
{
	for (size_t u = 0; u < salt_len; u ++) {
		salt[u] = (uint8_t)(0xB5u + 29u * u
			+ 7u * keynum + trial + 3u * logn);
	}
}

static void
hash_signature(uint8_t digest[32], const uint8_t *sig, size_t sig_len)
{
	shake_context sc;

	shake_init(&sc, 256);
	shake_inject(&sc, sig, sig_len);
	shake_flip(&sc);
	shake_extract(&sc, digest, 32);
}

static void
print_digest(const uint8_t digest[32])
{
	for (unsigned u = 0; u < 32; u ++) {
		fprintf(stderr, "%s0x%02X", u == 0 ? "" : ", ", digest[u]);
	}
	fprintf(stderr, "\n");
}

static void
make_range_salt(uint8_t *salt, size_t salt_len,
	unsigned logn, unsigned trial)
{
	uint64_t rng_state = UINT64_C(0xE856A11E9A110000)
		+ ((uint64_t)logn << 40)
		+ (uint64_t)trial * UINT64_C(0x9E3779B97F4A7C15);

	for (size_t u = 0; u < salt_len; u ++) {
		if ((u & 7u) == 0) {
			(void)rng_next_u64(&rng_state);
		}
		salt[u] = (uint8_t)(rng_state >> ((u & 7u) << 3));
	}
}

static int
make_basis(unsigned logn, int8_t *f, int8_t *g, int8_t *F, int8_t *G,
	uint64_t *rng_state)
{
	uint8_t tmp[HAWK_TMPSIZE_KEYGEN(10)];
	uint8_t seed[40];

	return Hawk_keygen(logn, f, g, F, G, NULL, NULL, NULL, seed,
		test_rng, rng_state, tmp, sizeof tmp) == 0;
}

static int
check_sampler_trace(const e8_sign_param *param,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const uint8_t *hpub, const uint8_t *salt,
	const uint8_t *sig, size_t sig_len,
	const int32_t *z0, const int32_t *z1, int64_t pnorm,
	unsigned keynum, unsigned trial)
{
	unsigned logn = param->logn;
	size_t n = (size_t)1 << logn;
	size_t salt_len = e8_salt_len(logn);
	uint8_t salt2[40], h[256], h0b[MAXN], h1b[MAXN];
	uint8_t t0[MAXN], t1[MAXN];
	uint8_t neg_sig[40 + 4 * MAXN];
	int32_t w0_32[MAXN], w1_32[MAXN];
	int16_t s0[MAXN], s1[MAXN], ns0[MAXN], ns1[MAXN];
	int64_t w0[MAXN], w1[MAXN], qnorm, cqnorm, check_pnorm;
	int nonzero_w = 0;
	shake_context sc_data;

	if (!e8_decode_sig_uncompressed(logn, salt2, s0, s1, sig, sig_len)) {
		fprintf(stderr, "ERR: sampled E8 signature decode failed\n");
		return 0;
	}
	if (memcmp(salt, salt2, salt_len) != 0) {
		fprintf(stderr, "ERR: sampled E8 salt mismatch\n");
		return 0;
	}

	make_message_context(&sc_data, logn, keynum, trial, 0);
	hash_to_h(logn, h, &sc_data, hpub, salt, salt_len);
	bits_to_poly(w0_32, h0b, h, 0, n);
	bits_to_poly(w1_32, h1b, h, n, n);
	compute_t_mod2(t0, t1, f, g, F, G, h0b, h1b, n);
	for (size_t u = 0; u < n; u ++) {
		if ((((uint32_t)z0[u]) & 1u) != t0[u]
			|| (((uint32_t)z1[u]) & 1u) != t1[u])
		{
			fprintf(stderr,
				"ERR: sampled E8 z parity mismatch"
				" logn=%u key=%u trial=%u coeff=%u\n",
				logn, keynum, trial, (unsigned)u);
			return 0;
		}
	}

	compute_inverse_w(w0, w1, f, g, F, G, z0, z1, n);
	for (size_t u = 0; u < n; u ++) {
		if (w0[u] == INT64_MIN || w1[u] == INT64_MIN) {
			fprintf(stderr,
				"ERR: sampled E8 w cannot be negated"
				" logn=%u key=%u trial=%u coeff=%u\n",
				logn, keynum, trial, (unsigned)u);
			return 0;
		}
		if ((((uint64_t)w0[u]) & 1u) != h0b[u]
			|| (((uint64_t)w1[u]) & 1u) != h1b[u]
			|| (((uint64_t)(-w0[u])) & 1u) != h0b[u]
			|| (((uint64_t)(-w1[u])) & 1u) != h1b[u])
		{
			fprintf(stderr,
				"ERR: sampled E8 w/-w parity mismatch"
				" logn=%u key=%u trial=%u coeff=%u\n",
				logn, keynum, trial, (unsigned)u);
			return 0;
		}
		if (w0[u] < INT32_MIN || w0[u] > INT32_MAX
			|| w1[u] < INT32_MIN || w1[u] > INT32_MAX)
		{
			fprintf(stderr,
				"ERR: sampled E8 w out of int32 range"
				" logn=%u key=%u trial=%u coeff=%u\n",
				logn, keynum, trial, (unsigned)u);
			return 0;
		}
		w0_32[u] = (int32_t)w0[u];
		w1_32[u] = (int32_t)w1[u];
	}
	if (!e8_sym_break(w0_32, w1_32, logn)) {
		for (size_t u = 0; u < n; u ++) {
			if (w0_32[u] == INT32_MIN || w1_32[u] == INT32_MIN) {
				fprintf(stderr,
					"ERR: sampled E8 int32 w cannot be"
					" negated logn=%u key=%u trial=%u"
					" coeff=%u\n",
					logn, keynum, trial, (unsigned)u);
				return 0;
			}
			w0[u] = -w0[u];
			w1[u] = -w1[u];
			w0_32[u] = -w0_32[u];
			w1_32[u] = -w1_32[u];
		}
	}
	if (!e8_sym_break(w0_32, w1_32, logn)) {
		fprintf(stderr,
			"ERR: sampled E8 reconstructed w is not sym-broken"
			" logn=%u key=%u trial=%u\n", logn, keynum, trial);
		return 0;
	}
	for (size_t u = 0; u < n; u ++) {
		int64_t x0 = (int64_t)h0b[u] - w0[u];
		int64_t x1 = (int64_t)h1b[u] - w1[u];

		nonzero_w |= w0[u] != 0 || w1[u] != 0;
		if ((((uint64_t)w0[u]) & 1u) != h0b[u]
			|| (((uint64_t)w1[u]) & 1u) != h1b[u]
			|| (((uint64_t)(-w0[u])) & 1u) != h0b[u]
			|| (((uint64_t)(-w1[u])) & 1u) != h1b[u]
			|| (x0 % 2) != 0 || (x1 % 2) != 0)
		{
			fprintf(stderr,
				"ERR: sampled E8 canonical w integrality mismatch"
				" logn=%u key=%u trial=%u coeff=%u\n",
				logn, keynum, trial, (unsigned)u);
			return 0;
		}
		x0 /= 2;
		x1 /= 2;
		if (x0 < INT16_MIN || x0 > INT16_MAX
			|| x1 < INT16_MIN || x1 > INT16_MAX
			|| s0[u] != (int16_t)x0 || s1[u] != (int16_t)x1)
		{
			fprintf(stderr,
				"ERR: sampled E8 decoded s mismatch"
				" logn=%u key=%u trial=%u coeff=%u\n",
				logn, keynum, trial, (unsigned)u);
			return 0;
		}
	}
	if (!nonzero_w) {
		fprintf(stderr,
			"ERR: sampled E8 signature had zero reconstructed w"
			" logn=%u key=%u trial=%u\n", logn, keynum, trial);
		return 0;
	}

	check_pnorm = pnorm_from_apply(logn, z0, z1);
	if (pnorm != check_pnorm) {
		fprintf(stderr,
			"ERR: sampled E8 returned pnorm mismatch"
			" logn=%u key=%u trial=%u\n", logn, keynum, trial);
		return 0;
	}
	if (!e8_qnorm_direct(&qnorm, q00, q01, q10, q11,
		w0_32, w1_32, logn))
	{
		fprintf(stderr,
			"ERR: sampled E8 qnorm failed"
			" logn=%u key=%u trial=%u\n", logn, keynum, trial);
		return 0;
	}
	if (qnorm != pnorm) {
		fprintf(stderr,
			"ERR: sampled E8 norm equality mismatch"
			" logn=%u key=%u trial=%u\n", logn, keynum, trial);
		return 0;
	}
	if (!e8_qnorm_completion(&cqnorm, q00, q01, w0_32, w1_32, logn)
		|| cqnorm != qnorm)
	{
		fprintf(stderr,
			"ERR: sampled E8 completion norm mismatch"
			" logn=%u key=%u trial=%u\n", logn, keynum, trial);
		return 0;
	}

	for (size_t u = 0; u < n; u ++) {
		int64_t x0 = (int64_t)h0b[u] - s0[u];
		int64_t x1 = (int64_t)h1b[u] - s1[u];

		if (x0 < INT16_MIN || x0 > INT16_MAX
			|| x1 < INT16_MIN || x1 > INT16_MAX)
		{
			fprintf(stderr,
				"ERR: sampled E8 negated representative"
				" is outside test codec range logn=%u"
				" key=%u trial=%u coeff=%u\n",
				logn, keynum, trial, (unsigned)u);
			return 0;
		}
		ns0[u] = (int16_t)x0;
		ns1[u] = (int16_t)x1;
	}
	if (!e8_encode_sig_uncompressed(logn,
		neg_sig, sig_len, salt2, ns0, ns1))
	{
		fprintf(stderr,
			"ERR: sampled E8 negated signature encode failed"
			" logn=%u key=%u trial=%u\n", logn, keynum, trial);
		return 0;
	}
	make_message_context(&sc_data, logn, keynum, trial, 0);
	if (e8_verify_uncompressed_with_sigma(logn,
		neg_sig, sig_len, &sc_data, hpub, (size_t)1 << (logn - 4),
		q00, q01, q10, q11, param->sigma_verify))
	{
		fprintf(stderr,
			"ERR: sampled E8 verifier accepted -w representative"
			" logn=%u key=%u trial=%u\n", logn, keynum, trial);
		return 0;
	}

	return 1;
}

static int
record_sample_range(const e8_sign_param *param,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G,
	const uint8_t *hpub, const uint8_t *salt,
	const shake_context *sc_data,
	const int32_t *z0, const int32_t *z1,
	range_diagnostic_stats *stats, unsigned trial)
{
	unsigned logn = param->logn;
	size_t n = (size_t)1 << logn;
	size_t salt_len = e8_salt_len(logn);
	uint8_t h[256], h0b[MAXN], h1b[MAXN];
	uint8_t t0[MAXN], t1[MAXN];
	int64_t w0[MAXN], w1[MAXN];

	hash_to_h(logn, h, sc_data, hpub, salt, salt_len);
	for (size_t u = 0; u < n; u ++) {
		h0b[u] = (uint8_t)get_bit(h, u);
		h1b[u] = (uint8_t)get_bit(h, n + u);
	}
	compute_t_mod2(t0, t1, f, g, F, G, h0b, h1b, n);
	for (size_t u = 0; u < n; u ++) {
		uint64_t az0 = abs_i32_u64(z0[u]);
		uint64_t az1 = abs_i32_u64(z1[u]);

		if ((((uint32_t)z0[u]) & 1u) != t0[u]
			|| (((uint32_t)z1[u]) & 1u) != t1[u])
		{
			fprintf(stderr,
				"ERR: range diagnostic z parity mismatch"
				" logn=%u trial=%u coeff=%u\n",
				logn, trial, (unsigned)u);
			return 0;
		}
		if (az0 > stats->max_abs_z0) {
			stats->max_abs_z0 = az0;
		}
		if (az1 > stats->max_abs_z1) {
			stats->max_abs_z1 = az1;
		}
	}

	compute_inverse_w(w0, w1, f, g, F, G, z0, z1, n);
	for (size_t u = 0; u < n; u ++) {
		uint64_t aw0 = abs_i64_u64(w0[u]);
		uint64_t aw1 = abs_i64_u64(w1[u]);

		if ((((uint64_t)w0[u]) & 1u) != h0b[u]
			|| (((uint64_t)w1[u]) & 1u) != h1b[u])
		{
			fprintf(stderr,
				"ERR: range diagnostic w parity mismatch"
				" logn=%u trial=%u coeff=%u\n",
				logn, trial, (unsigned)u);
			return 0;
		}
		if (aw0 > stats->max_abs_w0) {
			stats->max_abs_w0 = aw0;
		}
		if (aw1 > stats->max_abs_w1) {
			stats->max_abs_w1 = aw1;
		}
	}
	return 1;
}

static int
run_range_diagnostic_logn(const e8_sign_param *param)
{
	unsigned logn = param->logn;
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	size_t hpub_len = (size_t)1 << (logn - 4);
	uint64_t rng_state = UINT64_C(0xE856A11E4AD10000) + logn;
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	uint8_t sig[40 + 4 * MAXN];
	uint8_t hpub[64], salt[40];
	int32_t z0[MAXN], z1[MAXN];
	range_diagnostic_stats stats;

	memset(&stats, 0, sizeof stats);
	if (!make_basis(logn, f, g, F, G, &rng_state)) {
		fprintf(stderr,
			"ERR: range diagnostic keygen failed logn=%u\n",
			logn);
		return 0;
	}
	make_hpub(hpub, hpub_len, logn, 0);
	e8_sampler_set_thread_count(1);
	e8_sampler_set_rng_mode(E8_SAMPLER_RNG_PER_BLOCK);

	for (unsigned trial = 0; trial < RANGE_DIAGNOSTIC_TRIALS; trial ++) {
		shake_context sc_data;
		int64_t pnorm;
		unsigned attempts;

		make_range_salt(salt, salt_len, logn, trial);
		make_range_message_context(&sc_data, logn, trial);
		if (!e8_sign_sampler_trace_uncompressed(logn,
			sig, sig_len, &sc_data, hpub, hpub_len,
			f, g, F, G, salt,
			param->sigma_sign, param->sigma_verify,
			param->max_attempts, test_rng, &rng_state,
			z0, z1, &pnorm, &attempts))
		{
			fprintf(stderr,
				"ERR: range diagnostic signing failed"
				" logn=%u trial=%u\n", logn, trial);
			return 0;
		}
		if (!record_sample_range(param, f, g, F, G, hpub, salt,
			&sc_data, z0, z1, &stats, trial))
		{
			return 0;
		}
		stats.signatures ++;
		stats.total_attempts += attempts;
		if (attempts > stats.max_seen_attempts) {
			stats.max_seen_attempts = attempts;
		}
		(void)pnorm;
	}

	printf("E8 sampler range diagnostic n=%u: signatures=%u"
		" max_abs_z0=%llu max_abs_z1=%llu"
		" max_abs_w0=%llu max_abs_w1=%llu"
		" total_attempts=%u max_seen_attempts=%u\n",
		1u << logn, stats.signatures,
		(unsigned long long)stats.max_abs_z0,
		(unsigned long long)stats.max_abs_z1,
		(unsigned long long)stats.max_abs_w0,
		(unsigned long long)stats.max_abs_w1,
		stats.total_attempts, stats.max_seen_attempts);
	return 1;
}

static int
run_range_diagnostic(void)
{
	for (size_t u = 0; u < sizeof PARAMS / sizeof PARAMS[0]; u ++) {
		if (!run_range_diagnostic_logn(&PARAMS[u])) {
			return 0;
		}
	}
	return 1;
}

static const fixed_vector_digest *
find_fixed_vector_digest(unsigned logn)
{
	for (size_t u = 0;
		u < sizeof FIXED_VECTOR_DIGESTS / sizeof FIXED_VECTOR_DIGESTS[0];
		u ++)
	{
		if (FIXED_VECTOR_DIGESTS[u].logn == logn) {
			return &FIXED_VECTOR_DIGESTS[u];
		}
	}
	return NULL;
}

static int
test_fixed_seed_vector_logn(const e8_sign_param *param)
{
	unsigned logn = param->logn;
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	size_t hpub_len = (size_t)1 << (logn - 4);
	uint64_t key_rng = UINT64_C(0xE856F17ED51A0000) + logn;
	uint64_t sign_rng = UINT64_C(0xE856F17ED51A8000) + logn;
	const fixed_vector_digest *expected;
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	int32_t q00[MAXN], q01[MAXN], q10[MAXN], q11[MAXN];
	int32_t z0[MAXN], z1[MAXN];
	uint8_t sig[40 + 4 * MAXN], hpub[64], salt[40];
	uint8_t digest[32];
	shake_context sc_data;
	int64_t pnorm;
	unsigned attempts;

	expected = find_fixed_vector_digest(logn);
	if (expected == NULL) {
		fprintf(stderr,
			"ERR: no sampled E8 fixed vector digest logn=%u\n",
			logn);
		return 0;
	}
	if (!make_basis(logn, f, g, F, G, &key_rng)) {
		fprintf(stderr,
			"ERR: fixed-vector keygen failed logn=%u\n", logn);
		return 0;
	}
	e8_compute_qform(q00, q01, q10, q11, f, g, F, G, logn);
	make_hpub(hpub, hpub_len, logn, 0x5Au);
	make_salt(salt, salt_len, logn, 0x5Au, 0x33u);
	make_message_context(&sc_data, logn, 0x5Au, 0x33u, 0);

	e8_sampler_set_thread_count(1);
	e8_sampler_set_rng_mode(E8_SAMPLER_RNG_PER_BLOCK);
	if (!e8_sign_sampler_trace_uncompressed(logn,
		sig, sig_len, &sc_data, hpub, hpub_len,
		f, g, F, G, salt,
		param->sigma_sign, param->sigma_verify,
		param->max_attempts, test_rng, &sign_rng,
		z0, z1, &pnorm, &attempts))
	{
		fprintf(stderr,
			"ERR: fixed-vector signing failed logn=%u\n", logn);
		return 0;
	}

	make_message_context(&sc_data, logn, 0x5Au, 0x33u, 0);
	if (!e8_verify_uncompressed_with_sigma(logn,
		sig, sig_len, &sc_data, hpub, hpub_len,
		q00, q01, q10, q11, param->sigma_verify))
	{
		fprintf(stderr,
			"ERR: fixed-vector verifier rejected logn=%u\n", logn);
		return 0;
	}
	if (!check_sampler_trace(param, f, g, F, G,
		q00, q01, q10, q11, hpub, salt,
		sig, sig_len, z0, z1, pnorm, 0x5Au, 0x33u))
	{
		return 0;
	}

	hash_signature(digest, sig, sig_len);
	if (memcmp(digest, expected->digest, sizeof digest) != 0) {
		fprintf(stderr,
			"ERR: fixed-vector signature digest mismatch logn=%u\n"
			"got: ", logn);
		print_digest(digest);
		fprintf(stderr, "expected: ");
		print_digest(expected->digest);
		return 0;
	}
	printf("E8 sampler fixed vector n=%u: attempts=%u pnorm=%lld\n",
		1u << logn, attempts, (long long)pnorm);
	return 1;
}

static int
test_unsupported_logn(const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G)
{
	uint8_t sig[40 + 4 * MAXN], hpub[64], salt[40];
	shake_context sc_data;
	uint64_t rng_state = UINT64_C(0xE856AD09ABCDEF00);

	make_message_context(&sc_data, 7, 0, 0, 0);
	memset(hpub, 0xA9, sizeof hpub);
	memset(salt, 0x5A, sizeof salt);
	if (e8_sign_sampler_uncompressed(7,
		sig, 0, &sc_data, hpub, 16, f, g, F, G, salt,
		PARAMS[0].sigma_sign, PARAMS[0].sigma_verify,
		PARAMS[0].max_attempts, test_rng, &rng_state))
	{
		fprintf(stderr, "ERR: sampled E8 signer accepted logn=7\n");
		return 0;
	}
	return 1;
}

static int
test_sampler_signing_logn(const e8_sign_param *param)
{
	unsigned logn = param->logn;
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	size_t hpub_len = (size_t)1 << (logn - 4);
	uint64_t rng_state = UINT64_C(0xE856A11E00000000) + logn;
	unsigned sig_count = 0, total_attempts = 0, max_seen = 0;
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	int32_t q00[MAXN], q01[MAXN], q10[MAXN], q11[MAXN];
	uint8_t sig[40 + 4 * MAXN], bad[40 + 4 * MAXN];
	uint8_t hpub[64], salt[40];
	int32_t z0[MAXN], z1[MAXN];

	printf("E8 sampler signer n=%u:\n", 1u << logn);
	if (!make_basis(logn, f, g, F, G, &rng_state)) {
		fprintf(stderr, "ERR: initial Hawk_keygen failed logn=%u\n",
			logn);
		return 0;
	}
	if (logn == 8 && !test_unsupported_logn(f, g, F, G)) {
		return 0;
	}

	for (unsigned keynum = 0; keynum < param->keygen_trials; keynum ++) {
		if (!make_basis(logn, f, g, F, G, &rng_state)) {
			fprintf(stderr,
				"ERR: Hawk_keygen failed for sampled E8"
				" logn=%u key=%u\n", logn, keynum);
			return 0;
		}
		make_hpub(hpub, hpub_len, logn, keynum);
		e8_compute_qform(q00, q01, q10, q11, f, g, F, G, logn);

		for (unsigned trial = 0; trial < param->sign_trials; trial ++) {
			shake_context sc_data;
			int64_t pnorm;
			int64_t threshold;
			unsigned attempts;
			unsigned sampler_threads = (trial & 1u) == 0 ? 1 : 4;

			make_salt(salt, salt_len, logn, keynum, trial);
			make_message_context(&sc_data, logn, keynum, trial, 0);
			e8_sampler_set_thread_count(sampler_threads);
			if (!e8_sign_sampler_trace_uncompressed(logn,
				sig, sig_len, &sc_data, hpub, hpub_len,
				f, g, F, G, salt,
				param->sigma_sign, param->sigma_verify,
				param->max_attempts, test_rng, &rng_state,
				z0, z1, &pnorm, &attempts))
			{
				fprintf(stderr,
					"ERR: sampled E8 signing failed"
					" logn=%u key=%u trial=%u\n",
					logn, keynum, trial);
				return 0;
			}

			printf("  key=%u trial=%u threads=%u"
				" attempts=%u pnorm=%lld\n",
				keynum, trial, sampler_threads, attempts,
				(long long)pnorm);
			if (!e8_verify_bound_from_sigma(logn,
				param->sigma_verify, &threshold)
				|| pnorm > threshold)
			{
				fprintf(stderr,
					"ERR: sampled E8 signature exceeded"
					" verification bound logn=%u key=%u"
					" trial=%u\n", logn, keynum, trial);
				return 0;
			}
			sig_count ++;
			total_attempts += attempts;
			if (attempts > max_seen) {
				max_seen = attempts;
			}

			make_message_context(&sc_data, logn, keynum, trial, 0);
			if (!e8_verify_uncompressed_with_sigma(logn,
				sig, sig_len, &sc_data, hpub, hpub_len,
				q00, q01, q10, q11, param->sigma_verify))
			{
				fprintf(stderr,
					"ERR: sampled E8 signature rejected"
					" logn=%u key=%u trial=%u\n",
					logn, keynum, trial);
				return 0;
			}
			if (!check_sampler_trace(param, f, g, F, G,
				q00, q01, q10, q11, hpub, salt,
				sig, sig_len, z0, z1, pnorm, keynum, trial))
			{
				return 0;
			}

			make_message_context(&sc_data, logn, keynum, trial, 1);
			if (e8_verify_uncompressed_with_sigma(logn,
				sig, sig_len, &sc_data, hpub, hpub_len,
				q00, q01, q10, q11, param->sigma_verify))
			{
				fprintf(stderr,
					"ERR: sampled E8 tampered message accepted"
					" logn=%u key=%u trial=%u\n",
					logn, keynum, trial);
				return 0;
			}

			memcpy(bad, sig, sig_len);
			bad[salt_len + 0] = 0xFF;
			bad[salt_len + 1] = 0x7F;
			make_message_context(&sc_data, logn, keynum, trial, 0);
			if (e8_verify_uncompressed_with_sigma(logn,
				bad, sig_len, &sc_data, hpub, hpub_len,
				q00, q01, q10, q11, param->sigma_verify))
			{
				fprintf(stderr,
					"ERR: sampled E8 tampered signature accepted"
					" logn=%u key=%u trial=%u\n",
					logn, keynum, trial);
				return 0;
			}
		}
	}

	e8_sampler_set_thread_count(1);
	printf("E8 sampler signer n=%u summary: signatures=%u"
		" total_attempts=%u max_seen=%u sigma_sign=%.3f"
		" sigma_verify=%.3f\n",
		1u << logn, sig_count, total_attempts, max_seen,
		param->sigma_sign, param->sigma_verify);
	return 1;
}

int
main(int argc, char **argv)
{
	if (argc == 2 && strcmp(argv[1], "--range-diagnostic") == 0) {
		return run_range_diagnostic() ? 0 : 1;
	}
	if (argc != 1) {
		fprintf(stderr, "usage: %s [--range-diagnostic]\n", argv[0]);
		return 1;
	}
	for (size_t u = 0; u < sizeof PARAMS / sizeof PARAMS[0]; u ++) {
		if (!test_fixed_seed_vector_logn(&PARAMS[u])) {
			return 1;
		}
		if (!test_sampler_signing_logn(&PARAMS[u])) {
			return 1;
		}
	}
	return 0;
}
