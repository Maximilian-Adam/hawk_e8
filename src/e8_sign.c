#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <assert.h>
#include <limits.h>

#define E8_MAXN   1024

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

static void
compute_t_mod2(uint8_t *t0, uint8_t *t1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const uint8_t *h0, const uint8_t *h1, size_t n)
{
	memset(t0, 0, E8_MAXN);
	memset(t1, 0, E8_MAXN);
	poly_mul_mod2_add(t0, f, h0, n);
	poly_mul_mod2_add(t0, F, h1, n);
	poly_mul_mod2_add(t1, g, h0, n);
	poly_mul_mod2_add(t1, G, h1, n);
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
compute_inverse_w(int64_t *w0, int64_t *w1,
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
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *z0, const int32_t *z1, size_t n)
{
	int64_t w0[E8_MAXN], w1[E8_MAXN];

	for (size_t u = 0; u < n; u ++) {
		int z_ok = ((((uint32_t)z0[u]) & 1u) == t0[u])
			&& ((((uint32_t)z1[u]) & 1u) == t1[u]);
		assert(z_ok);
		if (!z_ok) {
			return -1;
		}
	}

	compute_inverse_w(w0, w1, f, g, F, G, z0, z1, n);
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

	compute_t_mod2(t0, t1, f, g, F, G, h0b, h1b, n);

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

	compute_inverse_w(w0, w1, f, g, F, G, z0, z1, n);
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
e8_sign_sampler_trace_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify, int sampler_bound,
	unsigned max_attempts, hawk_rng rng, void *rng_context,
	int32_t *trace_z0, int32_t *trace_z1,
	int64_t *trace_pnorm, unsigned *trace_attempts)
{
	size_t salt_len = e8_salt_len(logn);
	uint8_t h[256], h0b[E8_MAXN], h1b[E8_MAXN];
	uint8_t t0[E8_MAXN], t1[E8_MAXN];
	int32_t z0[E8_MAXN], z1[E8_MAXN];
	int16_t s0[E8_MAXN], s1[E8_MAXN];
	int64_t threshold;

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
		|| salt == NULL || sigma_sign <= 0.0 || sigma_verify <= 0.0
		|| sampler_bound < 1 || max_attempts == 0 || rng == NULL)
	{
		return 0;
	}
	if (!e8_verify_bound_from_sigma(logn, sigma_verify, &threshold)) {
		return 0;
	}

	size_t n = (size_t)1 << logn;
	hash_to_h(logn, h, sc_data, hpub, salt, salt_len);
	bits_to_poly(z0, h0b, h, 0, n);
	bits_to_poly(z1, h1b, h, n, n);
	compute_t_mod2(t0, t1, f, g, F, G, h0b, h1b, n);

	for (unsigned attempt = 0; attempt < max_attempts; attempt ++) {
		uint64_t pnorm_u;
		int64_t pnorm;
		int sr;

		if (!e8_sample_z_construction_a_cm(z0, z1, &pnorm_u,
			t0, t1, logn, sigma_sign, rng, rng_context, NULL))
		{
			return 0;
		}
		if (pnorm_u > (uint64_t)INT64_MAX) {
			return 0;
		}
		pnorm = (int64_t)pnorm_u;
		sr = compute_s_from_sample(s0, s1, t0, t1, h0b, h1b,
			f, g, F, G, z0, z1, n);
		if (sr < 0) {
			return 0;
		}
		if (sr == 0 || pnorm > threshold) {
			continue;
		}
		if (!e8_encode_sig_uncompressed(logn,
			sig, sig_len, salt, s0, s1))
		{
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
		return 1;
	}

	if (trace_attempts != NULL) {
		*trace_attempts = max_attempts;
	}
	return 0;
}

/* see hawk_e8_inner.h */
int
e8_sign_sampler_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify, int sampler_bound,
	unsigned max_attempts, hawk_rng rng, void *rng_context)
{
	return e8_sign_sampler_trace_uncompressed(logn,
		sig, sig_len, sc_data, hpub, hpub_len, f, g, F, G, salt,
		sigma_sign, sigma_verify, sampler_bound,
		max_attempts, rng, rng_context, NULL, NULL, NULL, NULL);
}

#endif
