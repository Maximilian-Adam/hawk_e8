#include "e8_sampler_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

/*
 * E8 hot-table sampler variant (variable time): per block, one B-bit word
 * selects the RM(1,3) component against the tau's component CDF, then each
 * of the eight product coordinates reads one B-bit word against its centre
 * class's hot table of K = 6 values and, on a miss, a second B-bit word
 * against the class's integer tail table. Thresholds compare with
 * target <= cdf. All tables are generated offline (e8/tables/); this file
 * is integer-only.
 *
 * The threshold width B is a compile-time option: E8_HOT_BITS = 64
 * (variant hot64).
 */

#include "e8_tables_geom.h"

#ifndef E8_HOT_BITS
#define E8_HOT_BITS   64
#endif

#if E8_HOT_BITS == 64
#include "e8_tables_hot64.h"
typedef uint64_t e8h_word;
typedef e8_hot64_row e8h_row;
typedef e8_hot64_tables e8h_tables;
#define e8h_read(s)             cmn_rng_read_u64le(s)
#define E8H_SAMPLE_Z            e8_sample_z_hot64
#define E8H_SAMPLE_BLOCK        e8_hot64_sample_block
#define E8H_SAMPLE_1D           e8_hot64_sample_1d
#define E8H_TABLES_GET          e8_hot64_tables_get
#define E8H_TABLES_LOGN8        e8_hot64_tables_logn8
#define E8H_TABLES_LOGN9        e8_hot64_tables_logn9
#define E8H_TABLES_LOGN10       e8_hot64_tables_logn10
#else
#error "unsupported E8_HOT_BITS"
#endif

/*
 * One product coordinate: hot table, then integer tail. The last tail
 * threshold (or the last hot threshold, if there is no tail) is the
 * maximum word, so the final return is unreachable.
 */
static inline int
e8h_sample_1d(int32_t *out, const e8h_row *row, cmn_rng *s)
{
	e8h_word u = e8h_read(s);

	for (unsigned i = 0; i < row->tiny_len; i ++) {
		if (u <= row->tiny_cdf[i]) {
			*out = row->tiny_value[i];
			return 1;
		}
	}
	u = e8h_read(s);
	for (unsigned i = 0; i < row->tail_len; i ++) {
		if (u <= row->tail_cdf[i]) {
			*out = row->tail_value[i];
			return 1;
		}
	}
	return 0;
}

/*
 * Direct coset-matched Construction-A sampler for one HAWK-E8-CM block.
 * Here M = PZ^8.  For z = tau + 2y, the E8-side coset is
 *
 *     C_tau = 2M + P tau = P tau + 2P Z^8.
 *
 * The lift y is first decomposed modulo the Construction-A lattice
 * RM(1,3)+2Z^8.  Each of the 16 RM(1,3)-labelled components is
 *
 *     P tau + 2P r + 2P(RM(1,3)+2Z^8),
 *
 * where r is a representative with the selected syndrome.  The kernel
 * lattice on the right has an orthogonal product basis, so its
 * conditional Gaussian mass and sample are products of eight shifted
 * one-dimensional integer Gaussians in x-space.  z is reconstructed
 * affinely from the component's base block; it is congruent to tau
 * modulo 2.
 */
static inline int
e8h_sample_block(int32_t *zblk, uint8_t tau, const e8h_tables *tab,
	cmn_rng *s, uint64_t *norm2_out, e8_sampler_stats *stats,
	e8_ca_sample_trace *trace)
{
	const e8h_word *cdf = tab->component_cdf[tau];
	e8h_word u = e8h_read(s);
	unsigned component;
	int32_t coords[E8_BLOCK_DIM];
	int64_t n2;

	/* The last component threshold is the maximum word. */
	for (component = 0; component < E8_CA_COSETS - 1; component ++) {
		if (u <= cdf[component]) {
			break;
		}
	}
	n2 = e8_tab_base_norm2[tau][component];
	for (unsigned v = 0; v < E8_BLOCK_DIM; v ++) {
		unsigned cls = e8_tab_coord_class[tau][component][v];
		int64_t c;

		if (!e8h_sample_1d(&coords[v], &tab->rows[cls], s)) {
			return 0;
		}
		c = coords[v];
		n2 += (int64_t)E8_CA_PRODUCT_NORM2_I * c * c
			- 2 * c * e8_tab_center_num32[cls];
	}
	if (!e8_ca_reconstruct_z_affine(zblk,
		e8_tab_base_z[tau][component], coords) || n2 < 0)
	{
		return 0;
	}
#if HAWK_E8_DEBUG_CHECKS
	if (!e8_ca_debug_check_block(zblk, tau, (uint8_t)component,
		coords, n2))
	{
		return 0;
	}
#endif
	if (norm2_out != NULL) {
		*norm2_out = (uint64_t)n2;
	}
	e8_ca_stats_add_block(stats, (uint8_t)component, (uint64_t)n2);
	if (trace != NULL && !e8_ca_fill_trace(trace, zblk, tau,
		(uint8_t)component, coords))
	{
		return 0;
	}
	return 1;
}

static int
e8h_sample_range(e8_sampler_worker *w, cmn_rng *worker_stream)
{
	const e8h_tables *tab = w->tables;

	for (size_t r = w->start; r < w->end; r ++) {
		int32_t zblk[E8_BLOCK_DIM];
		uint64_t n2;
		uint8_t tau = e8_extract_tau(w->t0, w->t1, r, w->logn);
		cmn_rng block_stream;
		cmn_rng *s = worker_stream;
		uint64_t c0, w0;
		int ok;

		if (s == NULL) {
			c0 = e8_profile_cycles_start();
			w0 = e8_profile_wall_ns();
			e8_sampler_block_rng_init(&block_stream, w->seed,
				w->logn, (uint64_t)r, tau, w->sigma_sign_bits);
			e8_profile_add(&w->profile.cycles_block_rng_init,
				&w->profile.wall_ns_block_rng_init, c0, w0);
			s = &block_stream;
		}
		c0 = e8_profile_cycles_start();
		w0 = e8_profile_wall_ns();
		ok = e8h_sample_block(zblk, tau, tab, s, &n2, &w->stats, NULL);
		e8_profile_add(&w->profile.cycles_block_sample,
			&w->profile.wall_ns_block_sample, c0, w0);
		if (worker_stream == NULL) {
			cmn_rng_counters c;

			cmn_rng_counters_get(&block_stream, &c);
			cmn_rng_counters_add(&w->rng, &c);
		}
		if (!ok) {
			return 0;
		}
		e8_write_block(w->z0, w->z1, r, zblk, w->logn);
		if (UINT64_MAX - w->norm2 < n2) {
			return 0;
		}
		w->norm2 += n2;
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
E8H_SAMPLE_Z(const void *tables, int32_t *z0, int32_t *z1,
	uint64_t *pnorm_out, const uint8_t *t0, const uint8_t *t1,
	unsigned logn, unsigned rng_mode,
	const shake_context *prefix, uint64_t prefix_permutations,
	hawk_rng rng, void *rng_context, e8_sampler_stats *stats)
{
	const e8h_tables *tab = tables;

	if (tab == NULL) {
		return 0;
	}
	return e8_sampler_run(e8h_sample_range, tab, tab->sigma_sign_bits,
		z0, z1, pnorm_out, t0, t1, logn, rng_mode,
		prefix, prefix_permutations, rng, rng_context, stats);
}

/* see hawk_e8_inner.h */
int
E8H_SAMPLE_BLOCK(const void *tables, int32_t *zblk, uint8_t tau,
	cmn_rng *stream, uint64_t *norm2_out, e8_sampler_stats *stats,
	e8_ca_sample_trace *trace)
{
	if (tables == NULL || zblk == NULL || stream == NULL) {
		return 0;
	}
	return e8h_sample_block(zblk, tau, tables, stream, norm2_out,
		stats, trace);
}

/* see hawk_e8_inner.h */
int
E8H_SAMPLE_1D(const void *tables, unsigned cls, cmn_rng *stream,
	int32_t *out)
{
	const e8h_tables *tab = tables;

	if (tab == NULL || cls >= E8_TAB_CLASSES || stream == NULL
		|| out == NULL)
	{
		return 0;
	}
	return e8h_sample_1d(out, &tab->rows[cls], stream);
}

/* see hawk_e8_inner.h */
const void *
E8H_TABLES_GET(unsigned logn)
{
	switch (logn) {
	case 8:
		return &E8H_TABLES_LOGN8;
	case 9:
		return &E8H_TABLES_LOGN9;
	case 10:
		return &E8H_TABLES_LOGN10;
	default:
		return NULL;
	}
}

#endif
