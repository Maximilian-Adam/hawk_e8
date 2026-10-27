#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <limits.h>

#define E8_MAXN   1024

#if defined(__SIZEOF_INT128__)
typedef __int128 e8_i128;
#else
#error "E8 experimental path requires 128-bit integer support (GCC/Clang)"
#endif

/*
 * The public API is integer-only (make check-integer-only). Its sampler
 * (e8_api.c) uses the generated tables of the prototype signing standard deviations
 * sigma_sign = 3.592, 3.631, 3.669 (the values used by the E8 tests,
 * diagnostics and benchmarks; e8/tables/gen_tables.py), and its norm
 * bound is floor(8 n sigma_verify^2) with the default sigma_verify = 2.03,
 * 1.99, 1.95 (e8_sigma_verify_default()), precomputed here.
 */
static const int64_t E8_NORM_BOUND[] = {
	8439, 16220, 31150
};

/* see hawk_e8_inner.h */
int64_t
e8_norm_bound_default(unsigned logn)
{
	return logn >= 8 && logn <= 10 ? E8_NORM_BOUND[logn - 8] : 0;
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
 * Verifier with HAWK's techniques (hawk_vrfy.c, verify_inner(), the
 * norm computation after s0 is known): per prime, the NTT of w1, the
 * auto-adjoint NTT of q00 (first n/2 values) and their batch inversion
 * (Montgomery's trick: one modular inversion and 3(n/2 - 1)
 * multiplications), d = w1/q00, the NTT of q01 and e = w0 + q01 d, then
 * the half-trace
 *
 *   V = (n/2) ||w||^2_Q = sum_{u < n/2} q00[u] e[u] e[n-1-u]
 *                               + Delta[u] d[u] w1[n-1-u]   (mod p)
 *
 * (NTT values; adj(x) has NTT value x[n-1-u] at u, and q00, Delta are
 * auto-adjoint). E8 differs from HAWK only where the lattice does: the
 * Delta factor (HAWK's determinant is 1; Delta's NTT is a generated
 * constant), w0 is taken from the signature instead of being
 * reconstructed, and E8_VRFY_PRIMES = 3 primes instead of 2
 * (docs/verifier_range.md: with the key generation limits, V is below
 * the product of the three primes for every int16 s0, s1). As in HAWK,
 * q00's NTT is computed again after the inversion; twiddles, Montgomery
 * constants and the integer bound are precomputed. All residues must
 * agree; after the first prime, a residue above early_max (at most
 * (n/2) * bound, below every prime, so the true value would be rejected)
 * rejects at once.
 *
 * Returns 1 and sets *half = V when every residue agrees, 0 otherwise
 * (disagreement, early reject, or q00 not invertible modulo a prime).
 */
static int
completion_half(uint32_t *half,
	const int32_t *q00, const int32_t *q01,
	const int32_t *w0, const int32_t *w1, unsigned logn,
	uint32_t early_max)
{
	size_t n = (size_t)1 << logn;
	size_t hn = n >> 1;
	uint32_t c1[E8_MAXN], c2[E8_MAXN];
	uint32_t tnorm = 0;

	for (unsigned i = 0; i < E8_VRFY_PRIMES; i ++) {
		const e8_ntt_tab *tab = e8_ntt_tab_get(i);
		const uint32_t *delta = e8_ntt_delta_mont(logn, i);
		uint32_t p = tab->p;
		uint32_t p0i = tab->p0i;
		uint32_t R2 = tab->R2;
		uint32_t R3 = tab->R3;

		/* c2 <- w1 */
		e8_poly_to_ntt_tab(logn, c2, w1, tab);

		/* c1 <- q00 (auto-adjoint, first n/2 values) */
		e8_poly_to_ntt_autoadj_tab(logn, c1, q00, tab);

		/*
		 * c1 <- 1/c1 with Montgomery's trick (prefix products in
		 * the upper half of c1, one inversion, then back down),
		 * as HAWK. The prefix product is (q00[0] ... q00[n/2-1])
		 * / R^(n/2-1); its inverse is taken by Fermat's little
		 * theorem in the Montgomery domain, and every resulting
		 * c1[u] is the plain 1/q00[u].
		 */
		uint32_t bx = c1[0];
		c1[hn] = bx;
		for (size_t u = 1; u < hn; u ++) {
			bx = e8_mp_montymul(bx, c1[u], p, p0i);
			c1[u + hn] = bx;
		}
		if (bx == 0) {
			return 0;
		}
		uint32_t xm = e8_mp_montymul(bx, R2, p, p0i);
		uint32_t ym = (uint32_t)(UINT64_C(0x100000000) % p);
		for (int k = 31; k >= 0; k --) {
			ym = e8_mp_montymul(ym, ym, p, p0i);
			if (((p - 2) >> k) & 1) {
				ym = e8_mp_montymul(ym, xm, p, p0i);
			}
		}
		bx = e8_mp_montymul(ym, 1, p, p0i);
		for (size_t u = hn - 1; u > 0; u --) {
			uint32_t ix = e8_mp_montymul(bx, c1[u + hn - 1], p, p0i);
			bx = e8_mp_montymul(bx, c1[u], p, p0i);
			c1[u] = ix;
		}
		c1[0] = bx;

		/*
		 * c2 <- c2*c1 = d = w1/q00 (times 1/R)
		 * nnacc = sum Delta[u] d[u] w1[n-1-u] (times 1/R^2; Delta
		 * is in Montgomery form)
		 */
		uint32_t nnacc = 0;
		for (size_t u = 0; u < hn; u ++) {
			size_t v = n - 1 - u;
			uint32_t x1 = c2[u];
			uint32_t x2 = c2[v];
			uint32_t qx = c1[u];

			x1 = e8_mp_montymul(x1, qx, p, p0i);
			nnacc = e8_mp_add(nnacc, e8_mp_montymul(
				e8_mp_montymul(x1, x2, p, p0i), delta[u],
				p, p0i), p);
			x2 = e8_mp_montymul(x2, qx, p, p0i);
			c2[u] = x1;
			c2[v] = x2;
		}

		/* c1 <- q01, then c1*c2 = q01 d (plain, via R3) */
		e8_poly_to_ntt_tab(logn, c1, q01, tab);
		for (size_t u = 0; u < n; u ++) {
			c1[u] = e8_mp_montymul(
				e8_mp_montymul(c1[u], c2[u], p, p0i),
				R3, p, p0i);
		}

		/* c2 <- w0, then e = w0 + q01 d */
		e8_poly_to_ntt_tab(logn, c2, w0, tab);
		for (size_t u = 0; u < n; u ++) {
			c2[u] = e8_mp_add(c2[u], c1[u], p);
		}

		/* c1 <- q00 (auto-adjoint); nnacc += sum q00 e adj(e) */
		e8_poly_to_ntt_autoadj_tab(logn, c1, q00, tab);
		for (size_t u = 0; u < hn; u ++) {
			uint32_t x1 = c2[u];
			uint32_t x2 = c2[(n - 1) - u];

			nnacc = e8_mp_add(nnacc, e8_mp_montymul(c1[u],
				e8_mp_montymul(x1, x2, p, p0i), p, p0i), p);
		}

		/* nnacc was divided by R^2; back to the plain residue */
		nnacc = e8_mp_montymul(nnacc, R3, p, p0i);
		if (i == 0) {
			if (nnacc > early_max) {
				return 0;
			}
			tnorm = nnacc;
		} else if (nnacc != tnorm) {
			return 0;
		}
	}
	*half = tnorm;
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_qnorm_completion(int64_t *norm,
	const int32_t *q00, const int32_t *q01,
	const int32_t *w0, const int32_t *w1, unsigned logn)
{
	uint32_t half;

	if (logn < 8 || logn > 10 || norm == NULL
		|| !completion_half(&half, q00, q01, w0, w1, logn,
			UINT32_MAX))
	{
		return 0;
	}
	if ((half & (((uint32_t)1 << (logn - 1)) - 1)) != 0) {
		return 0;
	}
	*norm = (int64_t)(half >> (logn - 1));
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sig_to_w(unsigned logn, int32_t *w0, int32_t *w1,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub)
{
	size_t salt_len;
	uint8_t salt[40], h[256];
	int16_t s0[E8_MAXN], s1[E8_MAXN];

	if (logn < 8 || logn > 10) {
		return 0;
	}
	salt_len = e8_salt_len(logn);
	if (!e8_decode_sig_uncompressed(logn,
		salt, s0, s1, sig, sig_len))
	{
		return 0;
	}
	size_t n = (size_t)1 << logn;
	hash_to_h(logn, h, sc_data, hpub, salt, salt_len);
	for (size_t u = 0; u < n; u ++) {
		w0[u] = (int32_t)get_bit(h, u) - 2 * (int32_t)s0[u];
		w1[u] = (int32_t)get_bit(h + (n >> 3), u)
			- 2 * (int32_t)s1[u];
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_verify_uncompressed_bound(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01, int64_t bound)
{
	size_t salt_len;
	uint8_t salt[40], h[256];
	int16_t s0[E8_MAXN], s1[E8_MAXN];
	int32_t w0[E8_MAXN], w1[E8_MAXN];
	uint32_t half;

	if (logn < 8 || logn > 10 || bound < 0) {
		return 0;
	}
	salt_len = e8_salt_len(logn);
	if (salt_len == 0 || hpub_len != ((size_t)1 << (logn - 4))) {
		return 0;
	}
	size_t n = (size_t)1 << logn;
	size_t hn = n >> 1;

	/*
	 * The early reject limit (n/2) * bound must be below every prime
	 * (all above 2^30); with the default bounds it is below 2^25.
	 */
	if ((uint64_t)bound > ((UINT64_C(1) << 30) >> (logn - 1))) {
		return 0;
	}
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
	if (!completion_half(&half, q00, q01, w0, w1, logn,
		(uint32_t)((uint64_t)bound << (logn - 1))))
	{
		return 0;
	}
	return (half & (uint32_t)(hn - 1)) == 0
		&& (int64_t)(half >> (logn - 1)) <= bound;
}

/* see hawk_e8_inner.h */
int
e8_verify_uncompressed(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01)
{
	return e8_verify_uncompressed_bound(logn, sig, sig_len,
		sc_data, hpub, hpub_len, q00, q01,
		e8_norm_bound_default(logn));
}
#endif
