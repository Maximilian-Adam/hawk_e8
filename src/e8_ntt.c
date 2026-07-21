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

#endif
