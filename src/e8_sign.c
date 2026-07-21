#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <assert.h>
#include <limits.h>
#include <time.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#define E8_MAXN   1024
#define E8_DEBUG_INVERSE_CHECK_ATTEMPTS   4
#define E8_INVERSE_W_ABS_LIMIT   INT64_C(140000000)

static uint64_t
trace_wall_ns(void)
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
trace_cycles_start(void)
{
#if defined(__x86_64__)
	_mm_lfence();
	return __rdtsc();
#else
	return 0;
#endif
}

static uint64_t
trace_cycles_end(void)
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
trace_cycles_delta(uint64_t t0, uint64_t t1)
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
trace_wall_delta(uint64_t t0, uint64_t t1)
{
	if (t0 == 0 && t1 == 0) {
		return 0;
	}
	if (t1 <= t0) {
		return 1;
	}
	return t1 - t0;
}

#if HAWK_E8_PROFILE_SIGN
static void
sign_profile_stage_start(uint64_t *cycles_start, uint64_t *wall_start)
{
	*cycles_start = trace_cycles_start();
	*wall_start = trace_wall_ns();
}

static void
sign_profile_stage_add(uint64_t *cycles_total, uint64_t *wall_total,
	uint64_t cycles_start, uint64_t wall_start)
{
	uint64_t wall_end = trace_wall_ns();
	uint64_t cycles_end = trace_cycles_end();

	*cycles_total += trace_cycles_delta(cycles_start, cycles_end);
	*wall_total += trace_wall_delta(wall_start, wall_end);
}

static void
sign_profile_finish(e8_sign_trace_timing *trace_timing,
	uint64_t cycles_start, uint64_t wall_start)
{
	if (trace_timing != NULL && (cycles_start != 0 || wall_start != 0)) {
		uint64_t wall_end = trace_wall_ns();
		uint64_t cycles_end = trace_cycles_end();

		trace_timing->cycles_sign_total +=
			trace_cycles_delta(cycles_start, cycles_end);
		trace_timing->wall_ns_sign_total +=
			trace_wall_delta(wall_start, wall_end);
	}
}
#endif

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
	int64_t acc[E8_MAXN];

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

static int
dummy_sample_z(int32_t *z0, int32_t *z1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *h0, const int32_t *h1,
	const int32_t *r0, const int32_t *r1, size_t n)
{
	int32_t tmp0[E8_MAXN], tmp1[E8_MAXN];

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
compute_inverse_w_schoolbook(int64_t *w0, int64_t *w1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *z0, const int32_t *z1, size_t n)
{
	memset(w0, 0, n * sizeof *w0);
	memset(w1, 0, n * sizeof *w1);

	/* B^{-1} = [ G -F ; -g f ] for B = [ f F ; g G ]. */
	poly_mul_i8_i32_add_i64(w0, G, z0, n, 1);
	poly_mul_i8_i32_add_i64(w0, F, z1, n, -1);
	poly_mul_i8_i32_add_i64(w1, g, z0, n, -1);
	poly_mul_i8_i32_add_i64(w1, f, z1, n, 1);
}

static void
i8_to_ntt(unsigned logn, uint32_t *d,
	const int8_t *a, const e8_ntt_prime *prime, const uint32_t *gm)
{
	size_t n = (size_t)1 << logn;
	int32_t tmp[E8_MAXN];

	for (size_t u = 0; u < n; u ++) {
		tmp[u] = a[u];
	}
	for (size_t u = 0; u < n; u ++) {
		d[u] = e8_mp_set_i32(tmp[u], prime->p);
	}
	e8_ntt(logn, d, gm, prime->p, prime->p0i);
}

static void
ntt_to_monty(unsigned logn, uint32_t *a, const e8_ntt_prime *prime)
{
	size_t n = (size_t)1 << logn;
	uint32_t p = prime->p;
	uint32_t r = (uint32_t)(UINT64_C(0x100000000) % p);
	uint32_t r2 = e8_mp_mul(r, r, p);

	for (size_t u = 0; u < n; u ++) {
		a[u] = e8_mp_montymul(a[u], r2, p, prime->p0i);
	}
}

static void
i32_to_ntt_with_gm(unsigned logn, uint32_t *d,
	const int32_t *a, const e8_ntt_prime *prime, const uint32_t *gm)
{
	size_t n = (size_t)1 << logn;

	for (size_t u = 0; u < n; u ++) {
		d[u] = e8_mp_set_i32(a[u], prime->p);
	}
	e8_ntt(logn, d, gm, prime->p, prime->p0i);
}

static void
intt_with_igm(unsigned logn, uint32_t *a,
	const e8_ntt_prime *prime, const uint32_t *igm, uint32_t ni)
{
	size_t n = (size_t)1 << logn;

	e8_intt(logn, a, igm, prime->p, prime->p0i);
	for (size_t u = 0; u < n; u ++) {
		a[u] = e8_mp_mul(a[u], ni, prime->p);
	}
}

/* see hawk_e8_inner.h */
void
e8_inverse_w_ntt_prepare(e8_inverse_w_ntt_basis *basis,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, unsigned logn)
{
	if (basis == NULL) {
		return;
	}
	basis->magic = 0;
	basis->logn = 0;
	if (logn < 8 || logn > 10 || f == NULL || g == NULL
		|| F == NULL || G == NULL)
	{
		return;
	}
#if HAWK_E8_DEBUG_CHECKS
	basis->basis_digest = e8_basis_debug_digest(
		f, g, F, G, (size_t)1 << logn);
#endif
	for (unsigned u = 0; u < E8_INVERSE_W_NTT_PRIMES; u ++) {
		const e8_ntt_prime *prime = &E8_NTT_PRIMES[u];
		uint32_t p = prime->p;
		uint32_t r = (uint32_t)(UINT64_C(0x100000000) % p);
		uint32_t r2 = e8_mp_mul(r, r, p);
		uint32_t ig = e8_mp_mul(e8_mp_pow(prime->g, p - 2, p), r2, p);

		e8_mkgm(logn, basis->gm[u], prime->g, p, prime->p0i);
		e8_mkgm(logn, basis->igm[u], ig, p, prime->p0i);
		basis->ni[u] = e8_mp_pow((uint32_t)1 << logn, p - 2, p);
		i8_to_ntt(logn, basis->f[u], f, prime, basis->gm[u]);
		i8_to_ntt(logn, basis->g[u], g, prime, basis->gm[u]);
		i8_to_ntt(logn, basis->F[u], F, prime, basis->gm[u]);
		i8_to_ntt(logn, basis->G[u], G, prime, basis->gm[u]);
		/*
		 * Forward NTT inputs/outputs are standard residues; gm/igm
		 * entries are Montgomery roots.  Store only the fixed basis
		 * transforms as Montgomery residues here.  Per-attempt z
		 * transforms remain standard, so montymul(basis_monty, z)
		 * returns a standard pointwise product for the inverse NTT.
		 */
		ntt_to_monty(logn, basis->f[u], prime);
		ntt_to_monty(logn, basis->g[u], prime);
		ntt_to_monty(logn, basis->F[u], prime);
		ntt_to_monty(logn, basis->G[u], prime);
	}
	basis->logn = logn;
	basis->magic = E8_INVERSE_W_NTT_MAGIC;
}

static uint32_t
mp_sub(uint32_t a, uint32_t b, uint32_t p)
{
	return a >= b ? a - b : a + p - b;
}

static int
center_checked_single_prime(int64_t *d, const uint32_t *a, size_t n,
	uint32_t p)
{
	for (size_t u = 0; u < n; u ++) {
		int64_t x = a[u] > (p >> 1)
			? (int64_t)a[u] - (int64_t)p
			: (int64_t)a[u];
		uint64_t ax = x < 0 ? (uint64_t)(-x) : (uint64_t)x;
		int in_range = ax < (uint64_t)E8_INVERSE_W_ABS_LIMIT;

		assert(in_range);
		if (!in_range) {
			return 0;
		}
		d[u] = x;
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_compute_inverse_w_ntt(int64_t *w0, int64_t *w1,
	const e8_inverse_w_ntt_basis *basis,
	const int32_t *z0, const int32_t *z1, unsigned logn)
{
	if (logn < 8 || logn > 10 || w0 == NULL || w1 == NULL
		|| z0 == NULL || z1 == NULL || basis == NULL
		|| basis->magic != E8_INVERSE_W_NTT_MAGIC
		|| basis->logn != logn)
	{
		return 0;
	}
	size_t n = (size_t)1 << logn;
	const e8_ntt_prime *prime = &E8_NTT_PRIMES[0];
	uint32_t p = prime->p;
	uint32_t tz0[E8_MAXN], tz1[E8_MAXN];
	uint32_t rw0[E8_MAXN], rw1[E8_MAXN];

	/*
	 * Reconstruction is called only after pnorm <= 31150, the largest
	 * supported verify threshold (n=1024, sigma_verify=1.95).  Thus each
	 * product-coordinate x has |x| <= floor(sqrt(31150)) = 176.  Every
	 * row of 4*P^{-1} has L1 norm at most 12, hence |z| <= 3*176 = 528.
	 * Each coefficient of B^{-1}z is the sum of two n-term products, so
	 * the int8 basis bound gives |w| <= 2*1024*128*528 = 138412032.
	 * E8_INVERSE_W_ABS_LIMIT adds a small margin and remains far below
	 * the centered interval of p0, making one-prime reconstruction exact.
	 */
	i32_to_ntt_with_gm(logn, tz0, z0, prime, basis->gm[0]);
	i32_to_ntt_with_gm(logn, tz1, z1, prime, basis->gm[0]);
	for (size_t u = 0; u < n; u ++) {
		uint32_t gz0 = e8_mp_montymul(
			basis->G[0][u], tz0[u], p, prime->p0i);
		uint32_t fz1 = e8_mp_montymul(
			basis->f[0][u], tz1[u], p, prime->p0i);
		uint32_t fz1_big = e8_mp_montymul(
			basis->F[0][u], tz1[u], p, prime->p0i);
		uint32_t gz0_small = e8_mp_montymul(
			basis->g[0][u], tz0[u], p, prime->p0i);

		rw0[u] = mp_sub(gz0, fz1_big, p);
		rw1[u] = mp_sub(fz1, gz0_small, p);
	}
	intt_with_igm(logn, rw0, prime, basis->igm[0], basis->ni[0]);
	intt_with_igm(logn, rw1, prime, basis->igm[0], basis->ni[0]);
	if (!center_checked_single_prime(w0, rw0, n, p)
		|| !center_checked_single_prime(w1, rw1, n, p))
	{
		return 0;
	}
	return 1;
}

static int
sym_break_i64(const int64_t *w0, const int64_t *w1, size_t n)
{
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

static int
negate_i64_pair(int64_t *w0, int64_t *w1, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		if (w0[u] == INT64_MIN || w1[u] == INT64_MIN) {
			return 0;
		}
	}
	for (size_t u = 0; u < n; u ++) {
		w0[u] = -w0[u];
		w1[u] = -w1[u];
	}
	return 1;
}

static int
compute_s_from_w(int16_t *s0, int16_t *s1,
	const uint8_t *h0b, const uint8_t *h1b,
	int64_t *w0, int64_t *w1, size_t n)
{
	if (!sym_break_i64(w0, w1, n)
		&& !negate_i64_pair(w0, w1, n))
	{
		return -1;
	}

	for (size_t u = 0; u < n; u ++) {
		int64_t x0 = (int64_t)h0b[u] - w0[u];
		int64_t x1 = (int64_t)h1b[u] - w1[u];
		int w_ok = ((((uint64_t)w0[u]) & 1u) == h0b[u])
			&& ((((uint64_t)w1[u]) & 1u) == h1b[u])
			&& (x0 % 2) == 0 && (x1 % 2) == 0;

		assert(w_ok);
		if (!w_ok) {
			return -1;
		}
		x0 /= 2;
		x1 /= 2;
		if (x0 < INT16_MIN || x0 > INT16_MAX
			|| x1 < INT16_MIN || x1 > INT16_MAX)
		{
			return 0;
		}
		s0[u] = (int16_t)x0;
		s1[u] = (int16_t)x1;
	}

	return 1;
}

static int
compute_s_from_sample(int16_t *s0, int16_t *s1,
	const uint8_t *t0, const uint8_t *t1,
	const uint8_t *h0b, const uint8_t *h1b,
	const e8_inverse_w_ntt_basis *basis_ntt,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *z0, const int32_t *z1,
	unsigned logn, size_t n, unsigned attempt)
{
	int64_t w0[E8_MAXN], w1[E8_MAXN];
#if HAWK_E8_DEBUG_CHECKS
	int64_t sw0[E8_MAXN], sw1[E8_MAXN];
#else
	(void)f;
	(void)g;
	(void)F;
	(void)G;
	(void)attempt;
#endif

	for (size_t u = 0; u < n; u ++) {
		int z_ok = ((((uint32_t)z0[u]) & 1u) == t0[u])
			&& ((((uint32_t)z1[u]) & 1u) == t1[u]);
		assert(z_ok);
		if (!z_ok) {
			return -1;
		}
	}

	if (!e8_compute_inverse_w_ntt(w0, w1, basis_ntt, z0, z1, logn)) {
		return -1;
	}
#if HAWK_E8_DEBUG_CHECKS
	if (attempt < E8_DEBUG_INVERSE_CHECK_ATTEMPTS) {
		compute_inverse_w_schoolbook(sw0, sw1,
			f, g, F, G, z0, z1, n);
		for (size_t u = 0; u < n; u ++) {
			int w_ok = w0[u] == sw0[u] && w1[u] == sw1[u];

			assert(w_ok);
			if (!w_ok) {
				return -1;
			}
		}
	}
#endif
	return compute_s_from_w(s0, s1, h0b, h1b, w0, w1, n);
}

/* see hawk_e8_inner.h */
int
e8_sign_dummy_offset_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	const int32_t *r0, const int32_t *r1)
{
	size_t salt_len = e8_salt_len(logn);
	uint8_t h[256], h0b[E8_MAXN], h1b[E8_MAXN];
	uint8_t t0[E8_MAXN], t1[E8_MAXN];
	int32_t h0[E8_MAXN], h1[E8_MAXN], z0[E8_MAXN], z1[E8_MAXN];
	int64_t w0[E8_MAXN], w1[E8_MAXN];
	int16_t s0[E8_MAXN], s1[E8_MAXN];
	int sr;

	if (salt_len == 0 || sig_len != e8_sig_uncompressed_size(logn)
		|| hpub_len != ((size_t)1 << (logn - 4))
		|| sig == NULL || sc_data == NULL || hpub == NULL
		|| f == NULL || g == NULL || F == NULL || G == NULL
		|| salt == NULL)
	{
		return 0;
	}

	size_t n = (size_t)1 << logn;
	hash_to_h(logn, h, sc_data, hpub, salt, salt_len);
	bits_to_poly(h0, h0b, h, 0, n);
	bits_to_poly(h1, h1b, h, n, n);

	e8_compute_t_mod2(t0, t1, f, g, F, G, h0b, h1b, n);

	/*
	 * Dummy sampler path only: z = B h + 2*r.  This gives the right
	 * internal coset and lets the signer/verifier algebra be exercised,
	 * but it is not distributed like the target E8 Gaussian.
	 */
	if (!dummy_sample_z(z0, z1, f, g, F, G, h0, h1, r0, r1, n)) {
		return 0;
	}

	for (size_t u = 0; u < n; u ++) {
		assert((((uint32_t)z0[u]) & 1u) == t0[u]);
		assert((((uint32_t)z1[u]) & 1u) == t1[u]);
	}

	compute_inverse_w_schoolbook(w0, w1, f, g, F, G, z0, z1, n);
	sr = compute_s_from_w(s0, s1, h0b, h1b, w0, w1, n);
	if (sr != 1) {
		return 0;
	}

	return e8_encode_sig_uncompressed(logn, sig, sig_len, salt, s0, s1);
}

/* see hawk_e8_inner.h */
int
e8_sign_dummy_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt)
{
	return e8_sign_dummy_offset_uncompressed(logn,
		sig, sig_len, sc_data, hpub, hpub_len,
		f, g, F, G, salt, NULL, NULL);
}

/* see hawk_e8_inner.h */
int
e8_sign_sampler_trace_timed_uncompressed_prepared(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G,
	const e8_inverse_w_ntt_basis *basis_ntt,
	const e8_coset_f2_basis *basis_f2, const uint8_t *salt,
	double sigma_sign, double sigma_verify,
	unsigned max_attempts, hawk_rng rng, void *rng_context,
	int32_t *trace_z0, int32_t *trace_z1,
	int64_t *trace_pnorm, unsigned *trace_attempts,
	e8_sign_trace_timing *trace_timing)
{
	size_t salt_len = e8_salt_len(logn);
	uint8_t h[256], h0b[E8_MAXN], h1b[E8_MAXN];
	uint8_t t0[E8_MAXN], t1[E8_MAXN];
	int32_t z0[E8_MAXN], z1[E8_MAXN];
	int16_t s0[E8_MAXN], s1[E8_MAXN];
	int64_t threshold;
	size_t n;
#if HAWK_E8_PROFILE_SIGN
	uint64_t sign_c0 = 0, sign_w0 = 0;
	uint64_t stage_c0 = 0, stage_w0 = 0;
#endif

	if (trace_timing != NULL) {
		memset(trace_timing, 0, sizeof *trace_timing);
	}

	/*
	 * Experimental HAWK-E8-CM sampler path.  This calls the
	 * coset-matched Construction-A CM sampler, which returns internal
	 * P-coordinate samples z in 2R^2 + t and measures ||P_n z||^2.  This
	 * path is floating-point, data-dependent, not constant-time, and not
	 * parameter-calibrated.  It is separate from the dummy signing helpers
	 * above and from all ordinary HAWK signing.
	 */
	if (logn < 8 || logn > 10 || salt_len == 0
		|| sig_len != e8_sig_uncompressed_size(logn)
		|| hpub_len != ((size_t)1 << (logn - 4))
		|| sig == NULL || sc_data == NULL || hpub == NULL
		|| f == NULL || g == NULL || F == NULL || G == NULL
		|| basis_ntt == NULL || basis_f2 == NULL || salt == NULL
		|| basis_ntt->magic != E8_INVERSE_W_NTT_MAGIC
		|| basis_f2->magic != E8_COSET_F2_MAGIC
		|| basis_ntt->logn != logn || basis_f2->logn != logn
		|| sigma_sign <= 0.0 || sigma_verify <= 0.0
		|| max_attempts == 0 || rng == NULL)
	{
		return 0;
	}
	n = (size_t)1 << logn;
#if HAWK_E8_DEBUG_CHECKS
	{
		uint64_t digest = e8_basis_debug_digest(f, g, F, G, n);
		int basis_ok = basis_ntt->basis_digest == digest
			&& basis_f2->basis_digest == digest;

		assert(basis_ok);
		if (!basis_ok) {
			return 0;
		}
	}
#endif
#if HAWK_E8_PROFILE_SIGN
	if (trace_timing != NULL) {
		sign_profile_stage_start(&sign_c0, &sign_w0);
	}
#endif
	if (!e8_verify_bound_from_sigma(logn, sigma_verify, &threshold)) {
#if HAWK_E8_PROFILE_SIGN
		sign_profile_finish(trace_timing, sign_c0, sign_w0);
#endif
		return 0;
	}

#if HAWK_E8_PROFILE_SIGN
	if (trace_timing != NULL) {
		sign_profile_stage_start(&stage_c0, &stage_w0);
	}
#endif
	hash_to_h(logn, h, sc_data, hpub, salt, salt_len);
	bits_to_poly(z0, h0b, h, 0, n);
	bits_to_poly(z1, h1b, h, n, n);
#if HAWK_E8_PROFILE_SIGN
	if (trace_timing != NULL) {
		sign_profile_stage_add(&trace_timing->cycles_hash_total,
			&trace_timing->wall_ns_hash_total,
			stage_c0, stage_w0);
		sign_profile_stage_start(&stage_c0, &stage_w0);
	}
#endif
	e8_compute_t_mod2_prepared(t0, t1, basis_f2, h0b, h1b, n);
#if HAWK_E8_PROFILE_SIGN
	if (trace_timing != NULL) {
		sign_profile_stage_add(&trace_timing->cycles_target_total,
			&trace_timing->wall_ns_target_total,
			stage_c0, stage_w0);
	}
#endif
	for (unsigned attempt = 0; attempt < max_attempts; attempt ++) {
		uint64_t sample_c0, sample_c1, sample_w0, sample_w1;
		uint64_t pnorm_u;
		int64_t pnorm;
		int sr;
		int rejected;

#if HAWK_E8_PROFILE_SIGN
		if (trace_timing != NULL) {
			trace_timing->attempts_total ++;
		}
#endif
		sample_c0 = trace_cycles_start();
		sample_w0 = trace_wall_ns();
		int sample_ok = e8_sample_z_construction_a_cm(z0, z1, &pnorm_u,
			t0, t1, logn, sigma_sign, rng, rng_context, NULL);
		sample_w1 = trace_wall_ns();
		sample_c1 = trace_cycles_end();
		if (trace_timing != NULL) {
			uint64_t sample_cycles =
				trace_cycles_delta(sample_c0, sample_c1);
			uint64_t sample_wall =
				trace_wall_delta(sample_w0, sample_w1);

			trace_timing->cycles_sample_total += sample_cycles;
			trace_timing->cycles_sample_last = sample_cycles;
			trace_timing->wall_ns_sample_total += sample_wall;
			trace_timing->wall_ns_sample_last = sample_wall;
		}
		if (!sample_ok) {
#if HAWK_E8_PROFILE_SIGN
			sign_profile_finish(trace_timing, sign_c0, sign_w0);
#endif
			return 0;
		}
		if (pnorm_u > (uint64_t)INT64_MAX) {
#if HAWK_E8_PROFILE_SIGN
			sign_profile_finish(trace_timing, sign_c0, sign_w0);
#endif
			return 0;
		}
		pnorm = (int64_t)pnorm_u;
#if HAWK_E8_PROFILE_SIGN
		if (trace_timing != NULL) {
			sign_profile_stage_start(&stage_c0, &stage_w0);
		}
#endif
		rejected = pnorm > threshold;
#if HAWK_E8_PROFILE_SIGN
		if (trace_timing != NULL) {
			sign_profile_stage_add(
				&trace_timing->cycles_norm_check_total,
				&trace_timing->wall_ns_norm_check_total,
				stage_c0, stage_w0);
			if (rejected) {
				trace_timing->rejections_total ++;
			}
		}
#endif
		if (rejected) {
			continue;
		}
#if HAWK_E8_PROFILE_SIGN
		if (trace_timing != NULL) {
			sign_profile_stage_start(&stage_c0, &stage_w0);
		}
#endif
		sr = compute_s_from_sample(s0, s1, t0, t1, h0b, h1b,
			basis_ntt, f, g, F, G, z0, z1,
			logn, n, attempt);
#if HAWK_E8_PROFILE_SIGN
		if (trace_timing != NULL) {
			sign_profile_stage_add(
				&trace_timing->cycles_reconstruct_total,
				&trace_timing->wall_ns_reconstruct_total,
				stage_c0, stage_w0);
		}
#endif
		if (sr < 0) {
#if HAWK_E8_PROFILE_SIGN
			sign_profile_finish(trace_timing, sign_c0, sign_w0);
#endif
			return 0;
		}
#if HAWK_E8_PROFILE_SIGN
		if (trace_timing != NULL) {
			sign_profile_stage_start(&stage_c0, &stage_w0);
		}
#endif
		rejected = sr == 0;
#if HAWK_E8_PROFILE_SIGN
		if (trace_timing != NULL) {
			sign_profile_stage_add(
				&trace_timing->cycles_norm_check_total,
				&trace_timing->wall_ns_norm_check_total,
				stage_c0, stage_w0);
			if (rejected) {
				trace_timing->rejections_total ++;
			}
		}
#endif
		if (rejected) {
			continue;
		}
#if HAWK_E8_PROFILE_SIGN
		if (trace_timing != NULL) {
			sign_profile_stage_start(&stage_c0, &stage_w0);
		}
#endif
		int encode_ok = e8_encode_sig_uncompressed(logn,
			sig, sig_len, salt, s0, s1);
#if HAWK_E8_PROFILE_SIGN
		if (trace_timing != NULL) {
			sign_profile_stage_add(&trace_timing->cycles_encode_total,
				&trace_timing->wall_ns_encode_total,
				stage_c0, stage_w0);
		}
#endif
		if (!encode_ok) {
#if HAWK_E8_PROFILE_SIGN
			sign_profile_finish(trace_timing, sign_c0, sign_w0);
#endif
			return 0;
		}
		if (trace_z0 != NULL) {
			memcpy(trace_z0, z0, n * sizeof *trace_z0);
		}
		if (trace_z1 != NULL) {
			memcpy(trace_z1, z1, n * sizeof *trace_z1);
		}
		if (trace_pnorm != NULL) {
			*trace_pnorm = pnorm;
		}
		if (trace_attempts != NULL) {
			*trace_attempts = attempt + 1;
		}
#if HAWK_E8_PROFILE_SIGN
		sign_profile_finish(trace_timing, sign_c0, sign_w0);
#endif
		return 1;
	}

	if (trace_attempts != NULL) {
		*trace_attempts = max_attempts;
	}
#if HAWK_E8_PROFILE_SIGN
	sign_profile_finish(trace_timing, sign_c0, sign_w0);
#endif
	return 0;
}

/* see hawk_e8_inner.h */
int
e8_sign_sampler_trace_timed_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify,
	unsigned max_attempts, hawk_rng rng, void *rng_context,
	int32_t *trace_z0, int32_t *trace_z1,
	int64_t *trace_pnorm, unsigned *trace_attempts,
	e8_sign_trace_timing *trace_timing)
{
	e8_inverse_w_ntt_basis basis_ntt;
	e8_coset_f2_basis basis_f2;

	if (trace_timing != NULL) {
		memset(trace_timing, 0, sizeof *trace_timing);
	}
	if (logn < 8 || logn > 10
		|| f == NULL || g == NULL || F == NULL || G == NULL)
	{
		return 0;
	}
	e8_inverse_w_ntt_prepare(&basis_ntt, f, g, F, G, logn);
	e8_coset_f2_prepare(&basis_f2, f, g, F, G, (size_t)1 << logn);
	return e8_sign_sampler_trace_timed_uncompressed_prepared(logn,
		sig, sig_len, sc_data, hpub, hpub_len,
		f, g, F, G, &basis_ntt, &basis_f2, salt,
		sigma_sign, sigma_verify, max_attempts, rng, rng_context,
		trace_z0, trace_z1, trace_pnorm, trace_attempts,
		trace_timing);
}

/* see hawk_e8_inner.h */
int
e8_sign_sampler_trace_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify,
	unsigned max_attempts, hawk_rng rng, void *rng_context,
	int32_t *trace_z0, int32_t *trace_z1,
	int64_t *trace_pnorm, unsigned *trace_attempts)
{
	return e8_sign_sampler_trace_timed_uncompressed(logn,
		sig, sig_len, sc_data, hpub, hpub_len,
		f, g, F, G, salt, sigma_sign, sigma_verify,
		max_attempts, rng, rng_context, trace_z0, trace_z1,
		trace_pnorm, trace_attempts, NULL);
}

/* see hawk_e8_inner.h */
int
e8_sign_sampler_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify,
	unsigned max_attempts, hawk_rng rng, void *rng_context)
{
	return e8_sign_sampler_trace_uncompressed(logn,
		sig, sig_len, sc_data, hpub, hpub_len, f, g, F, G, salt,
		sigma_sign, sigma_verify, max_attempts,
		rng, rng_context, NULL, NULL, NULL, NULL);
}

#endif
