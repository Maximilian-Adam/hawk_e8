#include "e8_sampler_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

/*
 * E8 ref sampler variant: 78-bit thresholds and constant-time full scans,
 * mirroring HAWK's sig_gauss(). Integer-only.
 *
 * Draws. Every draw is a 78-bit uniform u = hi (15 bits) || lo (63 bits),
 * read exactly as HAWK reads its draws: a group of five little-endian
 * 64-bit words gives four draws, words 0..3 holding the low parts (bits
 * 0..62; bit 63 is unused here, HAWK uses it as the sign) and word 4 the
 * four high parts in its 16-bit lanes (bits 0..14 of each). A block uses
 * nine draws (component, then eight coordinates); groups continue across
 * the blocks of one stream, and a stream's last partial group is read
 * whole, so a stream of b blocks reads ceil(9b / 4) * 40 bytes.
 *
 * Comparisons. u < T is computed as in sig_gauss(): cc = (lo - Tlo) >> 63
 * (borrow of the low parts), then (hi - Thi - cc) >> 31; every table entry
 * is compared, with no early exit, and the result is the count of entries
 * with u < T.
 *
 * Component. The secret label tau selects a row class (one of
 * E8_REF_ROW_CLASSES distinct component mass orders) and a slot-to-
 * component permutation by a masked scan over all 256 taus; the row
 * class's 15 survival thresholds are selected by a masked scan over all
 * row classes; the slot is the count of thresholds above u, and the
 * component is the slot's 4-bit field of the permutation (a shift by a
 * secret amount, constant time on x86-64).
 *
 * Coordinates. The component's base block z = tau + 2 rep(component), its
 * offset P z, its squared norm and the eight product-coordinate centres
 * c = k/4 are computed arithmetically, without table lookups. The
 * coordinate's residue k mod 4 selects one of four two-sided survival
 * tables (padded with zeros to E8_REF_1D_LEN) by a masked scan, and the
 * value is floor(k/4) + lo_value + #{j : u < T[j]}.
 *
 * No secret-dependent branch or memory index is used outside the debug
 * checks, the optional statistics and the trace. With E8_REF_COUNT_OPS,
 * the loops count their iterations (test_e8_variants checks that every
 * block runs the same counts).
 */

#include "e8_tables_ref.h"

#ifndef E8_REF_COUNT_OPS
#define E8_REF_COUNT_OPS   0
#endif

#if E8_REF_COUNT_OPS
static uint64_t e8_ref_ops[4];
#define E8_REF_OP(i)   (e8_ref_ops[i] ++)
#else
#define E8_REF_OP(i)   ((void)0)
#endif

/*
 * Draw reader: four buffered draws per group.
 */
typedef struct {
	cmn_rng *s;
	uint64_t lo[4];
	uint32_t hi[4];
	unsigned next;
} e8_ref_reader;

static inline void
e8_ref_reader_init(e8_ref_reader *rd, cmn_rng *s)
{
	rd->s = s;
	rd->next = 4;
}

static inline void
e8_ref_draw(e8_ref_reader *rd, uint32_t *hi, uint64_t *lo)
{
	if (rd->next == 4) {
		uint64_t w;

		for (unsigned k = 0; k < 4; k ++) {
			rd->lo[k] = cmn_rng_read_u64le(rd->s)
				& UINT64_C(0x7FFFFFFFFFFFFFFF);
		}
		w = cmn_rng_read_u64le(rd->s);
		for (unsigned k = 0; k < 4; k ++) {
			rd->hi[k] = (uint32_t)(w >> (k << 4)) & 0x7FFF;
		}
		rd->next = 0;
	}
	*hi = rd->hi[rd->next];
	*lo = rd->lo[rd->next];
	rd->next ++;
}

/*
 * 1 if u < (thi, tlo), else 0 (constant time).
 */
static inline uint32_t
e8_ref_lt(uint32_t hi, uint64_t lo, uint32_t thi, uint64_t tlo)
{
	uint32_t cc = (uint32_t)((lo - tlo) >> 63);

	return (hi - thi - cc) >> 31;
}

/*
 * All-ones if a == b, else 0 (a, b < 2^31).
 */
static inline uint64_t
e8_ref_eq_mask(uint32_t a, uint32_t b)
{
	return -(uint64_t)(((a ^ b) - 1) >> 31);
}

static inline unsigned
e8_ref_component(const e8_ref_tables *tab, uint8_t tau, e8_ref_reader *rd)
{
	uint64_t perm = 0, rc = 0;
	uint32_t shi[15], hi, slot = 0;
	uint64_t slo[15], lo;

	for (unsigned j = 0; j < 256; j ++) {
		uint64_t m = e8_ref_eq_mask(tau, j);

		E8_REF_OP(0);
		perm |= m & e8_ref_slot_perm[j];
		rc |= m & e8_ref_row_class[j];
	}
	memset(shi, 0, sizeof shi);
	memset(slo, 0, sizeof slo);
	for (unsigned c = 0; c < E8_REF_ROW_CLASSES; c ++) {
		uint64_t m = e8_ref_eq_mask((uint32_t)rc, c);

		for (unsigned j = 0; j < 15; j ++) {
			E8_REF_OP(1);
			shi[j] |= (uint32_t)m & tab->slot_hi[c][j];
			slo[j] |= m & tab->slot_lo[c][j];
		}
	}
	e8_ref_draw(rd, &hi, &lo);
	for (unsigned j = 0; j < 15; j ++) {
		E8_REF_OP(2);
		slot += e8_ref_lt(hi, lo, shi[j], slo[j]);
	}
	return (unsigned)(perm >> (slot << 2)) & 0xF;
}

/*
 * One coordinate with centre num32 / 32 = k / 4: value floor(k/4) +
 * lo_value[k mod 4] + #{j : u < T[k mod 4][j]}.
 */
static inline int32_t
e8_ref_coord(const e8_ref_tables *tab, int64_t center_num32,
	e8_ref_reader *rd)
{
	uint32_t kk = (uint32_t)((center_num32 + 32) >> 3);
	uint32_t res = kk & 3;
	int32_t shift = (int32_t)(kk >> 2) - 1;
	uint32_t thi[E8_REF_1D_LEN], hi, r = 0;
	uint64_t tlo[E8_REF_1D_LEN], lo, base = 0;

	memset(thi, 0, sizeof thi);
	memset(tlo, 0, sizeof tlo);
	for (unsigned c = 0; c < 4; c ++) {
		uint64_t m = e8_ref_eq_mask(res, c);

		base |= m & (uint64_t)(int64_t)tab->lo_value[c];
		for (unsigned j = 0; j < E8_REF_1D_LEN; j ++) {
			E8_REF_OP(3);
			thi[j] |= (uint32_t)m & tab->hi[c][j];
			tlo[j] |= m & tab->lo[c][j];
		}
	}
	e8_ref_draw(rd, &hi, &lo);
	for (unsigned j = 0; j < E8_REF_1D_LEN; j ++) {
		E8_REF_OP(2);
		r += e8_ref_lt(hi, lo, thi[j], tlo[j]);
	}
	return shift + (int32_t)(int64_t)base + (int32_t)r;
}

static inline int
e8_ref_sample_block(int32_t *zblk, uint8_t tau, const e8_ref_tables *tab,
	e8_ref_reader *rd, uint64_t *norm2_out, e8_sampler_stats *stats,
	e8_ca_sample_trace *trace)
{
	unsigned component = e8_ref_component(tab, tau, rd);
	uint8_t rep = e8_rm13_component_rep((uint8_t)component);
	int8_t base_z[E8_BLOCK_DIM];
	int32_t bz[E8_BLOCK_DIM], offset[E8_BLOCK_DIM];
	int32_t coords[E8_BLOCK_DIM];
	int64_t n2 = 0;

	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		bz[u] = (int32_t)((tau >> u) & 1u)
			+ 2 * (int32_t)((rep >> u) & 1u);
		base_z[u] = (int8_t)bz[u];
	}
	e8_block_apply_P(offset, bz);
	for (unsigned u = 0; u < E8_BLOCK_DIM; u ++) {
		n2 += (int64_t)offset[u] * offset[u];
	}
	for (unsigned v = 0; v < E8_BLOCK_DIM; v ++) {
		int64_t cn = e8_ca_component_center_num32(offset, v);
		int64_t c;

		coords[v] = e8_ref_coord(tab, cn, rd);
		c = coords[v];
		n2 += (int64_t)E8_CA_PRODUCT_NORM2_I * c * c - 2 * c * cn;
	}
	if (!e8_ca_reconstruct_z_affine(zblk, base_z, coords) || n2 < 0) {
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
e8_ref_sample_range(e8_sampler_worker *w, cmn_rng *worker_stream)
{
	const e8_ref_tables *tab = w->tables;
	e8_ref_reader rd;

	/* Per-block mode re-initialises the reader for each block. */
	e8_ref_reader_init(&rd, worker_stream);
	for (size_t r = w->start; r < w->end; r ++) {
		int32_t zblk[E8_BLOCK_DIM];
		uint64_t n2;
		uint8_t tau = e8_extract_tau(w->t0, w->t1, r, w->logn);
		cmn_rng block_stream;
		uint64_t c0, w0;
		int ok;

		if (worker_stream == NULL) {
			c0 = e8_profile_cycles_start();
			w0 = e8_profile_wall_ns();
			e8_sampler_block_rng_init(&block_stream, w->seed,
				w->logn, (uint64_t)r, tau, w->sigma_sign_bits);
			e8_profile_add(&w->profile.cycles_block_rng_init,
				&w->profile.wall_ns_block_rng_init, c0, w0);
			e8_ref_reader_init(&rd, &block_stream);
		}
		c0 = e8_profile_cycles_start();
		w0 = e8_profile_wall_ns();
		ok = e8_ref_sample_block(zblk, tau, tab, &rd, &n2,
			&w->stats, NULL);
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
		w->norm2 += n2;
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_sample_z_ref(const void *tables, int32_t *z0, int32_t *z1,
	uint64_t *pnorm_out, const uint8_t *t0, const uint8_t *t1,
	unsigned logn, unsigned rng_mode,
	const shake_context *prefix, uint64_t prefix_permutations,
	hawk_rng rng, void *rng_context, e8_sampler_stats *stats)
{
	const e8_ref_tables *tab = tables;

	if (tab == NULL) {
		return 0;
	}
	return e8_sampler_run(e8_ref_sample_range, tab, tab->sigma_sign_bits,
		z0, z1, pnorm_out, t0, t1, logn, rng_mode,
		prefix, prefix_permutations, rng, rng_context, stats);
}

/* see hawk_e8_inner.h */
int
e8_ref_sample_stream(const void *tables, int32_t *z0, int32_t *z1,
	uint64_t *pnorm_out, const uint8_t *t0, const uint8_t *t1,
	unsigned logn, size_t start, size_t end, cmn_rng *stream)
{
	e8_sampler_worker w;

	if (tables == NULL || stream == NULL || pnorm_out == NULL
		|| logn < 8 || logn > 10 || start > end
		|| end > ((size_t)1 << (logn - 2)))
	{
		return 0;
	}
	memset(&w, 0, sizeof w);
	w.z0 = z0;
	w.z1 = z1;
	w.t0 = t0;
	w.t1 = t1;
	w.tables = tables;
	w.logn = logn;
	w.rng_mode = E8_SAMPLER_RNG_PER_WORKER;
	w.start = start;
	w.end = end;
	if (!e8_ref_sample_range(&w, stream)) {
		return 0;
	}
	*pnorm_out = w.norm2;
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_ref_sample_1d(const void *tables, int64_t center_num32, cmn_rng *stream,
	int32_t *out)
{
	e8_ref_reader rd;

	if (tables == NULL || stream == NULL || out == NULL
		|| center_num32 < -32 || center_num32 > 72
		|| (center_num32 & 7) != 0)
	{
		return 0;
	}
	e8_ref_reader_init(&rd, stream);
	*out = e8_ref_coord(tables, center_num32, &rd);
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_ref_sample_1d_seq(const void *tables, int64_t center_num32,
	cmn_rng *stream, int32_t *out, size_t count)
{
	e8_ref_reader rd;

	if (tables == NULL || stream == NULL || out == NULL
		|| center_num32 < -32 || center_num32 > 72
		|| (center_num32 & 7) != 0)
	{
		return 0;
	}
	e8_ref_reader_init(&rd, stream);
	for (size_t u = 0; u < count; u ++) {
		out[u] = e8_ref_coord(tables, center_num32, &rd);
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_ref_sample_blocks(const void *tables, uint8_t tau, cmn_rng *stream,
	size_t count, uint64_t *norm2, e8_ca_sample_trace *trace)
{
	e8_ref_reader rd;

	if (tables == NULL || stream == NULL) {
		return 0;
	}
	e8_ref_reader_init(&rd, stream);
	for (size_t u = 0; u < count; u ++) {
		int32_t zblk[E8_BLOCK_DIM];

		if (!e8_ref_sample_block(zblk, tau, tables, &rd,
			norm2 != NULL ? &norm2[u] : NULL, NULL,
			trace != NULL ? &trace[u] : NULL))
		{
			return 0;
		}
	}
	return 1;
}

/* see hawk_e8_inner.h */
int
e8_ref_ops_get(uint64_t ops[4])
{
#if E8_REF_COUNT_OPS
	memcpy(ops, e8_ref_ops, sizeof e8_ref_ops);
	memset(e8_ref_ops, 0, sizeof e8_ref_ops);
	return 1;
#else
	(void)ops;
	return 0;
#endif
}

/* see hawk_e8_inner.h */
const void *
e8_ref_tables_get(unsigned logn)
{
	switch (logn) {
	case 8:
		return &e8_ref_tables_logn8;
	case 9:
		return &e8_ref_tables_logn9;
	case 10:
		return &e8_ref_tables_logn10;
	default:
		return NULL;
	}
}

#endif
