#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <limits.h>

#define E8_MAXN   1024

typedef __int128 e8_i128;

static const double E8_SIGMA_VERIFY[] = {
	2.03, 1.99, 1.95
};

typedef struct {
	uint32_t p;
	uint32_t p0i;
	uint32_t g;
} e8_prime;

static const e8_prime E8_NTT_PRIMES[] = {
	{ 2147473409, 2042615807, 1790111537 },
	{ 2147389441, 1862176767,  677655126 },
	{ 2147387393, 1472104447,  563781659 },
	{ 2147377153, 3690881023,  978644358 }
};

static int
e8_param_salt_len(unsigned logn, size_t *salt_len)
{
	switch (logn) {
	case 8:
		*salt_len = 14;
		return 1;
	case 9:
		*salt_len = 24;
		return 1;
	case 10:
		*salt_len = 40;
		return 1;
	default:
		return 0;
	}
}

static int
e8_param_verify_bound(unsigned logn, int64_t *bound)
{
	double sigma_verify;

	if (!e8_sigma_verify_default(logn, &sigma_verify)) {
		return 0;
	}
	return e8_verify_bound_from_sigma(logn, sigma_verify, bound);
}

static uint32_t
e8_mp_montymul(uint32_t a, uint32_t b, uint32_t p, uint32_t p0i)
{
	uint64_t z = (uint64_t)a * (uint64_t)b;
	uint32_t w = (uint32_t)z * p0i;
	uint32_t d = (uint32_t)((z + (uint64_t)w * (uint64_t)p) >> 32) - p;
	return d + (p & tbmask(d));
}

static uint32_t
e8_mp_add(uint32_t a, uint32_t b, uint32_t p)
{
	uint32_t d = a + b;
	return d >= p ? d - p : d;
}

static uint32_t
e8_mp_mul(uint32_t a, uint32_t b, uint32_t p)
{
	return (uint32_t)(((uint64_t)a * b) % p);
}

static uint32_t
e8_mp_pow(uint32_t x, uint32_t e, uint32_t p)
{
	uint32_t y = 1;

	while (e != 0) {
		if ((e & 1) != 0) {
			y = e8_mp_mul(y, x, p);
		}
		x = e8_mp_mul(x, x, p);
		e >>= 1;
	}
	return y;
}

static uint32_t
e8_mp_set_i32(int32_t x, uint32_t p)
{
	int64_t y = x % (int64_t)p;

	if (y < 0) {
		y += p;
	}
	return (uint32_t)y;
}

static size_t
e8_bitrev(size_t x, unsigned logn)
{
	size_t y = 0;

	for (unsigned u = 0; u < logn; u ++) {
		y = (y << 1) | (x & 1);
		x >>= 1;
	}
	return y;
}

static void
e8_mkgm(unsigned logn, uint32_t *gm,
	uint32_t g, uint32_t p, uint32_t p0i)
{
	size_t n = (size_t)1 << logn;

	for (unsigned u = logn; u < 10; u ++) {
		g = e8_mp_montymul(g, g, p, p0i);
	}

	uint32_t x = (uint32_t)(UINT64_C(0x100000000) % p);
	for (size_t u = 0; u < n; u ++) {
		gm[e8_bitrev(u, logn)] = x;
		x = e8_mp_montymul(x, g, p, p0i);
	}
}

static void
e8_ntt(unsigned logn, uint32_t *a,
	const uint32_t *gm, uint32_t p, uint32_t p0i)
{
	size_t t = (size_t)1 << logn;

	for (unsigned lm = 0; lm < logn; lm ++) {
		size_t m = (size_t)1 << lm;
		size_t ht = t >> 1;
		size_t v0 = 0;
		for (size_t u = 0; u < m; u ++) {
			uint32_t s = gm[u + m];
			for (size_t v = 0; v < ht; v ++) {
				size_t k1 = v0 + v;
				size_t k2 = k1 + ht;
				uint32_t x1 = a[k1];
				uint32_t x2 = e8_mp_montymul(a[k2], s, p, p0i);
				a[k1] = e8_mp_add(x1, x2, p);
				a[k2] = x1 >= x2 ? x1 - x2 : x1 + p - x2;
			}
			v0 += t;
		}
		t = ht;
	}
}

static void
e8_poly_to_ntt(unsigned logn, uint32_t *d, const int32_t *a,
	uint32_t p, uint32_t p0i, const uint32_t *gm)
{
	size_t n = (size_t)1 << logn;

	for (size_t u = 0; u < n; u ++) {
		d[u] = e8_mp_set_i32(a[u], p);
	}
	e8_ntt(logn, d, gm, p, p0i);
}

static int
e8_completion_half_mod(uint32_t *half,
	const int32_t *q00, const int32_t *q01,
	const int32_t *w0, const int32_t *w1,
	unsigned logn, const e8_prime *prime)
{
	size_t n = (size_t)1 << logn;
	size_t hn = n >> 1;
	uint32_t gm[E8_MAXN];
	uint32_t tq00[E8_MAXN], tq01[E8_MAXN], tw0[E8_MAXN], tw1[E8_MAXN];
	uint32_t tdelta[E8_MAXN], td[E8_MAXN], te[E8_MAXN];
	int32_t delta[E8_MAXN];
	uint32_t acc = 0;
	uint32_t p = prime->p;

	e8_mkgm(logn, gm, prime->g, p, prime->p0i);
	e8_make_delta(delta, logn);
	e8_poly_to_ntt(logn, tq00, q00, p, prime->p0i, gm);
	e8_poly_to_ntt(logn, tq01, q01, p, prime->p0i, gm);
	e8_poly_to_ntt(logn, tw0, w0, p, prime->p0i, gm);
	e8_poly_to_ntt(logn, tw1, w1, p, prime->p0i, gm);
	e8_poly_to_ntt(logn, tdelta, delta, p, prime->p0i, gm);

	for (size_t u = 0; u < n; u ++) {
		if (tq00[u] == 0) {
			return 0;
		}
		td[u] = e8_mp_mul(tw1[u], e8_mp_pow(tq00[u], p - 2, p), p);
		te[u] = e8_mp_add(tw0[u], e8_mp_mul(tq01[u], td[u], p), p);
	}

	for (size_t u = 0; u < hn; u ++) {
		size_t v = n - 1 - u;
		uint32_t term1 = e8_mp_mul(tq00[u],
			e8_mp_mul(te[u], te[v], p), p);
		uint32_t term2 = e8_mp_mul(tdelta[u],
			e8_mp_mul(td[u], tw1[v], p), p);
		acc = e8_mp_add(acc, e8_mp_add(term1, term2, p), p);
	}

	*half = acc;
	return 1;
}

static int16_t
dec_i16le(const uint8_t *buf)
{
	uint32_t x = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8);
	return (int16_t)(x < 0x8000u ? (int32_t)x : (int32_t)x - 0x10000);
}

static void
enc_i16le(uint8_t *buf, int16_t x)
{
	uint32_t y = (uint16_t)x;
	buf[0] = (uint8_t)y;
	buf[1] = (uint8_t)(y >> 8);
}

static void
poly_mul_add_i128(e8_i128 *d,
	const int32_t *a, const int32_t *b, size_t n)
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
			e8_i128 x = (e8_i128)a[u] * b[v];
			if (w >= n) {
				w -= n;
				x = -x;
			}
			d[w] += x;
		}
	}
}

static e8_i128
inner_i32_i128(const int32_t *a, const e8_i128 *b, size_t n)
{
	e8_i128 s = 0;
	for (size_t u = 0; u < n; u ++) {
		s += (e8_i128)a[u] * b[u];
	}
	return s;
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

/* see hawk_e8_inner.h */
size_t
e8_salt_len(unsigned logn)
{
	size_t salt_len;

	if (!e8_param_salt_len(logn, &salt_len)) {
		return 0;
	}
	return salt_len;
}

/* see hawk_e8_inner.h */
size_t
e8_sig_uncompressed_size(unsigned logn)
{
	size_t salt_len;

	if (!e8_param_salt_len(logn, &salt_len)) {
		return 0;
	}
	return salt_len + ((size_t)4 << logn);
}

/* see hawk_e8_inner.h */
int
e8_encode_sig_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const uint8_t *salt,
	const int16_t *s0, const int16_t *s1)
{
	size_t salt_len = e8_salt_len(logn);
	uint8_t *buf = sig;

	if (salt_len == 0 || sig_len != e8_sig_uncompressed_size(logn)) {
		return 0;
	}
	size_t n = (size_t)1 << logn;
	memcpy(buf, salt, salt_len);
	buf += salt_len;
	for (size_t u = 0; u < n; u ++, buf += 2) {
		enc_i16le(buf, s0[u]);
	}
	for (size_t u = 0; u < n; u ++, buf += 2) {
		enc_i16le(buf, s1[u]);
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_decode_sig_uncompressed(unsigned logn,
	uint8_t *salt, int16_t *s0, int16_t *s1,
	const void *sig, size_t sig_len)
{
	size_t salt_len = e8_salt_len(logn);
	const uint8_t *buf = sig;

	if (salt_len == 0 || sig_len != e8_sig_uncompressed_size(logn)) {
		return 0;
	}
	size_t n = (size_t)1 << logn;
	if (salt != NULL) {
		memcpy(salt, buf, salt_len);
	}
	buf += salt_len;
	if (s0 != NULL) {
		for (size_t u = 0; u < n; u ++, buf += 2) {
			s0[u] = dec_i16le(buf);
		}
	} else {
		buf += n << 1;
	}
	if (s1 != NULL) {
		for (size_t u = 0; u < n; u ++, buf += 2) {
			s1[u] = dec_i16le(buf);
		}
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sym_break(const int32_t *w0, const int32_t *w1, unsigned logn)
{
	if (logn < 8 || logn > 10 || w0 == NULL || w1 == NULL) {
		return 0;
	}

	size_t n = (size_t)1 << logn;
	for (size_t u = 0; u < n; u ++) {
		if (w1[u] > 0) {
			return 1;
		}
		if (w1[u] < 0) {
			return 0;
		}
	}
	for (size_t u = 0; u < n; u ++) {
		if (w0[u] > 0) {
			return 1;
		}
		if (w0[u] < 0) {
			return 0;
		}
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_qnorm_direct(int64_t *norm,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const int32_t *w0, const int32_t *w1, unsigned logn)
{
	if (logn < 8 || logn > 10) {
		return 0;
	}

	size_t n = (size_t)1 << logn;
	e8_i128 qw0[E8_MAXN], qw1[E8_MAXN];
	memset(qw0, 0, n * sizeof *qw0);
	memset(qw1, 0, n * sizeof *qw1);
	poly_mul_add_i128(qw0, q00, w0, n);
	poly_mul_add_i128(qw0, q01, w1, n);
	poly_mul_add_i128(qw1, q10, w0, n);
	poly_mul_add_i128(qw1, q11, w1, n);

	e8_i128 acc = inner_i32_i128(w0, qw0, n)
		+ inner_i32_i128(w1, qw1, n);
	if (acc < 0 || acc > (e8_i128)INT64_MAX) {
		return 0;
	}
	*norm = (int64_t)acc;
	return 1;
}

/*
 * Compute the E8 completion-of-squares verifier norm:
 *
 *   n ||w||^2_Q = Tr(q00 e e^* + Delta d w1^*)
 *   d = w1/q00
 *   e = w0 + q01 d
 *
 * The modular NTT calculation below returns the half-trace, i.e.
 * (n/2)*||w||^2_Q. If the value is not represented unambiguously as a
 * small integer across the selected moduli, this helper returns 0. That is
 * fine for verification because all accepted values must be below the small
 * verifier threshold.
 */
/* see hawk_e8_inner.h */
int
e8_qnorm_completion(int64_t *norm,
	const int32_t *q00, const int32_t *q01,
	const int32_t *w0, const int32_t *w1, unsigned logn)
{
	if (logn < 8 || logn > 10 || norm == NULL) {
		return 0;
	}

	uint32_t half = 0;
	for (size_t u = 0;
		u < sizeof E8_NTT_PRIMES / sizeof E8_NTT_PRIMES[0];
		u ++)
	{
		uint32_t h;

		if (!e8_completion_half_mod(&h, q00, q01,
			w0, w1, logn, &E8_NTT_PRIMES[u]))
		{
			return 0;
		}
		if (u == 0) {
			half = h;
		} else if (h != half) {
			return 0;
		}
	}

	size_t hn = (size_t)1 << (logn - 1);
	if ((half & (uint32_t)(hn - 1)) != 0) {
		return 0;
	}
	*norm = (int64_t)(half >> (logn - 1));
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sigma_verify_default(unsigned logn, double *sigma_verify)
{
	if (logn < 8 || logn > 10 || sigma_verify == NULL) {
		return 0;
	}
	*sigma_verify = E8_SIGMA_VERIFY[logn - 8];
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_verify_bound_from_sigma(unsigned logn, double sigma_verify, int64_t *bound)
{
	if (logn < 8 || logn > 10 || !(sigma_verify > 0.0)
		|| bound == NULL)
	{
		return 0;
	}

	size_t n = (size_t)1 << logn;
	double x = 8.0 * (double)n * sigma_verify * sigma_verify;
	if (!(x >= 0.0) || x > (double)INT64_MAX) {
		return 0;
	}
	*bound = (int64_t)x;
	return 1;
}

static int
verify_uncompressed_with_bound(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11, int64_t bound)
{
	size_t salt_len;
	int64_t norm;
	uint8_t salt[40], h[256];
	int16_t s0[E8_MAXN], s1[E8_MAXN];
	int32_t w0[E8_MAXN], w1[E8_MAXN];

	(void)q10;
	(void)q11;

	if (!e8_param_salt_len(logn, &salt_len)
		|| hpub_len != ((size_t)1 << (logn - 4)))
	{
		return 0;
	}
	size_t n = (size_t)1 << logn;
	if (!e8_decode_sig_uncompressed(logn,
		salt, s0, s1, sig, sig_len))
	{
		return 0;
	}

	hash_to_h(logn, h, sc_data, hpub, salt, salt_len);
	for (size_t u = 0; u < n; u ++) {
		w0[u] = (int32_t)get_bit(h, u) - 2 * (int32_t)s0[u];
		w1[u] = (int32_t)get_bit(h + (n >> 3), u)
			- 2 * (int32_t)s1[u];
	}

	if (!e8_sym_break(w0, w1, logn)) {
		return 0;
	}

	if (!e8_qnorm_completion(&norm, q00, q01, w0, w1, logn)) {
		return 0;
	}
	return norm <= bound;
}

/* see hawk_e8_inner.h */
int
e8_verify_uncompressed_with_sigma(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11, double sigma_verify)
{
	int64_t bound;

	if (!e8_verify_bound_from_sigma(logn, sigma_verify, &bound)) {
		return 0;
	}
	return verify_uncompressed_with_bound(logn,
		sig, sig_len, sc_data, hpub, hpub_len,
		q00, q01, q10, q11, bound);
}

/* see hawk_e8_inner.h */
int
e8_verify_uncompressed(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11)
{
	int64_t bound;

	if (!e8_param_verify_bound(logn, &bound)) {
		return 0;
	}
	return verify_uncompressed_with_bound(logn,
		sig, sig_len, sc_data, hpub, hpub_len,
		q00, q01, q10, q11, bound);
}

#endif
