#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <assert.h>
#include <math.h>

#define E8_SAMPLER_MAX_BOUND   16
#define E8_SAMPLER_MAX_VALUES  (2 * E8_SAMPLER_MAX_BOUND + 1)

/*
 * Experimental E8 bounded sampler.
 *
 * This file is intentionally a standalone research component.  It uses
 * floating-point weights, bounded/truncated support, data-dependent control
 * flow, and no side-channel hardening.  It is not suitable for final
 * cryptographic deployment.  The experimental sampler-backed E8 signer calls
 * this component explicitly; ordinary HAWK signing never uses it.
 */

static int32_t
block_shift_get(const int32_t *a, unsigned u, unsigned shift)
{
	assert(shift < 4);
	if (shift == 0) {
		return a[u];
	}
	if (u >= shift) {
		return a[u - shift];
	}
	return -a[4 + u - shift];
}

static uint64_t
rng_u64(hawk_rng rng, void *rng_context)
{
	uint8_t buf[8];
	uint64_t x = 0;

	rng(rng_context, buf, sizeof buf);
	for (unsigned u = 0; u < 8; u ++) {
		x |= (uint64_t)buf[u] << (u << 3);
	}
	return x;
}

static double
rng_double(hawk_rng rng, void *rng_context)
{
	uint64_t x = rng_u64(rng, rng_context);

	return (double)(x >> 11) * 0x1.0p-53;
}

static int
make_coord_values(int vals[8][E8_SAMPLER_MAX_VALUES],
	int lens[8], uint8_t tau, int bound)
{
	if (bound < 1 || bound > E8_SAMPLER_MAX_BOUND) {
		return 0;
	}

	for (unsigned u = 0; u < 8; u ++) {
		unsigned bit = (tau >> u) & 1u;
		lens[u] = 0;
		for (int v = -bound; v <= bound; v ++) {
			if ((((uint32_t)v) & 1u) == bit) {
				vals[u][lens[u] ++] = v;
			}
		}
		if (lens[u] == 0) {
			return 0;
		}
	}
	return 1;
}

static double
candidate_weight(const int32_t *zblk, double inv_2sigma2)
{
	int64_t norm2 = e8_block_norm2(zblk);

	return exp(-(double)norm2 * inv_2sigma2);
}

static double
enumerate_total_weight(unsigned depth, int32_t *zblk,
	const int vals[8][E8_SAMPLER_MAX_VALUES], const int lens[8],
	double inv_2sigma2)
{
	if (depth == 8) {
		return candidate_weight(zblk, inv_2sigma2);
	}

	double total = 0.0;
	for (int u = 0; u < lens[depth]; u ++) {
		zblk[depth] = vals[depth][u];
		total += enumerate_total_weight(depth + 1,
			zblk, vals, lens, inv_2sigma2);
	}
	return total;
}

static int
enumerate_pick(unsigned depth, int32_t *zblk, int32_t *out,
	const int vals[8][E8_SAMPLER_MAX_VALUES], const int lens[8],
	double inv_2sigma2, double target, double *cdf)
{
	if (depth == 8) {
		double w = candidate_weight(zblk, inv_2sigma2);
		*cdf += w;
		if (w > 0.0 && *cdf >= target) {
			memcpy(out, zblk, 8 * sizeof *out);
			return 1;
		}
		return 0;
	}

	for (int u = 0; u < lens[depth]; u ++) {
		zblk[depth] = vals[depth][u];
		if (enumerate_pick(depth + 1, zblk, out,
			vals, lens, inv_2sigma2, target, cdf))
		{
			return 1;
		}
	}
	return 0;
}

/* see hawk_e8_inner.h */
uint8_t
e8_extract_tau(const uint8_t *t0, const uint8_t *t1,
	size_t r, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t k = (size_t)1 << (logn - 2);
	uint8_t tau = 0;
	for (unsigned u = 0; u < 4; u ++) {
		tau |= (uint8_t)((t0[r + u * k] & 1u) << u);
		tau |= (uint8_t)((t1[r + u * k] & 1u) << (u + 4));
	}
	return tau;
}

/* see hawk_e8_inner.h */
void
e8_write_block(int32_t *z0, int32_t *z1,
	size_t r, const int32_t *zblk, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t k = (size_t)1 << (logn - 2);
	for (unsigned u = 0; u < 4; u ++) {
		z0[r + u * k] = zblk[u];
		z1[r + u * k] = zblk[u + 4];
	}
}

/* see hawk_e8_inner.h */
void
e8_read_block(int32_t *zblk,
	const int32_t *z0, const int32_t *z1, size_t r, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t k = (size_t)1 << (logn - 2);
	for (unsigned u = 0; u < 4; u ++) {
		zblk[u] = z0[r + u * k];
		zblk[u + 4] = z1[r + u * k];
	}
}

/* see hawk_e8_inner.h */
void
e8_block_apply_P(int32_t *xblk, const int32_t *zblk)
{
	const int32_t *z0 = zblk;
	const int32_t *z1 = zblk + 4;

	for (unsigned u = 0; u < 4; u ++) {
		int32_t z0_y0 = z0[u];
		int32_t z0_y1 = block_shift_get(z0, u, 1);
		int32_t z1_y0 = z1[u];
		int32_t z1_y1 = block_shift_get(z1, u, 1);
		int32_t z1_y2 = block_shift_get(z1, u, 2);
		int32_t z1_y3 = block_shift_get(z1, u, 3);

		xblk[u] = 2 * (z0_y0 + z0_y1)
			+ z1_y0 - z1_y1 + z1_y2 + z1_y3;
		xblk[u + 4] = z1_y0 + z1_y1 + z1_y2 + z1_y3;
	}
}

/* see hawk_e8_inner.h */
int64_t
e8_block_norm2(const int32_t *zblk)
{
	int32_t xblk[8];
	int64_t norm2 = 0;

	e8_block_apply_P(xblk, zblk);
	for (unsigned u = 0; u < 8; u ++) {
		norm2 += (int64_t)xblk[u] * xblk[u];
	}
	return norm2;
}

/* see hawk_e8_inner.h */
int
e8_sample_block_float(int32_t *zblk, uint8_t tau,
	double sigma, int bound, hawk_rng rng, void *rng_context)
{
	int vals[8][E8_SAMPLER_MAX_VALUES];
	int lens[8];
	int32_t cur[8], last[8];

	if (zblk == NULL || sigma <= 0.0 || rng == NULL
		|| !make_coord_values(vals, lens, tau, bound))
	{
		return 0;
	}

	double inv_2sigma2 = 1.0 / (2.0 * sigma * sigma);
	double total = enumerate_total_weight(0, cur, vals, lens, inv_2sigma2);
	if (!(total > 0.0)) {
		return 0;
	}

	double target = rng_double(rng, rng_context) * total;
	double cdf = 0.0;
	if (enumerate_pick(0, cur, zblk,
		vals, lens, inv_2sigma2, target, &cdf))
	{
		return 1;
	}

	/*
	 * Floating-point roundoff can leave the target infinitesimally above
	 * the final accumulated value.  In that case, return the last point in
	 * the bounded support.
	 */
	for (unsigned u = 0; u < 8; u ++) {
		last[u] = vals[u][lens[u] - 1];
	}
	memcpy(zblk, last, sizeof last);
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sample_z_float(int32_t *z0, int32_t *z1, int64_t *norm2,
	const uint8_t *t0, const uint8_t *t1, unsigned logn,
	double sigma, int bound, hawk_rng rng, void *rng_context)
{
	if (logn < 8 || logn > 10 || z0 == NULL || z1 == NULL
		|| norm2 == NULL || t0 == NULL || t1 == NULL)
	{
		return 0;
	}

	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	int64_t acc = 0;

	memset(z0, 0, n * sizeof *z0);
	memset(z1, 0, n * sizeof *z1);
	for (size_t r = 0; r < k; r ++) {
		int32_t zblk[8];
		uint8_t tau = e8_extract_tau(t0, t1, r, logn);
		if (!e8_sample_block_float(zblk,
			tau, sigma, bound, rng, rng_context))
		{
			return 0;
		}
		e8_write_block(z0, z1, r, zblk, logn);
		acc += e8_block_norm2(zblk);
	}

	*norm2 = acc;
	return 1;
}

#endif
