#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <limits.h>

#if defined(__SIZEOF_INT128__)
typedef __int128 e8_s128;
#else
#error "E8 experimental path requires 128-bit integer support (GCC/Clang)"
#endif

/* see hawk_e8_inner.h */
const e8_ntt_prime E8_NTT_PRIMES[E8_NTT_PRIME_COUNT] = {
	{ 2147473409, 2042615807, 1790111537 },
	{ 2147389441, 1862176767,  677655126 },
	{ 2147387393, 1472104447,  563781659 },
	{ 2147377153, 3690881023,  978644358 }
};

/*
 * Generated twiddle tables and constants (e8_ntt_tab, hawk_e8_inner.h);
 * this is the only translation unit that includes them.
 */
#include "e8_tables_ntt.h"

/* see hawk_e8_inner.h */
const e8_ntt_tab *
e8_ntt_tab_get(unsigned prime)
{
	return prime < E8_NTT_TAB_PRIMES ? &e8_ntt_tabs[prime] : NULL;
}

/* see hawk_e8_inner.h */
const uint32_t *
e8_ntt_delta_mont(unsigned logn, unsigned prime)
{
	if (prime >= E8_NTT_TAB_PRIMES) {
		return NULL;
	}
	switch (logn) {
	case 8:
		return e8_ntt_delta_logn8[prime];
	case 9:
		return e8_ntt_delta_logn9[prime];
	case 10:
		return e8_ntt_delta_logn10[prime];
	default:
		return NULL;
	}
}

/* see hawk_e8_inner.h */
const uint32_t *
e8_ntt_s00_mont(unsigned logn)
{
	switch (logn) {
	case 8:
		return e8_ntt_s00_logn8;
	case 9:
		return e8_ntt_s00_logn9;
	case 10:
		return e8_ntt_s00_logn10;
	default:
		return NULL;
	}
}

/* see hawk_e8_inner.h */
const uint32_t *
e8_ntt_s01_mont(unsigned logn)
{
	switch (logn) {
	case 8:
		return e8_ntt_s01_logn8;
	case 9:
		return e8_ntt_s01_logn9;
	case 10:
		return e8_ntt_s01_logn10;
	default:
		return NULL;
	}
}

/* see hawk_e8_inner.h */
uint32_t
e8_mp_montymul(uint32_t a, uint32_t b, uint32_t p, uint32_t p0i)
{
	uint64_t z = (uint64_t)a * (uint64_t)b;
	uint32_t w = (uint32_t)z * p0i;
	uint32_t d = (uint32_t)((z + (uint64_t)w * (uint64_t)p) >> 32) - p;
	return d + (p & tbmask(d));
}

/* see hawk_e8_inner.h */
uint32_t
e8_mp_add(uint32_t a, uint32_t b, uint32_t p)
{
	uint32_t d = a + b;
	return d >= p ? d - p : d;
}

/* see hawk_e8_inner.h */
uint32_t
e8_mp_sub(uint32_t a, uint32_t b, uint32_t p)
{
	uint32_t d = a - b;
	return d + (p & tbmask(d));
}

/* see hawk_e8_inner.h */
uint32_t
e8_mp_mul(uint32_t a, uint32_t b, uint32_t p)
{
	return (uint32_t)(((uint64_t)a * b) % p);
}

/* see hawk_e8_inner.h */
uint32_t
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

/* see hawk_e8_inner.h */
uint32_t
e8_mp_set_i32(int32_t x, uint32_t p)
{
	int64_t y = x % (int64_t)p;

	if (y < 0) {
		y += p;
	}
	return (uint32_t)y;
}

/* see hawk_e8_inner.h */
size_t
e8_bitrev(size_t x, unsigned logn)
{
	size_t y = 0;

	for (unsigned u = 0; u < logn; u ++) {
		y = (y << 1) | (x & 1);
		x >>= 1;
	}
	return y;
}

/* see hawk_e8_inner.h */
void
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

/* see hawk_e8_inner.h */
void
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

/* see hawk_e8_inner.h */
void
e8_intt(unsigned logn, uint32_t *a,
	const uint32_t *igm, uint32_t p, uint32_t p0i)
{
	size_t t = 1;

	for (unsigned lm = 0; lm < logn; lm ++) {
		size_t hm = (size_t)1 << (logn - 1 - lm);
		size_t dt = t << 1;

		for (size_t u = 0; u < hm; u ++) {
			uint32_t s = igm[u + hm];
			size_t v0 = u * dt;
			for (size_t v = 0; v < t; v ++) {
				size_t k1 = v0 + v;
				size_t k2 = k1 + t;
				uint32_t x1 = a[k1];
				uint32_t x2 = a[k2];

				a[k1] = e8_mp_add(x1, x2, p);
				a[k2] = e8_mp_montymul(
					x1 >= x2 ? x1 - x2 : x1 + p - x2,
					s, p, p0i);
			}
		}
		t = dt;
	}
}

/* see hawk_e8_inner.h */
void
e8_ntt_to_ntt(unsigned logn, uint32_t *d,
	const int32_t *a, const e8_ntt_prime *prime)
{
	size_t n = (size_t)1 << logn;
	uint32_t gm[1024];
	uint32_t p = prime->p;

	e8_mkgm(logn, gm, prime->g, p, prime->p0i);
	for (size_t u = 0; u < n; u ++) {
		d[u] = e8_mp_set_i32(a[u], p);
	}
	e8_ntt(logn, d, gm, p, prime->p0i);
}

/* see hawk_e8_inner.h */
void
e8_poly_to_ntt_tab(unsigned logn, uint32_t *d, const int32_t *a,
	const e8_ntt_tab *tab)
{
	size_t n = (size_t)1 << logn;
	uint32_t p = tab->p;

	for (size_t u = 0; u < n; u ++) {
		uint32_t x = (uint32_t)a[u];
		d[u] = x + (p & tbmask(x));
	}
	e8_ntt(logn, d, tab->gm, p, tab->p0i);
}

/* see hawk_e8_inner.h */
void
e8_poly_to_ntt_autoadj_tab(unsigned logn, uint32_t *d, const int32_t *a,
	const e8_ntt_tab *tab)
{
	size_t hn = (size_t)1 << (logn - 1);
	size_t qn = hn >> 1;
	uint32_t p = tab->p;
	uint32_t p0i = tab->p0i;
	const uint32_t *gm = tab->gm;

	for (size_t u = 0; u < hn; u ++) {
		uint32_t x = (uint32_t)a[u];
		d[u] = x + (p & tbmask(x));
	}

	/*
	 * HAWK's mp_NTT_autoadj(): the first NTT layer combines a[u] with
	 * a[u + n/2] = -a[n/2 - u] (and a[n/2] = 0), then the remaining
	 * layers run on the first half only.
	 */
	uint32_t s1 = gm[1];
	for (size_t u = 1; u < qn; u ++) {
		uint32_t x1 = d[u];
		uint32_t x2 = d[hn - u];
		d[u] = e8_mp_sub(x1, e8_mp_montymul(x2, s1, p, p0i), p);
		d[hn - u] = e8_mp_sub(x2, e8_mp_montymul(x1, s1, p, p0i), p);
	}
	d[qn] = e8_mp_sub(d[qn], e8_mp_montymul(d[qn], s1, p, p0i), p);

	size_t t = hn;
	for (unsigned lm = 1; lm < logn; lm ++) {
		size_t m = (size_t)1 << lm;
		size_t ht = t >> 1;
		size_t v0 = 0;
		for (size_t u = 0; u < (m >> 1); u ++) {
			uint32_t s = gm[u + m];
			for (size_t v = 0; v < ht; v ++) {
				size_t k1 = v0 + v;
				size_t k2 = k1 + ht;
				uint32_t x1 = d[k1];
				uint32_t x2 = e8_mp_montymul(d[k2], s, p, p0i);
				d[k1] = e8_mp_add(x1, x2, p);
				d[k2] = e8_mp_sub(x1, x2, p);
			}
			v0 += t;
		}
		t = ht;
	}
}

/* see hawk_e8_inner.h */
void
e8_ntt_from_ntt(unsigned logn, uint32_t *a, const e8_ntt_prime *prime)
{
	size_t n = (size_t)1 << logn;
	uint32_t igm[1024];
	uint32_t p = prime->p;
	uint32_t r = (uint32_t)(UINT64_C(0x100000000) % p);
	uint32_t r2 = e8_mp_mul(r, r, p);
	uint32_t ig = e8_mp_mul(e8_mp_pow(prime->g, p - 2, p), r2, p);
	uint32_t ni = e8_mp_pow((uint32_t)n, p - 2, p);

	e8_mkgm(logn, igm, ig, p, prime->p0i);
	e8_intt(logn, a, igm, p, prime->p0i);
	for (size_t u = 0; u < n; u ++) {
		a[u] = e8_mp_mul(a[u], ni, p);
	}
}

/* see hawk_e8_inner.h */
void
e8_ntt_mul_add(unsigned logn, uint32_t *d,
	const uint32_t *a, const uint32_t *b, const e8_ntt_prime *prime)
{
	size_t n = (size_t)1 << logn;
	uint32_t p = prime->p;

	for (size_t u = 0; u < n; u ++) {
		d[u] = e8_mp_add(d[u], e8_mp_mul(a[u], b[u], p), p);
	}
}

/* see hawk_e8_inner.h */
int
e8_ntt_crt_reconstruct(int32_t *d, const uint32_t *const residues[],
	const e8_ntt_prime *const primes[], unsigned num_primes, size_t n)
{
	e8_s128 m_before[3];
	e8_s128 m_total;
	uint32_t inv_mont[3];

	if (num_primes < 2 || num_primes > 3) {
		return 0;
	}
	m_total = primes[0]->p;
	for (unsigned v = 1; v < num_primes; v ++) {
		uint32_t p = primes[v]->p;
		uint32_t r = (uint32_t)(UINT64_C(0x100000000) % p);
		uint32_t r2 = e8_mp_mul(r, r, p);
		uint32_t inv = e8_mp_pow(
			(uint32_t)(m_total % p), p - 2, p);

		m_before[v] = m_total;
		inv_mont[v] = e8_mp_montymul(inv, r2, p, primes[v]->p0i);
		m_total *= p;
	}
	for (size_t u = 0; u < n; u ++) {
		e8_s128 x = residues[0][u];

		for (unsigned v = 1; v < num_primes; v ++) {
			uint32_t p = primes[v]->p;
			uint32_t xm = (uint32_t)(x % p);
			uint32_t delta = residues[v][u] >= xm
				? residues[v][u] - xm
				: residues[v][u] + p - xm;
			uint32_t t = e8_mp_montymul(
				delta, inv_mont[v], p, primes[v]->p0i);

			x += m_before[v] * (e8_s128)t;
		}
		if (x > m_total / 2) {
			x -= m_total;
		}
		if (x < INT32_MIN || x > INT32_MAX) {
			return 0;
		}
		d[u] = (int32_t)x;
	}
	return 1;
}

/* see hawk_e8_inner.h */
void
e8_compute_keygen_qform(int32_t *q00, int32_t *q01, int32_t *q11,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, unsigned logn)
{
	/*
	 * HAWK's make_q001() technique: one NTT prime p (the verifier's
	 * first), Q = B^* S_n B entry by entry in the NTT domain, then back.
	 * The result is exact: with |f|, |g|, |F|, |G| <= 127 (int8 keys)
	 * and the S_n entries s00 = 8 + 4 X^(n/4) - 4 X^(3n/4),
	 * s01 = -s10 = 4 X^(n/2), s11 = 8, every coefficient of q00, q01 and
	 * q11 is at most n * 127 * (20 + 12) * 127 = 516128 n
	 * (5.3e8 for n = 1024) in absolute value, below p/2 (1.07e9).
	 */
	size_t n = (size_t)1 << logn;
	size_t hn = n >> 1;
	const e8_ntt_tab *tab = e8_ntt_tab_get(0);
	const uint32_t *s00 = e8_ntt_s00_mont(logn);
	const uint32_t *s01 = e8_ntt_s01_mont(logn);
	uint32_t p = tab->p;
	uint32_t p0i = tab->p0i;
	uint32_t R2 = tab->R2;
	uint32_t ni = tab->ni[logn - 8];
	uint32_t tf[E8_NTT_MAXN], tg[E8_NTT_MAXN], tF[E8_NTT_MAXN], tG[E8_NTT_MAXN];
	uint32_t *t00 = (uint32_t *)q00;
	uint32_t *t01 = (uint32_t *)q01;
	uint32_t *t11 = (uint32_t *)q11;

	for (size_t u = 0; u < n; u ++) {
		tf[u] = (uint32_t)f[u] + (p & tbmask((uint32_t)f[u]));
		tg[u] = (uint32_t)g[u] + (p & tbmask((uint32_t)g[u]));
		tF[u] = (uint32_t)F[u] + (p & tbmask((uint32_t)F[u]));
		tG[u] = (uint32_t)G[u] + (p & tbmask((uint32_t)G[u]));
	}
	e8_ntt(logn, tf, tab->gm, p, p0i);
	e8_ntt(logn, tg, tab->gm, p, p0i);
	e8_ntt(logn, tF, tab->gm, p, p0i);
	e8_ntt(logn, tG, tab->gm, p, p0i);

	/*
	 * NTT(adj(a))[u] = NTT(a)[n - 1 - u]. Per pair (u, v = n - 1 - u):
	 * (sx, sy) = S_n (x, y) at both indices, then
	 *   q00 = adj(f) sf + adj(g) sg
	 *   q01 = adj(f) sF + adj(g) sG
	 *   q11 = adj(F) sF + adj(G) sG
	 * q00 and q11 are auto-adjoint (equal NTT values at u and v).
	 * Montgomery products divide by R; the final R2 product restores
	 * plain values.
	 */
	for (size_t u = 0; u < hn; u ++) {
		size_t v = n - 1 - u;
		uint32_t fu = tf[u], fv = tf[v], gu = tg[u], gv = tg[v];
		uint32_t Fu = tF[u], Fv = tF[v], Gu = tG[u], Gv = tG[v];
		uint32_t g8u = e8_mp_add(gu, gu, p);
		uint32_t G8u = e8_mp_add(Gu, Gu, p), G8v = e8_mp_add(Gv, Gv, p);

		g8u = e8_mp_add(g8u, g8u, p);
		g8u = e8_mp_add(g8u, g8u, p);
		G8u = e8_mp_add(G8u, G8u, p);
		G8u = e8_mp_add(G8u, G8u, p);
		G8v = e8_mp_add(G8v, G8v, p);
		G8v = e8_mp_add(G8v, G8v, p);

		uint32_t sfu = e8_mp_add(e8_mp_montymul(s00[u], fu, p, p0i),
			e8_mp_montymul(s01[u], gu, p, p0i), p);
		uint32_t sgu = e8_mp_sub(g8u,
			e8_mp_montymul(s01[u], fu, p, p0i), p);
		uint32_t sFu = e8_mp_add(e8_mp_montymul(s00[u], Fu, p, p0i),
			e8_mp_montymul(s01[u], Gu, p, p0i), p);
		uint32_t sFv = e8_mp_add(e8_mp_montymul(s00[v], Fv, p, p0i),
			e8_mp_montymul(s01[v], Gv, p, p0i), p);
		uint32_t sGu = e8_mp_sub(G8u,
			e8_mp_montymul(s01[u], Fu, p, p0i), p);
		uint32_t sGv = e8_mp_sub(G8v,
			e8_mp_montymul(s01[v], Fv, p, p0i), p);

		uint32_t x00 = e8_mp_montymul(R2, e8_mp_add(
			e8_mp_montymul(fv, sfu, p, p0i),
			e8_mp_montymul(gv, sgu, p, p0i), p), p, p0i);
		uint32_t x11 = e8_mp_montymul(R2, e8_mp_add(
			e8_mp_montymul(Fv, sFu, p, p0i),
			e8_mp_montymul(Gv, sGu, p, p0i), p), p, p0i);
		t00[u] = t00[v] = x00;
		t11[u] = t11[v] = x11;
		t01[u] = e8_mp_montymul(R2, e8_mp_add(
			e8_mp_montymul(fv, sFu, p, p0i),
			e8_mp_montymul(gv, sGu, p, p0i), p), p, p0i);
		t01[v] = e8_mp_montymul(R2, e8_mp_add(
			e8_mp_montymul(fu, sFv, p, p0i),
			e8_mp_montymul(gu, sGv, p, p0i), p), p, p0i);
	}

	/*
	 * Back to coefficients: the inverse NTT returns n times the
	 * polynomial; the product with ni = R/n mod p divides by n. Values
	 * are then normalised to (-p/2, p/2).
	 */
	e8_intt(logn, t00, tab->igm, p, p0i);
	e8_intt(logn, t01, tab->igm, p, p0i);
	e8_intt(logn, t11, tab->igm, p, p0i);
	for (size_t u = 0; u < n; u ++) {
		uint32_t x00 = e8_mp_montymul(t00[u], ni, p, p0i);
		uint32_t x01 = e8_mp_montymul(t01[u], ni, p, p0i);
		uint32_t x11 = e8_mp_montymul(t11[u], ni, p, p0i);

		q00[u] = (int32_t)(x00 - (p & tbmask((p >> 1) - x00)));
		q01[u] = (int32_t)(x01 - (p & tbmask((p >> 1) - x01)));
		q11[u] = (int32_t)(x11 - (p & tbmask((p >> 1) - x11)));
	}
}

#endif
