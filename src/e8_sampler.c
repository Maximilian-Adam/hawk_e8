#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <assert.h>
#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define E8_SAMPLER_MAX_BOUND   16
#define E8_SAMPLER_MAX_VALUES  (2 * E8_SAMPLER_MAX_BOUND + 1)
#define E8_BLOCK_DIM           8
#define E8_CA_COSETS           16
#define E8_CA_TAUS             256
#define E8_CA_CACHE_INIT_TABLES 32
#define E8_1D_TAIL_SIGMAS      20.0
#define E8_1D_TAIL_FLOOR       32.0
#define E8_TWO_PI              6.28318530717958647692528676655900576839
#define E8_1D_TINY_VALUES      6
#define E8_1D_TINY_SCALE       4294967296.0
#define E8_RNG_STREAM_BYTES    128

#ifndef HAWK_E8_DEBUG_CHECKS
#define HAWK_E8_DEBUG_CHECKS   1
#endif

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
#define E8_CA_PRODUCT_NORM2_I 32

static const int32_t E8_CA_DELTA_Z[E8_BLOCK_DIM][E8_BLOCK_DIM] = {
	{  0, -2,  2,  0, -2,  0,  0, -2 },
	{ -2,  4, -4,  2,  2,  0,  0,  2 },
	{ -2, -2,  4, -4, -2,  2,  0,  0 },
	{  0,  0, -2,  2,  2, -2,  0,  0 },
	{  4, -2, -2,  4,  0, -2,  2,  0 },
	{ -2,  0,  0, -2,  0,  2, -2,  0 },
	{ -4,  4, -2, -2,  0,  0, -2,  2 },
	{  2, -2,  0,  0,  0,  0,  2, -2 }
};

typedef struct {
	uint64_t sigma_coord_bits;
	int64_t center_num32;
	int lo;
	int hi;
	size_t len;
	double total;
	double *cdf;
} e8_1d_integer_table;

typedef struct {
	int64_t center_num32;
	double tiny_mass;
	double tail_mass;
	uint8_t tiny_len;
	size_t tail_len;
	int32_t tiny_value[E8_1D_TINY_VALUES];
	uint32_t tiny_cdf[E8_1D_TINY_VALUES];
	int32_t *tail_value;
	double *tail_cdf;
} e8_1d_hot_row;

typedef struct {
	double masses[E8_CA_COSETS];
	double prefix[E8_CA_COSETS];
	double total;
} e8_ca_component_table;

typedef struct e8_ca_sigma_cache_s e8_ca_sigma_cache;
struct e8_ca_sigma_cache_s {
	uint64_t sigma_sign_bits;
	uint64_t sigma_coord_bits;
	double sigma_sign;
	double sigma_coord;
	size_t table_count;
	size_t table_cap;
	e8_1d_integer_table *tables;
	e8_1d_hot_row *hot_rows;
	uint32_t component_cdf[E8_CA_TAUS][E8_CA_COSETS];
	uint8_t component_fallback[E8_CA_TAUS];
	int32_t base_z[E8_CA_TAUS][E8_CA_COSETS][E8_BLOCK_DIM];
	int64_t base_norm2[E8_CA_TAUS][E8_CA_COSETS];
	e8_ca_component_table component[E8_CA_TAUS];
	uint8_t coord_class[E8_CA_TAUS][E8_CA_COSETS][E8_BLOCK_DIM];
	e8_ca_sigma_cache *next;
};

static e8_ca_sigma_cache *e8_ca_cache_list = NULL;

typedef struct {
	hawk_rng rng;
	void *rng_context;
	uint8_t buf[E8_RNG_STREAM_BYTES];
	size_t pos;
	size_t len;
} e8_rng_stream;

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

static int
rng_stream_init(e8_rng_stream *stream, hawk_rng rng, void *rng_context)
{
	if (stream == NULL || rng == NULL) {
		return 0;
	}
	stream->rng = rng;
	stream->rng_context = rng_context;
	stream->pos = 0;
	stream->len = 0;
	return 1;
}

static void
rng_stream_refill(e8_rng_stream *stream)
{
	stream->rng(stream->rng_context, stream->buf, sizeof stream->buf);
	stream->pos = 0;
	stream->len = sizeof stream->buf;
}

static uint64_t
rng_stream_u64(e8_rng_stream *stream)
{
	uint64_t x = 0;

	if (stream->len - stream->pos < 8) {
		rng_stream_refill(stream);
	}
	for (unsigned u = 0; u < 8; u ++) {
		x |= (uint64_t)stream->buf[stream->pos ++] << (u << 3);
	}
	return x;
}

static uint32_t
rng_stream_u32(e8_rng_stream *stream)
{
	uint32_t x = 0;

	if (stream->len - stream->pos < 4) {
		rng_stream_refill(stream);
	}
	for (unsigned u = 0; u < 4; u ++) {
		x |= (uint32_t)stream->buf[stream->pos ++] << (u << 3);
	}
	return x;
}

static double
rng_stream_double(e8_rng_stream *stream)
{
	uint64_t x = rng_stream_u64(stream);

	return (double)(x >> 11) * 0x1.0p-53;
}

static uint64_t
double_bits(double x)
{
	uint64_t bits;

	memcpy(&bits, &x, sizeof bits);
	return bits;
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

#if HAWK_E8_DEBUG_CHECKS
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
#endif

static void
make_u8_block(int32_t *blk, uint8_t bits)
{
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		blk[u] = (int32_t)((bits >> u) & 1u);
	}
}

static int
sample_1d_integer_bounds(double center, double sigma, int *lo, int *hi)
{
	if (!(sigma > 0.0)) {
		return 0;
	}

	double radius = E8_1D_TAIL_SIGMAS * sigma + E8_1D_TAIL_FLOOR;
	double lo_d = floor(center - radius);
	double hi_d = ceil(center + radius);

	if (lo_d < (double)INT32_MIN || hi_d > (double)INT32_MAX) {
		return 0;
	}

	*lo = (int)lo_d;
	*hi = (int)hi_d;
	return *lo <= *hi;
}

static double
sample_1d_integer_mass_range(double center, double sigma, int lo, int hi)
{
	double inv_2sigma2 = 1.0 / (2.0 * sigma * sigma);
	double total = 0.0;

	for (int x = lo; x <= hi; x ++) {
		double d = (double)x - center;
		total += exp(-(d * d) * inv_2sigma2);
	}

	return total;
}

static double
sample_1d_integer_mass(double center, double sigma)
{
	int lo, hi;

	if (!sample_1d_integer_bounds(center, sigma, &lo, &hi)) {
		return 0.0;
	}
	return sample_1d_integer_mass_range(center, sigma, lo, hi);
}

static void
sample_1d_hot_row_free(e8_1d_hot_row *row)
{
	if (row == NULL) {
		return;
	}
	free(row->tail_value);
	free(row->tail_cdf);
	memset(row, 0, sizeof *row);
}

static int
sample_1d_integer_table_build_tail(e8_1d_hot_row *row,
	const e8_1d_integer_table *table, const double *weights)
{
	unsigned char *used;
	size_t tail_cap = 0;
	double tail_mass = 0.0;

	if (row == NULL || table == NULL || weights == NULL) {
		return 0;
	}
	used = calloc(table->len, sizeof *used);
	if (used == NULL) {
		return 0;
	}
	for (unsigned i = 0; i < row->tiny_len; i ++) {
		int64_t idx = (int64_t)row->tiny_value[i] - table->lo;
		if (idx >= 0 && (size_t)idx < table->len) {
			used[idx] = 1;
		}
	}
	for (size_t i = 0; i < table->len; i ++) {
		if (!used[i] && weights[i] > 0.0) {
			tail_cap ++;
		}
	}
	if (tail_cap == 0) {
		free(used);
		row->tail_mass = 0.0;
		return 1;
	}

	row->tail_value = malloc(tail_cap * sizeof *row->tail_value);
	row->tail_cdf = malloc(tail_cap * sizeof *row->tail_cdf);
	if (row->tail_value == NULL || row->tail_cdf == NULL) {
		free(used);
		free(row->tail_value);
		free(row->tail_cdf);
		row->tail_value = NULL;
		row->tail_cdf = NULL;
		return 0;
	}

	for (size_t out = 0; out < tail_cap; out ++) {
		size_t best = table->len;
		double best_weight = 0.0;

		for (size_t i = 0; i < table->len; i ++) {
			if (!used[i] && weights[i] > best_weight) {
				best = i;
				best_weight = weights[i];
			}
		}
		if (best == table->len || !(best_weight > 0.0)) {
			break;
		}
		used[best] = 1;
		tail_mass += best_weight;
		row->tail_value[row->tail_len] = (int32_t)(table->lo + (int)best);
		row->tail_cdf[row->tail_len] = tail_mass;
		row->tail_len ++;
	}
	free(used);
	row->tail_mass = tail_mass;
	return row->tail_len > 0 && tail_mass > 0.0;
}

static int
sample_1d_integer_table_build_tiny(e8_1d_hot_row *row,
	const e8_1d_integer_table *table, const double *weights)
{
	size_t chosen[E8_1D_TINY_VALUES];
	unsigned chosen_len = 0;
	double tiny_mass = 0.0;
	double prefix = 0.0;

	if (row == NULL || table == NULL || weights == NULL || table->len == 0
		|| !(table->total > 0.0))
	{
		return 0;
	}
	memset(row, 0, sizeof *row);
	row->center_num32 = table->center_num32;

	for (unsigned out = 0; out < E8_1D_TINY_VALUES; out ++) {
		size_t best = table->len;
		double best_weight = 0.0;

		for (size_t i = 0; i < table->len; i ++) {
			int used = 0;

			for (unsigned j = 0; j < chosen_len; j ++) {
				used |= chosen[j] == i;
			}
			if (!used && weights[i] > best_weight) {
				best = i;
				best_weight = weights[i];
			}
		}
		if (best == table->len || !(best_weight > 0.0)) {
			break;
		}
		chosen[chosen_len ++] = best;
	}

	unsigned table_len = 0;
	for (unsigned i = 0; i < chosen_len; i ++) {
		size_t idx = chosen[i];
		uint64_t q;

		prefix = tiny_mass + weights[idx];
		q = (uint64_t)((prefix / table->total)
			* E8_1D_TINY_SCALE);
		if (q > UINT32_MAX) {
			q = UINT32_MAX;
		}
		if (table_len > 0 && q <= row->tiny_cdf[table_len - 1]) {
			continue;
		}
		row->tiny_value[table_len] = (int32_t)(table->lo + (int)idx);
		row->tiny_cdf[table_len] = (uint32_t)q;
		tiny_mass = prefix;
		table_len ++;
	}
	row->tiny_len = (uint8_t)table_len;
	row->tiny_mass = tiny_mass;
	if (table_len == 0 || !(tiny_mass > 0.0)) {
		return 0;
	}
	return sample_1d_integer_table_build_tail(row, table, weights);
}

static int
sample_1d_integer_table_build(e8_1d_integer_table *table,
	e8_1d_hot_row *row, int64_t center_num32, double sigma)
{
	double center = (double)center_num32 / E8_CA_PRODUCT_NORM2;
	int lo, hi;

	if (table == NULL || row == NULL
		|| !sample_1d_integer_bounds(center, sigma, &lo, &hi))
	{
		return 0;
	}

	int64_t span = (int64_t)hi - (int64_t)lo + 1;
	if (span <= 0) {
		return 0;
	}
	size_t len = (size_t)span;
	double *cdf = malloc(len * sizeof *cdf);
	if (cdf == NULL) {
		return 0;
	}
	double *weights = malloc(len * sizeof *weights);
	if (weights == NULL) {
		free(cdf);
		return 0;
	}

	double inv_2sigma2 = 1.0 / (2.0 * sigma * sigma);
	double total = 0.0;
	for (size_t i = 0; i < len; i ++) {
		int x = lo + (int)i;
		double d = (double)x - center;
		double w = exp(-(d * d) * inv_2sigma2);

		weights[i] = w;
		total += w;
		cdf[i] = total;
	}
	if (!(total > 0.0)) {
		free(weights);
		free(cdf);
		return 0;
	}

	table->center_num32 = center_num32;
	table->sigma_coord_bits = double_bits(sigma);
	table->lo = lo;
	table->hi = hi;
	table->len = len;
	table->total = total;
	table->cdf = cdf;
	int ok = sample_1d_integer_table_build_tiny(row, table, weights);
	free(weights);
	if (!ok) {
		free(cdf);
		memset(table, 0, sizeof *table);
		sample_1d_hot_row_free(row);
		return 0;
	}
	return 1;
}

static int
sample_1d_integer_gaussian_hot(int32_t *out,
	const e8_1d_hot_row *row, const e8_1d_integer_table *table,
	e8_rng_stream *rng_stream)
{
	if (out == NULL || row == NULL || table == NULL || table->cdf == NULL
		|| table->len == 0 || !(table->total > 0.0)
		|| row->tiny_len == 0 || rng_stream == NULL)
	{
		return 0;
	}

	uint32_t target = rng_stream_u32(rng_stream);
	for (unsigned i = 0; i < row->tiny_len; i ++) {
		if (target < row->tiny_cdf[i]) {
			*out = row->tiny_value[i];
			return 1;
		}
	}

	double tail_total = row->tail_mass;
	if (!(tail_total > 0.0)) {
		*out = row->tiny_value[row->tiny_len - 1];
		return 1;
	}

	double tail_target = rng_stream_double(rng_stream) * tail_total;
	for (size_t i = 0; i < row->tail_len; i ++) {
		if (row->tail_cdf[i] >= tail_target) {
			*out = row->tail_value[i];
			return 1;
		}
	}

	*out = row->tail_len > 0
		? row->tail_value[row->tail_len - 1]
		: row->tiny_value[row->tiny_len - 1];
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

static const int32_t E8_PINV_NUM[E8_BLOCK_DIM][E8_BLOCK_DIM] = {
	{  1,  1, -1,  1,  1, -3,  3, -1 },
	{ -1,  1,  1, -1,  1,  1, -3,  3 },
	{  1, -1,  1,  1, -3,  1,  1, -3 },
	{ -1,  1, -1,  1,  3, -3,  1,  1 },
	{  0,  0,  0,  0,  2,  0,  0,  2 },
	{  0,  0,  0,  0, -2,  2,  0,  0 },
	{  0,  0,  0,  0,  0, -2,  2,  0 },
	{  0,  0,  0,  0,  0,  0, -2,  2 }
};

/* see hawk_e8_inner.h */
int
e8_block_solve_P_checked(int32_t *zblk,
	const int32_t *xblk, uint8_t tau)
{
	int32_t check[E8_BLOCK_DIM];

	if (zblk == NULL || xblk == NULL) {
		return 0;
	}

	for (unsigned i = 0; i < E8_BLOCK_DIM; i ++) {
		int64_t num = 0;

		for (unsigned j = 0; j < E8_BLOCK_DIM; j ++) {
			num += (int64_t)E8_PINV_NUM[i][j] * xblk[j];
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

static int64_t
e8_ca_component_center_num32(const int32_t *offset, unsigned coord)
{
	int64_t dot = 0;

	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		dot += (int64_t)offset[u] * E8_CA_PRODUCT_BASIS[coord][u];
	}
	return -dot;
}

static double
e8_ca_component_center(const int32_t *offset, unsigned coord)
{
	return (double)e8_ca_component_center_num32(offset, coord)
		/ E8_CA_PRODUCT_NORM2;
}

static void
e8_ca_sigma_cache_free(e8_ca_sigma_cache *cache)
{
	if (cache == NULL) {
		return;
	}
	for (size_t i = 0; i < cache->table_count; i ++) {
		free(cache->tables[i].cdf);
		sample_1d_hot_row_free(&cache->hot_rows[i]);
	}
	free(cache->tables);
	free(cache->hot_rows);
	free(cache);
}

static int
e8_ca_cache_get_1d_table(e8_ca_sigma_cache *cache,
	int64_t center_num32, size_t *table_index)
{
	if (cache == NULL || table_index == NULL) {
		return 0;
	}

	for (size_t i = 0; i < cache->table_count; i ++) {
		if (cache->tables[i].center_num32 == center_num32
			&& cache->tables[i].sigma_coord_bits
				== cache->sigma_coord_bits)
		{
			*table_index = i;
			return 1;
		}
	}

	if (cache->table_count == cache->table_cap) {
		size_t new_cap = cache->table_cap == 0
			? E8_CA_CACHE_INIT_TABLES : cache->table_cap << 1;
		e8_1d_integer_table *new_tables =
			realloc(cache->tables, new_cap * sizeof *new_tables);
		e8_1d_hot_row *new_hot_rows;

		if (new_tables == NULL) {
			return 0;
		}
		cache->tables = new_tables;
		new_hot_rows = realloc(cache->hot_rows,
			new_cap * sizeof *new_hot_rows);
		if (new_hot_rows == NULL) {
			return 0;
		}
		cache->hot_rows = new_hot_rows;
		memset(cache->tables + cache->table_cap, 0,
			(new_cap - cache->table_cap) * sizeof *cache->tables);
		memset(cache->hot_rows + cache->table_cap, 0,
			(new_cap - cache->table_cap) * sizeof *cache->hot_rows);
		cache->table_cap = new_cap;
	}

	size_t new_index = cache->table_count;
	if (new_index > UINT8_MAX) {
		return 0;
	}
	if (!sample_1d_integer_table_build(&cache->tables[new_index],
		&cache->hot_rows[new_index], center_num32, cache->sigma_coord))
	{
		return 0;
	}
	cache->table_count ++;
	*table_index = new_index;
	return 1;
}

static int
e8_ca_component_cdf_build(e8_ca_sigma_cache *cache, unsigned tau)
{
	e8_ca_component_table *table;
	uint8_t fallback = 0;
	int have_fallback = 0;

	if (cache == NULL || tau >= E8_CA_TAUS) {
		return 0;
	}
	table = &cache->component[tau];
	if (!(table->total > 0.0)) {
		return 0;
	}

	for (unsigned component = 0; component < E8_CA_COSETS; component ++) {
		uint64_t q = (uint64_t)((table->prefix[component]
			/ table->total) * E8_1D_TINY_SCALE);

		if (q > UINT32_MAX) {
			q = UINT32_MAX;
		}
		cache->component_cdf[tau][component] = (uint32_t)q;
		if (table->masses[component] > 0.0) {
			fallback = (uint8_t)component;
			have_fallback = 1;
		}
	}
	if (!have_fallback) {
		return 0;
	}
	cache->component_cdf[tau][fallback] = UINT32_MAX;
	cache->component_fallback[tau] = fallback;
	return 1;
}

static int
e8_ca_sigma_cache_build(e8_ca_sigma_cache *cache)
{
	if (cache == NULL || !(cache->sigma_sign > 0.0)) {
		return 0;
	}

	for (unsigned tau = 0; tau < E8_CA_TAUS; tau ++) {
		double total = 0.0;

		for (unsigned component = 0;
			component < E8_CA_COSETS; component ++)
		{
			int32_t offset[E8_BLOCK_DIM];
			double mass = 1.0;
			int64_t base_norm2 = 0;

			e8_ca_component_offset_cm(offset,
				(uint8_t)tau, (uint8_t)component);
			if (!e8_block_solve_P_checked(
				cache->base_z[tau][component],
				offset, (uint8_t)tau))
			{
				return 0;
			}
			for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
				base_norm2 += (int64_t)offset[u] * offset[u];
			}
			cache->base_norm2[tau][component] = base_norm2;
			for (unsigned coord = 0; coord < E8_BLOCK_DIM; coord ++) {
				int64_t center_num32 =
					e8_ca_component_center_num32(offset, coord);
				size_t table_index;

				if (!e8_ca_cache_get_1d_table(cache,
					center_num32, &table_index))
				{
					return 0;
				}
				if (table_index > UINT8_MAX) {
					return 0;
				}
				cache->coord_class[tau][component][coord]
					= (uint8_t)table_index;
				mass *= cache->tables[table_index].total;
			}
			cache->component[tau].masses[component] = mass;
			total += mass;
			cache->component[tau].prefix[component] = total;
		}
		if (!(total > 0.0)) {
			return 0;
		}
		cache->component[tau].total = total;
		if (!e8_ca_component_cdf_build(cache, tau)) {
			return 0;
		}
	}
	return 1;
}

static e8_ca_sigma_cache *
e8_ca_get_sigma_cache(double sigma_sign)
{
	if (!(sigma_sign > 0.0)) {
		return NULL;
	}

	uint64_t sigma_bits = double_bits(sigma_sign);
	for (e8_ca_sigma_cache *cur = e8_ca_cache_list;
		cur != NULL; cur = cur->next)
	{
		if (cur->sigma_sign_bits == sigma_bits) {
			return cur;
		}
	}

	e8_ca_sigma_cache *cache = calloc(1, sizeof *cache);
	if (cache == NULL) {
		return NULL;
	}
	cache->sigma_sign = sigma_sign;
	cache->sigma_sign_bits = sigma_bits;
	cache->sigma_coord = sigma_sign / sqrt(E8_CA_PRODUCT_NORM2);
	cache->sigma_coord_bits = double_bits(cache->sigma_coord);
	if (!e8_ca_sigma_cache_build(cache)) {
		e8_ca_sigma_cache_free(cache);
		return NULL;
	}
	cache->next = e8_ca_cache_list;
	e8_ca_cache_list = cache;
	return cache;
}

/* see hawk_e8_inner.h */
int
e8_sampler_warm_cache(double sigma_sign)
{
	return e8_ca_get_sigma_cache(sigma_sign) != NULL;
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
e8_ca_component_mass_cm_cached(double *mass_out, uint8_t tau,
	uint8_t component, double sigma_sign)
{
	if (mass_out == NULL || component >= E8_CA_COSETS) {
		return 0;
	}

	e8_ca_sigma_cache *cache = e8_ca_get_sigma_cache(sigma_sign);
	if (cache == NULL) {
		return 0;
	}
	*mass_out = cache->component[tau].masses[component];
	return 1;
}

static int
e8_ca_select_component_cm_cached(uint8_t *component,
	uint8_t *component_codeword, uint8_t tau,
	e8_ca_sigma_cache *cache, e8_rng_stream *rng_stream)
{
	if (component == NULL || component_codeword == NULL
		|| cache == NULL || rng_stream == NULL)
	{
		return 0;
	}

	const uint32_t *cdf = cache->component_cdf[tau];
	uint32_t target = rng_stream_u32(rng_stream);
	for (unsigned u = 0; u < E8_CA_COSETS; u ++) {
		if (target < cdf[u]) {
			*component = (uint8_t)u;
			*component_codeword = RM13_CODEWORDS[u];
			return 1;
		}
	}

	*component = cache->component_fallback[tau];
	*component_codeword = RM13_CODEWORDS[*component];
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sampler_cache_compare_1d_mass(double sigma_sign,
	int64_t center_num32, double *reference_mass, double *cached_mass)
{
	if (reference_mass == NULL || cached_mass == NULL
		|| !(sigma_sign > 0.0))
	{
		return 0;
	}

	double sigma_coord = sigma_sign / sqrt(E8_CA_PRODUCT_NORM2);
	double center = (double)center_num32 / E8_CA_PRODUCT_NORM2;
	e8_ca_sigma_cache *cache = e8_ca_get_sigma_cache(sigma_sign);
	size_t table_index;

	if (cache == NULL
		|| !e8_ca_cache_get_1d_table(cache,
			center_num32, &table_index))
	{
		return 0;
	}
	*reference_mass = sample_1d_integer_mass(center, sigma_coord);
	*cached_mass = cache->tables[table_index].total;
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sampler_cache_compare_component_mass(uint8_t tau,
	uint8_t component, double sigma_sign,
	double *reference_mass, double *cached_mass)
{
	if (reference_mass == NULL || cached_mass == NULL
		|| component >= E8_CA_COSETS)
	{
		return 0;
	}

	double cmass;
	if (!e8_ca_component_mass_cm_cached(&cmass,
		tau, component, sigma_sign))
	{
		return 0;
	}
	*reference_mass = e8_ca_component_mass_cm(tau,
		RM13_CODEWORDS[component], sigma_sign);
	*cached_mass = cmass;
	return 1;
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
e8_ca_reconstruct_z_affine(int32_t *zblk, const int32_t *base_z,
	const int32_t *coords)
{
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		int64_t z = base_z[u];

		for (unsigned v = 0; v < E8_BLOCK_DIM; v ++) {
			z += (int64_t)coords[v] * E8_CA_DELTA_Z[v][u];
		}
		if (z < INT32_MIN || z > INT32_MAX) {
			return 0;
		}
		zblk[u] = (int32_t)z;
	}
	return 1;
}

static int
e8_sample_block_construction_a_cm_inner(int32_t *zblk, uint8_t tau,
	double sigma_sign, e8_rng_stream *rng_stream,
	uint64_t *norm2_out, e8_sampler_stats *stats,
	e8_ca_sample_trace *trace)
{
	uint8_t component, component_codeword;
	int32_t offset[E8_BLOCK_DIM];
	int32_t coords[E8_BLOCK_DIM];
	int32_t xblk[E8_BLOCK_DIM];
	int64_t n2 = 0;
	e8_ca_sigma_cache *cache;

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
	if (zblk == NULL || !(sigma_sign > 0.0) || rng_stream == NULL) {
		return 0;
	}

	cache = e8_ca_get_sigma_cache(sigma_sign);
	if (cache == NULL
		|| !e8_ca_select_component_cm_cached(&component,
			&component_codeword, tau, cache, rng_stream))
	{
		return 0;
	}
	n2 = cache->base_norm2[tau][component];
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		uint8_t center_class = cache->coord_class[tau][component][u];
		const e8_1d_hot_row *row = &cache->hot_rows[center_class];

		if (!sample_1d_integer_gaussian_hot(&coords[u],
			row, &cache->tables[center_class], rng_stream))
		{
			return 0;
		}
		int64_t c = coords[u];
		n2 += (int64_t)E8_CA_PRODUCT_NORM2_I * c * c
			- 2 * c * row->center_num32;
	}
	if (!e8_ca_reconstruct_z_affine(zblk,
		cache->base_z[tau][component], coords))
	{
		return 0;
	}
#if HAWK_E8_DEBUG_CHECKS
	int32_t check_z[E8_BLOCK_DIM];
	int64_t check_n2 = 0;

	e8_ca_component_offset_cm(offset, tau, component);
	if (!e8_ca_reconstruct_x(xblk, offset, coords)) {
		return 0;
	}
	if (!e8_block_solve_P_checked(check_z, xblk, tau)) {
		return 0;
	}
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		check_n2 += (int64_t)xblk[u] * xblk[u];
		if (check_z[u] != zblk[u]) {
			return 0;
		}
	}
	if (n2 < 0 || check_n2 != n2
		|| (uint64_t)n2 != (uint64_t)e8_block_norm2(zblk)
		|| e8_rm13_syndrome(lift_parity_from_z(tau, zblk))
			!= component)
	{
		return 0;
	}
#else
	if (n2 < 0) {
		return 0;
	}
	if (trace != NULL) {
		e8_ca_component_offset_cm(offset, tau, component);
		if (!e8_ca_reconstruct_x(xblk, offset, coords)) {
			return 0;
		}
	}
#endif

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
	e8_rng_stream rng_stream;

	if (!rng_stream_init(&rng_stream, rng, rng_context)) {
		return 0;
	}
	return e8_sample_block_construction_a_cm_inner(zblk, tau,
		sigma_sign, &rng_stream, norm2_out, stats, trace);
}

/* see hawk_e8_inner.h */
int
e8_sample_block_construction_a_cm(int32_t *zblk, uint8_t tau,
	double sigma_sign, hawk_rng rng, void *rng_context,
	uint64_t *norm2_out, e8_sampler_stats *stats)
{
	e8_rng_stream rng_stream;

	if (!rng_stream_init(&rng_stream, rng, rng_context)) {
		return 0;
	}
	return e8_sample_block_construction_a_cm_inner(zblk, tau,
		sigma_sign, &rng_stream, norm2_out, stats, NULL);
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
	if (!e8_sampler_warm_cache(sigma_sign)) {
		return 0;
	}

	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	uint64_t acc = 0;
	e8_rng_stream rng_stream;

	if (!rng_stream_init(&rng_stream, rng, rng_context)) {
		return 0;
	}

	memset(z0, 0, n * sizeof *z0);
	memset(z1, 0, n * sizeof *z1);
	for (size_t r = 0; r < k; r ++) {
		int32_t zblk[E8_BLOCK_DIM];
		uint64_t n2;
		uint8_t tau = e8_extract_tau(t0, t1, r, logn);

		if (!e8_sample_block_construction_a_cm_inner(zblk,
			tau, sigma_sign, &rng_stream, &n2, stats, NULL))
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
