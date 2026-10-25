#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <limits.h>

#define E8_MAXN   1024

#if defined(__SIZEOF_INT128__)
typedef __int128 e8_i128;
#else
#error "E8 experimental path requires 128-bit integer support (GCC/Clang)"
#endif

static const double E8_SIGMA_VERIFY[] = {
	2.03, 1.99, 1.95
};

static int
e8_param_verify_bound(unsigned logn, int64_t *bound)
{
	double sigma_verify;

	if (!e8_sigma_verify_default(logn, &sigma_verify)) {
		return 0;
	}
	return e8_verify_bound_from_sigma(logn, sigma_verify, bound);
}

static int
e8_completion_half_mod(uint32_t *half,
	const int32_t *q00, const int32_t *q01,
	const int32_t *w0, const int32_t *w1,
	unsigned logn, const e8_ntt_prime *prime)
{
	size_t n = (size_t)1 << logn;
	size_t hn = n >> 1;
	uint32_t tq00[E8_MAXN], tq01[E8_MAXN], tw0[E8_MAXN], tw1[E8_MAXN];
	uint32_t tdelta[E8_MAXN], td[E8_MAXN], te[E8_MAXN];
	int32_t delta[E8_MAXN];
	uint32_t acc = 0;
	uint32_t p = prime->p;

	e8_make_delta(delta, logn);
	e8_ntt_to_ntt(logn, tq00, q00, prime);
	e8_ntt_to_ntt(logn, tq01, q01, prime);
	e8_ntt_to_ntt(logn, tw0, w0, prime);
	e8_ntt_to_ntt(logn, tw1, w1, prime);
	e8_ntt_to_ntt(logn, tdelta, delta, prime);

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

	salt_len = e8_salt_len(logn);
	if (salt_len == 0 || hpub_len != ((size_t)1 << (logn - 4)))
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
