#include "e8_sampler_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * Runtime hot64 table builder and the legacy double-sigma sampler entry
 * points. This translation unit uses floating point and is linked only
 * into tests, tools and the caller-supplied-salt signers that sweep
 * sigma_sign (e8_sign_legacy.c); the public API and the benchmark signing
 * path use the generated tables (e8/tables/) and never call it.
 *
 * The builder is the former runtime cache: Gaussian weights and
 * normalisers in double, hot and component thresholds
 * floor(prefix / total * 2^64) computed in long double from double
 * prefixes (e8_cdf_threshold_u64()), with the same K = 6 and ordering as
 * the generator (e8/tables/gen_tables.py). The tail is integer:
 * thresholds of the tail's cumulative mass relative to the tail mass, the
 * last one forced to the maximum and the list truncated after the first
 * maximum (or at the generated table capacity, E8_HOT64_TAIL_LEN_MAX,
 * which then ends with the maximum). These are the generator's rounding
 * rules up to commit dd2c419; the generator now floors the mass at or
 * beyond each value (HAWK's rule), which moves most thresholds by a unit
 * or a few. Because the weights and prefixes are doubles, the low bits
 * of these thresholds differ from the exact generated ones in any case;
 * test_e8_tables reports the differences for the three fixed sigma_sign
 * values.
 *
 * The output uses the generated table format (e8_hot64_tables) and the
 * generated, sigma-independent geometry (e8_tables_geom.h), which the
 * builder recomputes and checks.
 */

#include "e8_tables_geom.h"
#include "e8_tables_hot64.h"

#define E8_1D_TAIL_SIGMAS      20.0
#define E8_1D_TAIL_FLOOR       32.0
#define E8_TWO_PI              6.28318530717958647692528676655900576839
#define E8_CA_PRODUCT_NORM2    32.0

typedef struct e8_rt_cache_s e8_rt_cache;
struct e8_rt_cache_s {
	uint64_t sigma_sign_bits;
	double sigma_coord;
	double class_total[E8_TAB_CLASSES];
	double component_mass[E8_CA_TAUS][E8_CA_COSETS];
	e8_hot64_tables tables;
	e8_rt_cache *next;
};

static e8_rt_cache *e8_rt_cache_list = NULL;
static pthread_mutex_t e8_rt_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint64_t
double_bits(double x)
{
	uint64_t bits;

	memcpy(&bits, &x, sizeof bits);
	return bits;
}

static uint64_t
e8_cdf_threshold_u64(double prefix, double total)
{
	/*
	 * Hot CDFs are stored as 64-bit fixed-point thresholds and compared
	 * with a 64-bit uniform target.  The conceptual scale is 2^64, one
	 * past UINT64_MAX, so full CDFs are saturated to UINT64_MAX.
	 */
	static const long double two64 = 0x1.0p64L;
	long double ratio, scaled;

	if (!(prefix > 0.0) || !(total > 0.0)) {
		return 0;
	}
	if (prefix >= total) {
		return UINT64_MAX;
	}
	ratio = (long double)prefix / (long double)total;
	if (!(ratio < 1.0L)) {
		return UINT64_MAX - 1;
	}
	scaled = ratio * two64;
	if (!(scaled < two64)) {
		return UINT64_MAX - 1;
	}
	return (uint64_t)scaled;
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
sample_1d_integer_mass(double center, double sigma)
{
	int lo, hi;
	double inv_2sigma2, total = 0.0;

	if (!sample_1d_integer_bounds(center, sigma, &lo, &hi)) {
		return 0.0;
	}
	inv_2sigma2 = 1.0 / (2.0 * sigma * sigma);
	for (int x = lo; x <= hi; x ++) {
		double d = (double)x - center;
		total += exp(-(d * d) * inv_2sigma2);
	}
	return total;
}

/*
 * Index of the largest weight not yet used (ties: lowest index, i.e.
 * lowest value), or len if none is positive.
 */
static size_t
best_unused(const double *weights, const unsigned char *used, size_t len)
{
	size_t best = len;
	double best_weight = 0.0;

	for (size_t i = 0; i < len; i ++) {
		if (!used[i] && weights[i] > best_weight) {
			best = i;
			best_weight = weights[i];
		}
	}
	return best;
}

/*
 * Build the hot64 row of one centre class; *total_out receives the
 * double normaliser.
 */
static int
e8_rt_build_row(e8_hot64_row *row, double *total_out,
	int64_t center_num32, double sigma)
{
	double center = (double)center_num32 / E8_CA_PRODUCT_NORM2;
	double *weights;
	unsigned char *used;
	double total = 0.0, tiny_mass = 0.0, tail_mass = 0.0, prefix;
	double inv_2sigma2;
	size_t len, chosen[E8_HOT64_TINY_LEN];
	unsigned chosen_len = 0;
	int lo, hi, ok = 0;

	memset(row, 0, sizeof *row);
	if (!sample_1d_integer_bounds(center, sigma, &lo, &hi)) {
		return 0;
	}
	len = (size_t)((int64_t)hi - (int64_t)lo + 1);
	weights = malloc(len * sizeof *weights);
	used = calloc(len, 1);
	if (weights == NULL || used == NULL) {
		goto out;
	}
	inv_2sigma2 = 1.0 / (2.0 * sigma * sigma);
	for (size_t i = 0; i < len; i ++) {
		double d = (double)(lo + (int)i) - center;

		weights[i] = exp(-(d * d) * inv_2sigma2);
		total += weights[i];
	}
	if (!(total > 0.0)) {
		goto out;
	}

	/* The K highest-weight values. */
	while (chosen_len < E8_HOT64_TINY_LEN) {
		size_t best = best_unused(weights, used, len);

		if (best == len) {
			break;
		}
		used[best] = 1;
		chosen[chosen_len ++] = best;
	}
	memset(used, 0, len);
	for (unsigned i = 0; i < chosen_len; i ++) {
		size_t idx = chosen[i];
		uint64_t q;

		prefix = tiny_mass + weights[idx];
		q = e8_cdf_threshold_u64(prefix, total);
		if (row->tiny_len > 0 && q <= row->tiny_cdf[row->tiny_len - 1]) {
			continue;
		}
		row->tiny_value[row->tiny_len] = (int8_t)(lo + (int)idx);
		row->tiny_cdf[row->tiny_len] = q;
		row->tiny_len ++;
		used[idx] = 1;
		tiny_mass = prefix;
	}
	if (row->tiny_len == 0) {
		goto out;
	}

	/*
	 * Integer tail: the remaining positive weights in weight order,
	 * thresholds relative to the tail mass, truncated after the first
	 * maximum or at the table capacity.
	 */
	for (size_t i = 0; i < len; i ++) {
		if (!used[i]) {
			tail_mass += weights[i];
		}
	}
	if (tail_mass > 0.0) {
		double tprefix = 0.0;

		while (row->tail_len < E8_HOT64_TAIL_LEN_MAX) {
			size_t best = best_unused(weights, used, len);
			uint64_t q;

			if (best == len) {
				break;
			}
			used[best] = 1;
			tprefix += weights[best];
			q = e8_cdf_threshold_u64(tprefix, tail_mass);
			row->tail_value[row->tail_len] =
				(int8_t)(lo + (int)best);
			row->tail_cdf[row->tail_len] = q;
			row->tail_len ++;
			if (q == UINT64_MAX) {
				break;
			}
		}
	}
	if (row->tail_len > 0) {
		row->tail_cdf[row->tail_len - 1] = UINT64_MAX;
	} else {
		row->tiny_cdf[row->tiny_len - 1] = UINT64_MAX;
	}
	*total_out = total;
	ok = 1;
out:
	free(weights);
	free(used);
	return ok;
}

/*
 * Recompute the sigma-independent geometry and check it against the
 * generated tables (class order: first occurrence over tau, component,
 * coordinate).
 */
static int
e8_rt_check_geometry(void)
{
	int64_t centers[E8_TAB_CLASSES];
	unsigned count = 0;

	for (unsigned tau = 0; tau < E8_CA_TAUS; tau ++) {
		for (unsigned comp = 0; comp < E8_CA_COSETS; comp ++) {
			int32_t offset[E8_BLOCK_DIM];
			int32_t base_z[E8_BLOCK_DIM];
			int64_t base_norm2 = 0;

			e8_ca_component_offset_cm(offset,
				(uint8_t)tau, (uint8_t)comp);
			if (!e8_block_solve_P_checked(base_z, offset,
				(uint8_t)tau))
			{
				return 0;
			}
			for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
				base_norm2 += (int64_t)offset[u] * offset[u];
				if (base_z[u] != e8_tab_base_z[tau][comp][u]) {
					return 0;
				}
			}
			if (base_norm2 != e8_tab_base_norm2[tau][comp]) {
				return 0;
			}
			for (unsigned v = 0; v < E8_BLOCK_DIM; v ++) {
				int64_t c = e8_ca_component_center_num32(
					offset, v);
				unsigned j;

				for (j = 0; j < count; j ++) {
					if (centers[j] == c) {
						break;
					}
				}
				if (j == count) {
					if (count == E8_TAB_CLASSES) {
						return 0;
					}
					centers[count ++] = c;
				}
				if (c != e8_tab_center_num32[j]
					|| j != e8_tab_coord_class[tau][comp][v])
				{
					return 0;
				}
			}
		}
	}
	return count == E8_TAB_CLASSES;
}

static int
e8_rt_build(e8_rt_cache *cache, double sigma_sign)
{
	static int geometry_ok = -1;
	e8_hot64_tables *tab = &cache->tables;

	if (geometry_ok < 0) {
		geometry_ok = e8_rt_check_geometry();
	}
	if (!geometry_ok) {
		return 0;
	}
	cache->sigma_sign_bits = double_bits(sigma_sign);
	cache->sigma_coord = sigma_sign / sqrt(E8_CA_PRODUCT_NORM2);
	tab->sigma_sign_bits = cache->sigma_sign_bits;
	for (unsigned j = 0; j < E8_TAB_CLASSES; j ++) {
		if (!e8_rt_build_row(&tab->rows[j], &cache->class_total[j],
			e8_tab_center_num32[j], cache->sigma_coord))
		{
			return 0;
		}
	}
	for (unsigned tau = 0; tau < E8_CA_TAUS; tau ++) {
		double prefix[E8_CA_COSETS];
		double total = 0.0;

		for (unsigned comp = 0; comp < E8_CA_COSETS; comp ++) {
			double mass = 1.0;

			for (unsigned v = 0; v < E8_BLOCK_DIM; v ++) {
				mass *= cache->class_total[
					e8_tab_coord_class[tau][comp][v]];
			}
			cache->component_mass[tau][comp] = mass;
			total += mass;
			prefix[comp] = total;
		}
		if (!(total > 0.0)) {
			return 0;
		}
		for (unsigned comp = 0; comp < E8_CA_COSETS; comp ++) {
			tab->component_cdf[tau][comp] =
				e8_cdf_threshold_u64(prefix[comp], total);
		}
		tab->component_cdf[tau][E8_CA_COSETS - 1] = UINT64_MAX;
	}
	return 1;
}

static e8_rt_cache *
e8_rt_get(double sigma_sign)
{
	uint64_t bits = double_bits(sigma_sign);
	e8_rt_cache *cache;

	if (!(sigma_sign > 0.0) || pthread_mutex_lock(&e8_rt_cache_mutex) != 0)
	{
		return NULL;
	}
	for (cache = e8_rt_cache_list; cache != NULL; cache = cache->next) {
		if (cache->sigma_sign_bits == bits) {
			(void)pthread_mutex_unlock(&e8_rt_cache_mutex);
			return cache;
		}
	}
	cache = calloc(1, sizeof *cache);
	if (cache != NULL && !e8_rt_build(cache, sigma_sign)) {
		free(cache);
		cache = NULL;
	}
	if (cache != NULL) {
		cache->next = e8_rt_cache_list;
		e8_rt_cache_list = cache;
	}
	(void)pthread_mutex_unlock(&e8_rt_cache_mutex);
	return cache;
}

/* see hawk_e8_inner.h */
const void *
e8_sampler_runtime_hot64(double sigma_sign)
{
	e8_rt_cache *cache = e8_rt_get(sigma_sign);

	return cache == NULL ? NULL : &cache->tables;
}

/* see hawk_e8_inner.h */
int
e8_sampler_warm_cache(double sigma_sign)
{
	return e8_rt_get(sigma_sign) != NULL;
}

/* see hawk_e8_inner.h */
double
e8_sigma_to_rho_s(double sigma)
{
	return sqrt(E8_TWO_PI) * sigma;
}

/* see hawk_e8_inner.h */
int
e8_sampler_cache_compare_1d_mass(double sigma_sign,
	int64_t center_num32, double *reference_mass, double *cached_mass)
{
	e8_rt_cache *cache;

	if (reference_mass == NULL || cached_mass == NULL) {
		return 0;
	}
	cache = e8_rt_get(sigma_sign);
	if (cache == NULL) {
		return 0;
	}
	*reference_mass = sample_1d_integer_mass(
		(double)center_num32 / E8_CA_PRODUCT_NORM2, cache->sigma_coord);
	for (unsigned j = 0; j < E8_TAB_CLASSES; j ++) {
		if (e8_tab_center_num32[j] == center_num32) {
			*cached_mass = cache->class_total[j];
			return 1;
		}
	}

	/*
	 * A centre that is not a class of the geometry: the builder's
	 * normaliser for that centre.
	 */
	{
		e8_hot64_row row;

		return e8_rt_build_row(&row, cached_mass, center_num32,
			cache->sigma_coord);
	}
}

/* see hawk_e8_inner.h */
int
e8_sampler_cache_compare_component_mass(uint8_t tau,
	uint8_t component, double sigma_sign,
	double *reference_mass, double *cached_mass)
{
	e8_rt_cache *cache;
	int32_t offset[E8_BLOCK_DIM];
	double sigma_coord, mass = 1.0;

	if (reference_mass == NULL || cached_mass == NULL
		|| component >= E8_CA_COSETS)
	{
		return 0;
	}
	cache = e8_rt_get(sigma_sign);
	if (cache == NULL) {
		return 0;
	}
	sigma_coord = sigma_sign / sqrt(E8_CA_PRODUCT_NORM2);
	e8_ca_component_offset_cm(offset, tau, component);
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		double center = (double)e8_ca_component_center_num32(offset, u)
			/ E8_CA_PRODUCT_NORM2;
		double m = sample_1d_integer_mass(center, sigma_coord);

		if (!(m > 0.0)) {
			return 0;
		}
		mass *= m;
	}
	*reference_mass = mass;
	*cached_mass = cache->component_mass[tau][component];
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sample_block_construction_a_cm_trace(int32_t *zblk, uint8_t tau,
	double sigma_sign, hawk_rng rng, void *rng_context,
	uint64_t *norm2_out, e8_sampler_stats *stats,
	e8_ca_sample_trace *trace)
{
	const void *tables = e8_sampler_runtime_hot64(sigma_sign);
	cmn_rng stream;

	if (tables == NULL || rng == NULL) {
		return 0;
	}
	cmn_rng_init_source(&stream, rng, rng_context);
	return e8_hot64_sample_block(tables, zblk, tau, &stream,
		norm2_out, stats, trace);
}

/* see hawk_e8_inner.h */
int
e8_sample_block_construction_a_cm(int32_t *zblk, uint8_t tau,
	double sigma_sign, hawk_rng rng, void *rng_context,
	uint64_t *norm2_out, e8_sampler_stats *stats)
{
	return e8_sample_block_construction_a_cm_trace(zblk, tau,
		sigma_sign, rng, rng_context, norm2_out, stats, NULL);
}

/* see hawk_e8_inner.h */
int
e8_sample_z_construction_a_cm(int32_t *z0, int32_t *z1,
	uint64_t *pnorm_out, const uint8_t *t0, const uint8_t *t1,
	unsigned logn, double sigma_sign, hawk_rng rng, void *rng_context,
	e8_sampler_stats *stats)
{
	return e8_sample_z_construction_a_cm_ex(z0, z1, pnorm_out, t0, t1,
		logn, sigma_sign, e8_sampler_get_rng_mode(), NULL, 0,
		rng, rng_context, stats);
}

/* see hawk_e8_inner.h */
int
e8_sample_z_construction_a_cm_ex(int32_t *z0, int32_t *z1,
	uint64_t *pnorm_out, const uint8_t *t0, const uint8_t *t1,
	unsigned logn, double sigma_sign, unsigned rng_mode,
	const shake_context *prefix, uint64_t prefix_permutations,
	hawk_rng rng, void *rng_context, e8_sampler_stats *stats)
{
	const void *tables = e8_sampler_runtime_hot64(sigma_sign);

	if (tables == NULL) {
		return 0;
	}
	return e8_sample_z_hot64(tables, z0, z1, pnorm_out, t0, t1, logn,
		rng_mode, prefix, prefix_permutations, rng, rng_context, stats);
}

#endif
