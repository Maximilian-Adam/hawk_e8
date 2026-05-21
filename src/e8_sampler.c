#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <assert.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>

#define E8_SAMPLER_MAX_BOUND   16
#define E8_SAMPLER_MAX_VALUES  (2 * E8_SAMPLER_MAX_BOUND + 1)
#define E8_BLOCK_DIM           8
#define E8_CA_COSETS           16
#define E8_1D_TAIL_SIGMAS      20.0
#define E8_1D_TAIL_FLOOR       32.0
#define E8_TWO_PI              6.28318530717958647692528676655900576839

static const uint8_t RM13_CHECK_ROWS[E8_CA_COSETS >> 2] = {
	0xFF, 0xAA, 0xCC, 0xF0
};

static const uint8_t RM13_CODEWORDS[E8_CA_COSETS] = {
	0x00, 0xFF, 0xAA, 0x55,
	0xCC, 0x33, 0x66, 0x99,
	0xF0, 0x0F, 0x5A, 0xA5,
	0x3C, 0xC3, 0x96, 0x69
};

/*
 * Orthogonal product basis for the Construction-A kernel component
 *
 *     2P (RM(1,3) + 2Z^8).
 *
 * Vectors are stored by product coordinate.  Each vector has squared norm
 * 32 and is orthogonal to the seven others.  Thus a selected component
 *
 *     P tau + 2P r + 2P(RM(1,3) + 2Z^8)
 *
 * is sampled by eight independent shifted integer Gaussians in these
 * product coordinates, then reconstructed as an E8-side block xblk.
 */
static const int32_t E8_CA_PRODUCT_BASIS[E8_BLOCK_DIM][E8_BLOCK_DIM] = {
	{ -4,  0,  0,  0,  0,  0,  0, -4 },
	{ -4,  0,  0,  0,  0,  0,  0,  4 },
	{  0, -4,  0,  0, -4,  0,  0,  0 },
	{  0, -4,  0,  0,  4,  0,  0,  0 },
	{  0,  0, -4,  0,  0, -4,  0,  0 },
	{  0,  0, -4,  0,  0,  4,  0,  0 },
	{  0,  0,  0, -4,  0,  0, -4,  0 },
	{  0,  0,  0, -4,  0,  0,  4,  0 }
};

#define E8_CA_PRODUCT_NORM2   32.0

/*
 * Experimental E8 samplers.
 *
 * This file is intentionally a standalone research component.  It uses
 * floating-point weights, data-dependent control flow, and no side-channel
 * hardening.  It is not suitable for final cryptographic deployment.
 *
 * The bounded functions are retained as an explicitly named truncated
 * baseline.  The HAWK-E8-CM signer uses the Construction-A CM functions
 * below.  Their external contract is the HAWK-E8-CM contract: tau is the
 * internal P-coordinate label, the returned block is zblk in 2Z^8 + tau,
 * and the measured E8-side vector is xblk = P zblk.
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

static unsigned
parity8(uint8_t x)
{
	x ^= x >> 4;
	x ^= x >> 2;
	x ^= x >> 1;
	return x & 1u;
}

static uint8_t
rm13_component_rep(uint8_t component)
{
	/*
	 * The RM(1,3) parity-check columns are the odd four-bit values
	 * 1,3,5,...,15.  Columns 0,1,2,4 form an invertible 4x4 subsystem, so
	 * this compact representative has the requested syndrome.
	 */
	uint8_t b1 = (uint8_t)((component >> 1) & 1u);
	uint8_t b2 = (uint8_t)((component >> 2) & 1u);
	uint8_t b4 = (uint8_t)((component >> 3) & 1u);
	uint8_t b0 = (uint8_t)((component & 1u) ^ b1 ^ b2 ^ b4);

	return (uint8_t)(b0 | (b1 << 1) | (b2 << 2) | (b4 << 4));
}

/* see hawk_e8_inner.h */
uint8_t
e8_rm13_syndrome(uint8_t p)
{
	uint8_t s = 0;

	for (unsigned u = 0; u < sizeof RM13_CHECK_ROWS; u ++) {
		s |= (uint8_t)(parity8(p & RM13_CHECK_ROWS[u]) << u);
	}
	return s;
}

/* see hawk_e8_inner.h */
uint8_t
e8_rm13_codeword(unsigned u)
{
	return u < E8_CA_COSETS ? RM13_CODEWORDS[u] : 0;
}

static uint8_t
lift_parity_from_z(uint8_t tau, const int32_t *zblk)
{
	uint8_t p = 0;

	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		int32_t d = zblk[u] - (int32_t)((tau >> u) & 1u);
		int32_t y;

		assert((d & 1) == 0);
		y = d / 2;
		p |= (uint8_t)((((uint32_t)y) & 1u) << u);
	}
	return p;
}

static void
make_u8_block(int32_t *blk, uint8_t bits)
{
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		blk[u] = (int32_t)((bits >> u) & 1u);
	}
}

static double
sample_1d_integer_mass(double center, double sigma)
{
	if (!(sigma > 0.0)) {
		return 0.0;
	}

	double radius = E8_1D_TAIL_SIGMAS * sigma + E8_1D_TAIL_FLOOR;
	double lo_d = floor(center - radius);
	double hi_d = ceil(center + radius);

	if (lo_d < (double)INT32_MIN || hi_d > (double)INT32_MAX) {
		return 0.0;
	}

	int lo = (int)lo_d;
	int hi = (int)hi_d;
	double inv_2sigma2 = 1.0 / (2.0 * sigma * sigma);
	double total = 0.0;

	for (int x = lo; x <= hi; x ++) {
		double d = (double)x - center;
		total += exp(-(d * d) * inv_2sigma2);
	}

	return total;
}

static int
sample_1d_integer_gaussian(int32_t *out, double center,
	double sigma, hawk_rng rng, void *rng_context)
{
	if (out == NULL || !(sigma > 0.0) || rng == NULL) {
		return 0;
	}

	double radius = E8_1D_TAIL_SIGMAS * sigma + E8_1D_TAIL_FLOOR;
	double lo_d = floor(center - radius);
	double hi_d = ceil(center + radius);

	if (lo_d < (double)INT32_MIN || hi_d > (double)INT32_MAX) {
		return 0;
	}

	int lo = (int)lo_d;
	int hi = (int)hi_d;
	double inv_2sigma2 = 1.0 / (2.0 * sigma * sigma);
	double total = sample_1d_integer_mass(center, sigma);
	if (!(total > 0.0)) {
		return 0;
	}
	double target = rng_double(rng, rng_context) * total;
	double cdf = 0.0;
	for (int x = lo; x <= hi; x ++) {
		double d = (double)x - center;
		cdf += exp(-(d * d) * inv_2sigma2);
		if (cdf >= target) {
			*out = (int32_t)x;
			return 1;
		}
	}

	*out = (int32_t)hi;
	return 1;
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
e8_block_solve_P_checked(int32_t *zblk,
	const int32_t *xblk, uint8_t tau)
{
	static const int32_t PINV_NUM[E8_BLOCK_DIM][E8_BLOCK_DIM] = {
		{  1,  1, -1,  1,  1, -3,  3, -1 },
		{ -1,  1,  1, -1,  1,  1, -3,  3 },
		{  1, -1,  1,  1, -3,  1,  1, -3 },
		{ -1,  1, -1,  1,  3, -3,  1,  1 },
		{  0,  0,  0,  0,  2,  0,  0,  2 },
		{  0,  0,  0,  0, -2,  2,  0,  0 },
		{  0,  0,  0,  0,  0, -2,  2,  0 },
		{  0,  0,  0,  0,  0,  0, -2,  2 }
	};
	int32_t check[E8_BLOCK_DIM];

	if (zblk == NULL || xblk == NULL) {
		return 0;
	}

	for (unsigned i = 0; i < E8_BLOCK_DIM; i ++) {
		int64_t num = 0;

		for (unsigned j = 0; j < E8_BLOCK_DIM; j ++) {
			num += (int64_t)PINV_NUM[i][j] * xblk[j];
		}
		if ((num % 4) != 0) {
			return 0;
		}
		num /= 4;
		if (num < INT32_MIN || num > INT32_MAX) {
			return 0;
		}
		zblk[i] = (int32_t)num;
		if ((((uint32_t)zblk[i]) & 1u) != ((tau >> i) & 1u)) {
			return 0;
		}
	}

	e8_block_apply_P(check, zblk);
	for (unsigned i = 0; i < E8_BLOCK_DIM; i ++) {
		if (check[i] != xblk[i]) {
			return 0;
		}
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sample_block_bounded_float(int32_t *zblk, uint8_t tau,
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
e8_sample_z_bounded_float(int32_t *z0, int32_t *z1, int64_t *norm2,
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
		if (!e8_sample_block_bounded_float(zblk,
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

/* see hawk_e8_inner.h */
int
e8_sample_block_float(int32_t *zblk, uint8_t tau,
	double sigma, int bound, hawk_rng rng, void *rng_context)
{
	return e8_sample_block_bounded_float(zblk,
		tau, sigma, bound, rng, rng_context);
}

/* see hawk_e8_inner.h */
int
e8_sample_z_float(int32_t *z0, int32_t *z1, int64_t *norm2,
	const uint8_t *t0, const uint8_t *t1, unsigned logn,
	double sigma, int bound, hawk_rng rng, void *rng_context)
{
	return e8_sample_z_bounded_float(z0, z1, norm2,
		t0, t1, logn, sigma, bound, rng, rng_context);
}

/* see hawk_e8_inner.h */
double
e8_sigma_to_rho_s(double sigma)
{
	return sqrt(E8_TWO_PI) * sigma;
}

static int
rm13_codeword_index(uint8_t codeword, uint8_t *index)
{
	for (unsigned u = 0; u < E8_CA_COSETS; u ++) {
		if (RM13_CODEWORDS[u] == codeword) {
			*index = (uint8_t)u;
			return 1;
		}
	}
	return 0;
}

static void
e8_ca_component_offset_cm(int32_t *offset,
	uint8_t tau, uint8_t component)
{
	int32_t taublk[E8_BLOCK_DIM];
	int32_t repblk[E8_BLOCK_DIM];
	int32_t prep[E8_BLOCK_DIM];

	make_u8_block(taublk, tau);
	make_u8_block(repblk, rm13_component_rep(component));
	e8_block_apply_P(offset, taublk);
	e8_block_apply_P(prep, repblk);
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		offset[u] += 2 * prep[u];
	}
}

static double
e8_ca_component_center(const int32_t *offset, unsigned coord)
{
	int64_t dot = 0;

	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		dot += (int64_t)offset[u] * E8_CA_PRODUCT_BASIS[coord][u];
	}
	return -(double)dot / E8_CA_PRODUCT_NORM2;
}

static double
e8_ca_component_mass_cm(uint8_t tau, uint8_t component_codeword,
	double sigma_sign)
{
	uint8_t component;
	int32_t offset[E8_BLOCK_DIM];
	double sigma_coord = sigma_sign / sqrt(E8_CA_PRODUCT_NORM2);
	double mass = 1.0;

	if (!(sigma_sign > 0.0)
		|| !rm13_codeword_index(component_codeword, &component))
	{
		return 0.0;
	}

	e8_ca_component_offset_cm(offset, tau, component);
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		double center = e8_ca_component_center(offset, u);
		double m = sample_1d_integer_mass(center, sigma_coord);

		if (!(m > 0.0)) {
			return 0.0;
		}
		mass *= m;
	}
	return mass;
}

static int
e8_ca_select_component_cm(uint8_t *component, uint8_t *component_codeword,
	uint8_t tau, double sigma_sign, hawk_rng rng, void *rng_context)
{
	double masses[E8_CA_COSETS];
	double total = 0.0;

	if (component == NULL || component_codeword == NULL
		|| !(sigma_sign > 0.0) || rng == NULL)
	{
		return 0;
	}

	for (unsigned u = 0; u < E8_CA_COSETS; u ++) {
		masses[u] = e8_ca_component_mass_cm(tau,
			RM13_CODEWORDS[u], sigma_sign);
		total += masses[u];
	}
	if (!(total > 0.0)) {
		return 0;
	}

	double target = rng_double(rng, rng_context) * total;
	double cdf = 0.0;
	for (unsigned u = 0; u < E8_CA_COSETS; u ++) {
		cdf += masses[u];
		if (masses[u] > 0.0 && cdf >= target) {
			*component = (uint8_t)u;
			*component_codeword = RM13_CODEWORDS[u];
			return 1;
		}
	}

	for (int u = E8_CA_COSETS - 1; u >= 0; u --) {
		if (masses[u] > 0.0) {
			*component = (uint8_t)u;
			*component_codeword = RM13_CODEWORDS[u];
			return 1;
		}
	}
	return 0;
}

static int
e8_ca_reconstruct_x(int32_t *xblk, const int32_t *offset,
	const int32_t *coords)
{
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		int64_t x = offset[u];

		for (unsigned v = 0; v < E8_BLOCK_DIM; v ++) {
			x += (int64_t)E8_CA_PRODUCT_BASIS[v][u] * coords[v];
		}
		if (x < INT32_MIN || x > INT32_MAX) {
			return 0;
		}
		xblk[u] = (int32_t)x;
	}
	return 1;
}

static int
e8_sample_block_construction_a_cm_inner(int32_t *zblk, uint8_t tau,
	double sigma_sign, hawk_rng rng, void *rng_context,
	uint64_t *norm2_out, e8_sampler_stats *stats,
	e8_ca_sample_trace *trace)
{
	uint8_t component, component_codeword;
	int32_t offset[E8_BLOCK_DIM];
	int32_t coords[E8_BLOCK_DIM];
	int32_t xblk[E8_BLOCK_DIM];
	int64_t n2 = 0;
	double sigma_coord = sigma_sign / sqrt(E8_CA_PRODUCT_NORM2);

	/*
	 * Direct coset-matched Construction-A sampler for one HAWK-E8-CM
	 * block.  Here M = PZ^8.  For z = tau + 2y, the E8-side coset is
	 *
	 *     C_tau = 2M + P tau = P tau + 2P Z^8.
	 *
	 * The lift y is first decomposed modulo the Construction-A lattice
	 * RM(1,3)+2Z^8.  Each of the 16 RM(1,3)-labelled components is
	 *
	 *     P tau + 2P r + 2P(RM(1,3)+2Z^8),
	 *
	 * where r is a representative with the selected syndrome.  The kernel
	 * lattice on the right has the orthogonal product basis above, so its
	 * conditional Gaussian mass and sample are products of eight shifted
	 * one-dimensional integer Gaussians in x-space.  After reconstructing
	 * xblk in C_tau, the checked inverse map recovers zblk = P^{-1} xblk;
	 * because xblk - P tau is in 2PZ^8, this inverse is integral and
	 * zblk is congruent to tau modulo 2.
	 */
	if (zblk == NULL || !(sigma_sign > 0.0) || rng == NULL) {
		return 0;
	}

	if (!e8_ca_select_component_cm(&component, &component_codeword,
		tau, sigma_sign, rng, rng_context))
	{
		return 0;
	}
	e8_ca_component_offset_cm(offset, tau, component);
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		double center = e8_ca_component_center(offset, u);

		if (!sample_1d_integer_gaussian(&coords[u],
			center, sigma_coord, rng, rng_context))
		{
			return 0;
		}
	}
	if (!e8_ca_reconstruct_x(xblk, offset, coords)
		|| !e8_block_solve_P_checked(zblk, xblk, tau))
	{
		return 0;
	}

	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		n2 += (int64_t)xblk[u] * xblk[u];
	}
	if (n2 < 0 || (uint64_t)n2 != (uint64_t)e8_block_norm2(zblk)
		|| e8_rm13_syndrome(lift_parity_from_z(tau, zblk))
			!= component)
	{
		return 0;
	}

	if (norm2_out != NULL) {
		*norm2_out = (uint64_t)n2;
	}
	if (stats != NULL) {
		stats->blocks ++;
		stats->one_dim_samples += E8_BLOCK_DIM;
		stats->construction_a_cosets[component] ++;
		stats->norm2_sum += (uint64_t)n2;
		if ((uint64_t)n2 > stats->norm2_max) {
			stats->norm2_max = (uint64_t)n2;
		}
	}
	if (trace != NULL) {
		trace->component = component;
		trace->component_codeword = component_codeword;
		memcpy(trace->product_coords, coords, sizeof coords);
		memcpy(trace->xblk, xblk, sizeof xblk);
		memcpy(trace->zblk, zblk, E8_BLOCK_DIM * sizeof *zblk);
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sample_block_construction_a_cm_trace(int32_t *zblk, uint8_t tau,
	double sigma_sign, hawk_rng rng, void *rng_context,
	uint64_t *norm2_out, e8_sampler_stats *stats,
	e8_ca_sample_trace *trace)
{
	return e8_sample_block_construction_a_cm_inner(zblk, tau,
		sigma_sign, rng, rng_context, norm2_out, stats, trace);
}

/* see hawk_e8_inner.h */
int
e8_sample_block_construction_a_cm(int32_t *zblk, uint8_t tau,
	double sigma_sign, hawk_rng rng, void *rng_context,
	uint64_t *norm2_out, e8_sampler_stats *stats)
{
	return e8_sample_block_construction_a_cm_inner(zblk, tau,
		sigma_sign, rng, rng_context, norm2_out, stats, NULL);
}

/* see hawk_e8_inner.h */
int
e8_sample_z_construction_a_cm(int32_t *z0, int32_t *z1,
	uint64_t *pnorm_out, const uint8_t *t0, const uint8_t *t1,
	unsigned logn, double sigma_sign, hawk_rng rng, void *rng_context,
	e8_sampler_stats *stats)
{
	if (logn < 8 || logn > 10 || z0 == NULL || z1 == NULL
		|| pnorm_out == NULL || t0 == NULL || t1 == NULL
		|| !(sigma_sign > 0.0) || rng == NULL)
	{
		return 0;
	}

	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	uint64_t acc = 0;

	memset(z0, 0, n * sizeof *z0);
	memset(z1, 0, n * sizeof *z1);
	for (size_t r = 0; r < k; r ++) {
		int32_t zblk[E8_BLOCK_DIM];
		uint64_t n2;
		uint8_t tau = e8_extract_tau(t0, t1, r, logn);

		if (!e8_sample_block_construction_a_cm(zblk,
			tau, sigma_sign, rng, rng_context, &n2, stats))
		{
			return 0;
		}
		e8_write_block(z0, z1, r, zblk, logn);
		if (UINT64_MAX - acc < n2) {
			return 0;
		}
		acc += n2;
	}

	*pnorm_out = acc;
	return 1;
}

#endif
