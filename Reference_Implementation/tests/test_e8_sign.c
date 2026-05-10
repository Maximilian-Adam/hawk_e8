#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hawk_e8_inner.h"

#define MAXN   1024
#define KEYGEN_TRIALS   3
#define SIGN_TRIALS   4
#define OFFSET_CASES   3

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
make_message_context(shake_context *sc_data, unsigned logn, unsigned trial)
{
	static const char prefix[] = "experimental e8 dummy signer test";
	uint8_t buf[2];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)trial;
	shake_init(sc_data, 256);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

static unsigned
get_bit(const uint8_t *buf, size_t u)
{
	return (buf[u >> 3] >> (u & 7)) & 1u;
}

static void
hash_to_h(unsigned logn, uint8_t *h,
	const shake_context *sc_data, const void *hpub,
	const uint8_t *salt, size_t salt_len)
{
	uint8_t hm[64];
	size_t hpub_len = (size_t)1 << (logn - 4);
	shake_context scd;

	scd = *sc_data;
	shake_inject(&scd, hpub, hpub_len);
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

static int
poly_mul_i8_i32_i32(int32_t *d,
	const int8_t *a, const int32_t *b, size_t n)
{
	int64_t acc[MAXN];

	memset(acc, 0, n * sizeof *acc);
	poly_mul_i8_i32_add_i64(acc, a, b, n, 1);
	for (size_t u = 0; u < n; u ++) {
		if (acc[u] < INT32_MIN || acc[u] > INT32_MAX) {
			return 0;
		}
		d[u] = (int32_t)acc[u];
	}
	return 1;
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

static int
compute_z_from_h_and_r(int32_t *z0, int32_t *z1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *h0, const int32_t *h1,
	const int32_t *r0, const int32_t *r1, size_t n)
{
	int32_t tmp0[MAXN], tmp1[MAXN];

	if (!poly_mul_i8_i32_i32(tmp0, f, h0, n)
		|| !poly_mul_i8_i32_i32(tmp1, F, h1, n))
	{
		return 0;
	}
	for (size_t u = 0; u < n; u ++) {
		int64_t x = (int64_t)tmp0[u] + tmp1[u];
		if (r0 != NULL) {
			x += 2 * (int64_t)r0[u];
		}
		if (x < INT32_MIN || x > INT32_MAX) {
			return 0;
		}
		z0[u] = (int32_t)x;
	}

	if (!poly_mul_i8_i32_i32(tmp0, g, h0, n)
		|| !poly_mul_i8_i32_i32(tmp1, G, h1, n))
	{
		return 0;
	}
	for (size_t u = 0; u < n; u ++) {
		int64_t x = (int64_t)tmp0[u] + tmp1[u];
		if (r1 != NULL) {
			x += 2 * (int64_t)r1[u];
		}
		if (x < INT32_MIN || x > INT32_MAX) {
			return 0;
		}
		z1[u] = (int32_t)x;
	}

	return 1;
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

static void
make_offset_case(unsigned logn, unsigned caseid, int32_t *r0, int32_t *r1)
{
	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;

	memset(r0, 0, n * sizeof *r0);
	memset(r1, 0, n * sizeof *r1);
	switch (caseid) {
	case 0:
		r0[0] = 1;
		r1[1] = -1;
		break;
	case 1:
		r0[k] = 1;
		r0[n - 1] = -1;
		r1[2] = 1;
		break;
	default:
		r0[3] = -1;
		r1[k + 1] = 1;
		r1[3 * k] = 1;
		break;
	}
}

static void
make_hpub(uint8_t *hpub, size_t hpub_len, unsigned logn, unsigned keynum)
{
	for (size_t u = 0; u < hpub_len; u ++) {
		hpub[u] = (uint8_t)(0x51u + 19u * u + 7u * logn + keynum);
	}
}

static void
make_salt(uint8_t *salt, size_t salt_len, unsigned logn,
	unsigned keynum, unsigned trial)
{
	for (size_t u = 0; u < salt_len; u ++) {
		salt[u] = (uint8_t)(0xA7u + 23u * u
			+ 11u * logn + 5u * keynum + trial);
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

static void
make_identity_basis(size_t n, int8_t *f, int8_t *g, int8_t *F, int8_t *G)
{
	memset(f, 0, n);
	memset(g, 0, n);
	memset(F, 0, n);
	memset(G, 0, n);
	f[0] = 1;
	G[0] = 1;
}

static int
check_signature_zero_s(unsigned logn, const uint8_t *sig, size_t sig_len)
{
	size_t n = (size_t)1 << logn;
	uint8_t salt[40];
	int16_t s0[MAXN], s1[MAXN];

	if (!e8_decode_sig_uncompressed(logn, salt, s0, s1, sig, sig_len)) {
		fprintf(stderr, "ERR: dummy E8 signature failed to decode\n");
		return 0;
	}
	for (size_t u = 0; u < n; u ++) {
		if (s0[u] != 0 || s1[u] != 0) {
			fprintf(stderr,
				"ERR: dummy E8 signature produced non-zero s"
				" at logn=%u coeff=%u\n",
				logn, (unsigned)u);
			return 0;
		}
	}
	return 1;
}

static int
check_nonzero_offset_signature(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const uint8_t *hpub, size_t hpub_len,
	const uint8_t *salt, const uint8_t *sig, size_t sig_len,
	const int32_t *r0, const int32_t *r1, unsigned caseid)
{
	size_t n = (size_t)1 << logn;
	size_t salt_len = e8_salt_len(logn);
	size_t expected_hpub_len = (size_t)1 << (logn - 4);
	uint8_t salt2[40], h[256], h0b[MAXN], h1b[MAXN];
	uint8_t t0[MAXN], t1[MAXN];
	int32_t h0[MAXN], h1[MAXN], z0[MAXN], z1[MAXN];
	int32_t w0_32[MAXN], w1_32[MAXN], pz0[MAXN], pz1[MAXN];
	int64_t w0[MAXN], w1[MAXN];
	int16_t s0[MAXN], s1[MAXN];
	int64_t pnorm = 0, qnorm;
	int seen_nonzero = 0;
	shake_context sc_data;

	if (!e8_decode_sig_uncompressed(logn, salt2, s0, s1, sig, sig_len)) {
		fprintf(stderr, "ERR: nonzero dummy E8 signature decode failed\n");
		return 0;
	}
	if (memcmp(salt, salt2, salt_len) != 0) {
		fprintf(stderr, "ERR: nonzero dummy E8 salt mismatch\n");
		return 0;
	}
	if (hpub_len != expected_hpub_len) {
		fprintf(stderr,
			"ERR: unexpected hpub_len logn=%u got=%u expected=%u\n",
			logn, (unsigned)hpub_len,
			(unsigned)expected_hpub_len);
		return 0;
	}

	make_message_context(&sc_data, logn, SIGN_TRIALS + caseid);
	hash_to_h(logn, h, &sc_data, hpub, salt, salt_len);
	bits_to_poly(h0, h0b, h, 0, n);
	bits_to_poly(h1, h1b, h, n, n);
	compute_t_mod2(t0, t1, f, g, F, G, h0b, h1b, n);
	if (!compute_z_from_h_and_r(z0, z1, f, g, F, G,
		h0, h1, r0, r1, n))
	{
		fprintf(stderr, "ERR: could not recompute nonzero dummy z\n");
		return 0;
	}
	for (size_t u = 0; u < n; u ++) {
		if ((((uint32_t)z0[u]) & 1u) != t0[u]
			|| (((uint32_t)z1[u]) & 1u) != t1[u])
		{
			fprintf(stderr,
				"ERR: nonzero dummy z parity mismatch"
				" logn=%u case=%u coeff=%u\n",
				logn, caseid, (unsigned)u);
			return 0;
		}
	}

	compute_inverse_w(w0, w1, f, g, F, G, z0, z1, n);
	for (size_t u = 0; u < n; u ++) {
		int64_t x0 = (int64_t)h0b[u] - w0[u];
		int64_t x1 = (int64_t)h1b[u] - w1[u];

		if ((((uint64_t)w0[u]) & 1u) != h0b[u]
			|| (((uint64_t)w1[u]) & 1u) != h1b[u]
			|| (x0 % 2) != 0 || (x1 % 2) != 0)
		{
			fprintf(stderr,
				"ERR: nonzero dummy w parity/integrality mismatch"
				" logn=%u case=%u coeff=%u\n",
				logn, caseid, (unsigned)u);
			return 0;
		}
		x0 /= 2;
		x1 /= 2;
		if (x0 < INT16_MIN || x0 > INT16_MAX
			|| x1 < INT16_MIN || x1 > INT16_MAX
			|| s0[u] != (int16_t)x0 || s1[u] != (int16_t)x1)
		{
			fprintf(stderr,
				"ERR: decoded nonzero dummy s mismatch"
				" logn=%u case=%u coeff=%u\n",
				logn, caseid, (unsigned)u);
			return 0;
		}
		if (s0[u] != 0 || s1[u] != 0) {
			seen_nonzero = 1;
		}

		w0_32[u] = (int32_t)h0b[u] - 2 * (int32_t)s0[u];
		w1_32[u] = (int32_t)h1b[u] - 2 * (int32_t)s1[u];
	}
	if (!seen_nonzero) {
		fprintf(stderr,
			"ERR: nonzero dummy signature had identically zero s"
			" logn=%u case=%u\n", logn, caseid);
		return 0;
	}

	e8_apply_P(pz0, pz1, z0, z1, logn);
	for (size_t u = 0; u < n; u ++) {
		pnorm += (int64_t)pz0[u] * pz0[u]
			+ (int64_t)pz1[u] * pz1[u];
	}
	if (!e8_qnorm_direct(&qnorm, q00, q01, q10, q11,
		w0_32, w1_32, logn))
	{
		fprintf(stderr, "ERR: nonzero dummy Q_E8 norm failed\n");
		return 0;
	}
	if (pnorm != qnorm) {
		fprintf(stderr,
			"ERR: nonzero dummy ||Pz||^2/Q_E8 norm mismatch"
			" logn=%u case=%u\n", logn, caseid);
		return 0;
	}

	return 1;
}

static int
sign_and_reach_verify(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const uint8_t *hpub, size_t hpub_len,
	unsigned keynum, unsigned trial)
{
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	uint8_t sig[40 + 4 * MAXN], salt[40];
	shake_context sc_data;
	int verified;

	make_salt(salt, salt_len, logn, keynum, trial);
	make_message_context(&sc_data, logn, trial);
	if (!e8_sign_dummy_uncompressed(logn, sig, sig_len, &sc_data,
		hpub, hpub_len, f, g, F, G, salt))
	{
		fprintf(stderr,
			"ERR: dummy E8 signing failed for logn=%u"
			" key=%u trial=%u\n", logn, keynum, trial);
		return 0;
	}
	if (!check_signature_zero_s(logn, sig, sig_len)) {
		return 0;
	}

	make_message_context(&sc_data, logn, trial);
	verified = e8_verify_uncompressed(logn, sig, sig_len, &sc_data,
		hpub, hpub_len, q00, q01, q10, q11);
	(void)verified;
	return 1;
}

static int
sign_offset_and_check(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const uint8_t *hpub, size_t hpub_len,
	unsigned keynum, unsigned caseid)
{
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	uint8_t sig[40 + 4 * MAXN], salt[40];
	int32_t r0[MAXN], r1[MAXN];
	shake_context sc_data;
	int verified;

	make_offset_case(logn, caseid, r0, r1);
	make_salt(salt, salt_len, logn, keynum, SIGN_TRIALS + caseid);
	make_message_context(&sc_data, logn, SIGN_TRIALS + caseid);
	if (!e8_sign_dummy_offset_uncompressed(logn, sig, sig_len,
		&sc_data, hpub, hpub_len, f, g, F, G, salt, r0, r1))
	{
		fprintf(stderr,
			"ERR: offset dummy E8 signing failed for logn=%u"
			" key=%u case=%u\n", logn, keynum, caseid);
		return 0;
	}
	if (!check_nonzero_offset_signature(logn, f, g, F, G,
		q00, q01, q10, q11, hpub, hpub_len,
		salt, sig, sig_len, r0, r1, caseid))
	{
		return 0;
	}

	make_message_context(&sc_data, logn, SIGN_TRIALS + caseid);
	verified = e8_verify_uncompressed(logn, sig, sig_len, &sc_data,
		hpub, hpub_len, q00, q01, q10, q11);
	(void)verified;
	return 1;
}

static int
test_identity_logn(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t hpub_len = (size_t)1 << (logn - 4);
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	uint8_t hpub[64];
	int32_t q00[MAXN], q01[MAXN], q10[MAXN], q11[MAXN];

	make_identity_basis(n, f, g, F, G);
	make_hpub(hpub, hpub_len, logn, 0);
	e8_compute_qform(q00, q01, q10, q11, f, g, F, G, logn);

	for (unsigned trial = 0; trial < SIGN_TRIALS; trial ++) {
		if (!sign_and_reach_verify(logn, f, g, F, G,
			q00, q01, q10, q11, hpub, hpub_len, 0, trial))
		{
			return 0;
		}
	}
	for (unsigned caseid = 0; caseid < OFFSET_CASES; caseid ++) {
		if (!sign_offset_and_check(logn, f, g, F, G,
			q00, q01, q10, q11, hpub, hpub_len, 0, caseid))
		{
			return 0;
		}
	}
	return 1;
}

static int
test_hawk_basis_logn(unsigned logn)
{
	size_t hpub_len = (size_t)1 << (logn - 4);
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	uint8_t hpub[64];
	int32_t q00[MAXN], q01[MAXN], q10[MAXN], q11[MAXN];
	uint64_t rng_state = UINT64_C(0xE8516E0000000000) + logn;

	for (unsigned keynum = 0; keynum < KEYGEN_TRIALS; keynum ++) {
		if (!make_basis(logn, f, g, F, G, &rng_state)) {
			fprintf(stderr,
				"ERR: Hawk_keygen failed for E8 dummy signing"
				" logn=%u key=%u\n", logn, keynum);
			return 0;
		}
		make_hpub(hpub, hpub_len, logn, keynum + 1);
		e8_compute_qform(q00, q01, q10, q11, f, g, F, G, logn);
		for (unsigned trial = 0; trial < SIGN_TRIALS; trial ++) {
			if (!sign_and_reach_verify(logn, f, g, F, G,
				q00, q01, q10, q11, hpub, hpub_len,
				keynum + 1, trial))
			{
				return 0;
			}
		}
		for (unsigned caseid = 0; caseid < OFFSET_CASES; caseid ++) {
			if (!sign_offset_and_check(logn, f, g, F, G,
				q00, q01, q10, q11, hpub, hpub_len,
				keynum + 1, caseid))
			{
				return 0;
			}
		}
	}
	return 1;
}

static int
test_logn(unsigned logn)
{
	return test_identity_logn(logn) && test_hawk_basis_logn(logn);
}

int
main(void)
{
	for (unsigned logn = 8; logn <= 10; logn ++) {
		printf("E8 dummy signing n=%u: ", 1u << logn);
		fflush(stdout);
		if (!test_logn(logn)) {
			return 1;
		}
		printf("done.\n");
	}

	return 0;
}
