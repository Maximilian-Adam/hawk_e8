#ifndef E8_SAMPLER_INNER_H__
#define E8_SAMPLER_INNER_H__

/*
 * Internal interface shared by the E8 sampler translation units:
 *
 *   e8_sampler.c          integer-only: block geometry helpers, thread and
 *                         RNG-mode controls, stream derivations, worker
 *                         dispatch (e8_sampler_run()), profile and counters
 *   e8_sampler_hot.c      integer-only: the hot64 sampler on the generated
 *                         tables (e8/tables/)
 *   e8_sampler_runtime.c  floating point, tests and tools only: the runtime
 *                         table builder for arbitrary sigma_sign and the
 *                         legacy double-sigma entry points
 *
 * Every function here is integer-only.
 */

#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <assert.h>
#include <limits.h>
#include <string.h>

#include "cmn_pool.h"
#include "cmn_rng.h"
#include "cmn_timing.h"

#define E8_BLOCK_DIM           8
#define E8_CA_COSETS           16
#define E8_CA_TAUS             256
#define E8_MASTER_SEED_BYTES   32
#define E8_BOUND_SEED_BYTES    40
#define E8_SAMPLER_MAX_THREADS 24
#define E8_CA_PRODUCT_NORM2_I  32

#ifndef HAWK_E8_SAMPLER_THREADS
#define HAWK_E8_SAMPLER_THREADS 1
#endif

#define e8_sampler_run  Zh(e8_sampler_run)
#define e8_sampler_block_rng_init  Zh(e8_sampler_block_rng_init)
#define e8_sampler_stats_add  Zh(e8_sampler_stats_add)

static const uint8_t E8_RM13_CODEWORDS[E8_CA_COSETS] = {
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

/*
 * z-side change per unit of each product coordinate: z = base_z
 * + sum_v coords[v] * E8_CA_DELTA_Z[v].
 */
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

static inline unsigned
e8_parity8(uint8_t x)
{
	x ^= x >> 4;
	x ^= x >> 2;
	x ^= x >> 1;
	return x & 1u;
}

/*
 * Representative of an RM(1,3) component: the parity-check columns are
 * the odd four-bit values 1,3,5,...,15; columns 0,1,2,4 form an
 * invertible 4x4 subsystem, so this compact representative has the
 * requested syndrome.
 */
static inline uint8_t
e8_rm13_component_rep(uint8_t component)
{
	uint8_t b1 = (uint8_t)((component >> 1) & 1u);
	uint8_t b2 = (uint8_t)((component >> 2) & 1u);
	uint8_t b4 = (uint8_t)((component >> 3) & 1u);
	uint8_t b0 = (uint8_t)((component & 1u) ^ b1 ^ b2 ^ b4);

	return (uint8_t)(b0 | (b1 << 1) | (b2 << 2) | (b4 << 4));
}

static inline void
e8_make_u8_block(int32_t *blk, uint8_t bits)
{
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		blk[u] = (int32_t)((bits >> u) & 1u);
	}
}

/*
 * E8-side offset of a component: P tau + 2 P rep(component).
 */
static inline void
e8_ca_component_offset_cm(int32_t *offset, uint8_t tau, uint8_t component)
{
	int32_t taublk[E8_BLOCK_DIM];
	int32_t repblk[E8_BLOCK_DIM];
	int32_t prep[E8_BLOCK_DIM];

	e8_make_u8_block(taublk, tau);
	e8_make_u8_block(repblk, e8_rm13_component_rep(component));
	e8_block_apply_P(offset, taublk);
	e8_block_apply_P(prep, repblk);
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		offset[u] += 2 * prep[u];
	}
}

/*
 * Centre of product coordinate coord, times 32.
 */
static inline int64_t
e8_ca_component_center_num32(const int32_t *offset, unsigned coord)
{
	int64_t dot = 0;

	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		dot += (int64_t)offset[u] * E8_CA_PRODUCT_BASIS[coord][u];
	}
	return -dot;
}

static inline int
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

static inline int
e8_ca_reconstruct_z_affine(int32_t *zblk, const int8_t *base_z,
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

#if HAWK_E8_DEBUG_CHECKS
static inline uint8_t
e8_lift_parity_from_z(uint8_t tau, const int32_t *zblk)
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

/*
 * Debug check of a sampled block: reconstruct x from the component offset
 * and product coordinates, solve back to z, and compare z, the norm and
 * the RM(1,3) syndrome with the fast path.
 */
static inline int
e8_ca_debug_check_block(const int32_t *zblk, uint8_t tau,
	uint8_t component, const int32_t *coords, int64_t n2)
{
	int32_t offset[E8_BLOCK_DIM];
	int32_t xblk[E8_BLOCK_DIM];
	int32_t check_z[E8_BLOCK_DIM];
	int64_t check_n2 = 0;

	e8_ca_component_offset_cm(offset, tau, component);
	if (!e8_ca_reconstruct_x(xblk, offset, coords)
		|| !e8_block_solve_P_checked(check_z, xblk, tau))
	{
		return 0;
	}
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		check_n2 += (int64_t)xblk[u] * xblk[u];
		if (check_z[u] != zblk[u]) {
			return 0;
		}
	}
	return n2 >= 0 && check_n2 == n2
		&& (uint64_t)n2 == (uint64_t)e8_block_norm2(zblk)
		&& e8_rm13_syndrome(e8_lift_parity_from_z(tau, zblk))
			== component;
}
#endif

/*
 * Fill a block trace (tests): component, product coordinates, x and z.
 */
static inline int
e8_ca_fill_trace(e8_ca_sample_trace *trace, const int32_t *zblk,
	uint8_t tau, uint8_t component, const int32_t *coords)
{
	int32_t offset[E8_BLOCK_DIM];
	int32_t xblk[E8_BLOCK_DIM];

	e8_ca_component_offset_cm(offset, tau, component);
	if (!e8_ca_reconstruct_x(xblk, offset, coords)) {
		return 0;
	}
	trace->component = component;
	trace->component_codeword = E8_RM13_CODEWORDS[component];
	memcpy(trace->product_coords, coords,
		E8_BLOCK_DIM * sizeof *coords);
	memcpy(trace->xblk, xblk, sizeof xblk);
	memcpy(trace->zblk, zblk, E8_BLOCK_DIM * sizeof *zblk);
	return 1;
}

static inline void
e8_ca_stats_add_block(e8_sampler_stats *stats, uint8_t component,
	uint64_t n2)
{
	if (stats != NULL) {
		stats->blocks ++;
		stats->one_dim_samples += E8_BLOCK_DIM;
		stats->construction_a_cosets[component] ++;
		stats->norm2_sum += n2;
		if (n2 > stats->norm2_max) {
			stats->norm2_max = n2;
		}
	}
}

/*
 * Per-stage sampler profile timers (HAWK_E8_PROFILE_SAMPLER; off by
 * default because they run on every block inside the timed call).
 */
#if HAWK_E8_PROFILE_SAMPLER
#define e8_profile_wall_ns()          cmn_wall_ns()
#define e8_profile_cycles_start()     cmn_tsc_start()
static inline void
e8_profile_add(uint64_t *cycles_total, uint64_t *wall_total,
	uint64_t cycles_start, uint64_t wall_start)
{
	uint64_t wall_end = cmn_wall_ns();
	uint64_t cycles_end = cmn_tsc_end();

	*cycles_total += cmn_delta(cycles_start, cycles_end);
	*wall_total += cmn_delta(wall_start, wall_end);
}
#else
#define e8_profile_wall_ns()          ((uint64_t)0)
#define e8_profile_cycles_start()     ((uint64_t)0)
#define e8_profile_add(ct, wt, cs, ws) \
	do { \
		(void)(ct); \
		(void)(wt); \
		(void)(cs); \
		(void)(ws); \
	} while (0)
#endif

/*
 * One worker's share of a full-dimension call: blocks [start, end).
 */
typedef struct e8_sampler_worker_s e8_sampler_worker;

/*
 * A variant's range sampler: sample blocks [w->start, w->end), writing
 * z0/z1 and adding to w->norm2, w->stats and (per-block mode) w->rng and
 * w->profile. worker_stream is the worker's stream in per-worker mode
 * and NULL in per-block mode, where each block opens its own stream with
 * e8_sampler_block_rng_init(). Returns 1 on success.
 */
typedef int (*e8_sample_range_fn)(e8_sampler_worker *w,
	cmn_rng *worker_stream);

struct e8_sampler_worker_s {
	int32_t *z0;
	int32_t *z1;
	const uint8_t *t0;
	const uint8_t *t1;
	const uint8_t *seed;
	const shake_context *prefix;
	const void *tables;
	e8_sample_range_fn range_fn;
	unsigned logn;
	unsigned worker_id;
	unsigned rng_mode;
	size_t start;
	size_t end;
	uint64_t sigma_sign_bits;
	uint64_t norm2;
	e8_sampler_stats stats;
	e8_sampler_profile profile;
	cmn_rng_counters rng;
	int ok;
};

/*
 * Full-dimension sampling with a variant's range sampler: resolves the
 * thread count and RNG mode, draws the seed (40 bytes per worker mode, 32
 * per block mode) from rng, splits the n/4 blocks into contiguous worker
 * ranges, runs worker 0 on the caller and the others on the pool, and
 * sums the norms. Arguments as for e8_sample_z_fn (hawk_e8_inner.h).
 */
int e8_sampler_run(e8_sample_range_fn range_fn, const void *tables,
	uint64_t sigma_sign_bits, int32_t *z0, int32_t *z1,
	uint64_t *pnorm_out, const uint8_t *t0, const uint8_t *t1,
	unsigned logn, unsigned rng_mode,
	const shake_context *prefix, uint64_t prefix_permutations,
	hawk_rng rng, void *rng_context, e8_sampler_stats *stats);

/*
 * Legacy per-block stream of block r (see hawk_e8_inner.h):
 *
 *   SHAKE256("HAWK-E8-CM block sampler v1" || seed32 || u8 logn || u8 tau
 *            || u64le(block_index) || u64le(sigma_sign bits))
 */
void e8_sampler_block_rng_init(cmn_rng *rng, const uint8_t *seed,
	unsigned logn, uint64_t block_index, uint8_t tau,
	uint64_t sigma_sign_bits);

void e8_sampler_stats_add(e8_sampler_stats *dst,
	const e8_sampler_stats *src);

#endif

#endif
