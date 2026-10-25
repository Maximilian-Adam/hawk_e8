#ifndef HAWK_VARIANTS_H__
#define HAWK_VARIANTS_H__

/*
 * Benchmark-only HAWK sampler variants and the variant signer.
 *
 * The signer (build/hawk_sign_variant.c, derived from upstream
 * hawk_sign.c by hawk_sign_variant.patch) is upstream sign_finish_inner()
 * with its SHAKE-mode sig_gauss() call replaced by the hook
 * hawk_variant_sampler(). The hook receives exactly sig_gauss()'s
 * arguments, including upstream's already-absorbed (not flipped) prefix
 * sc_extra = hm || K || enc32le(attempt + 1), and must return the squared
 * norm of x in 2Z + t (2n int8 coordinates), as sig_gauss() does.
 *
 * Variants (hawk_sampler_variants[]):
 *
 *   name   threshold_bits  technique     sampler
 *   ref    78              ct_full_scan  upstream sig_gauss() (through
 *                                        hawk_shim.c; 4 interleaved
 *                                        instances prefix || seed || u8 j)
 *   ref-T  78              ct_full_scan  upstream table and scan, worker
 *                                        streams below
 *   hot64  64              vt_hot_table  generated hot tables
 *   hot32  32              vt_hot_table  generated hot tables
 *
 * Every variant other than ref reads a 40-byte seed from the signing RNG,
 * exactly as sig_gauss() does (so attempts = rng_calls / 2), splits the
 * 2n coordinates into T contiguous ranges [2n w / T, 2n (w+1) / T), and
 * samples range w from its own stream
 *
 *   SHAKE256(prefix || "HAWK bench sampler worker v1" || seed
 *            || u8 logn || u8 w || u64le(range_start) || u64le(range_end))
 *
 * (cmn_rng_init_prefixed()), worker 0 on the caller and the others on
 * the shared pool (cmn_pool); the caller sums the partial norms. T = 1
 * uses one stream and does not touch the pool. ref-T reads its draws as
 * sig_gauss() does (groups of five little-endian words per four
 * coordinates: four low words with the sign in bit 63, one word of four
 * 16-bit high parts), sequentially within a range, so its mapping differs
 * from upstream ref but its per-coordinate distribution is the same; the
 * hot variants read one B-bit word per coordinate plus one per tail draw.
 *
 * Statistics: the worker streams' counters are summed per call and
 * cumulatively (hawkv_rng_counters_get()).
 */

#include "hawk_inner.h"
#include "cmn_rng.h"

typedef uint32_t (*hawk_sampler_fn)(unsigned logn,
	void (*rng)(void *ctx, void *dst, size_t len), void *rng_context,
	const shake_context *sc_extra, int8_t *x, const uint8_t *t);

/*
 * Range sampler of a split variant: coordinates [start, end) of x from
 * one stream, with parity bits t; adds their squared norm to *sn.
 */
typedef int (*hawk_range_fn)(const void *tables, unsigned logn,
	int8_t *x, const uint8_t *t, size_t start, size_t end,
	cmn_rng *stream, uint32_t *sn);

typedef struct {
	const char *name;
	unsigned threshold_bits;
	const char *technique;
	hawk_range_fn range;            /* NULL for upstream ref */
	const void *(*tables)(unsigned logn);
} hawk_sampler_variant;

extern const hawk_sampler_variant hawk_sampler_variants[];
extern const size_t hawk_sampler_variant_count;

const hawk_sampler_variant *hawk_sampler_variant_find(const char *name);

/*
 * The hook called by the variant signer, and its selection: the variant
 * signer then samples with variant v and T threads (T in 1..24; ref
 * ignores T and calls upstream sig_gauss() through the shim). Selection
 * is process-global, like the E8 sampler's thread count. The default is
 * ref.
 */
extern hawk_sampler_fn hawk_variant_sampler;

int hawkv_select(const hawk_sampler_variant *v, unsigned threads);

/*
 * Sets the hook to an arbitrary sampler (tests).
 */
void hawkv_set_hook(hawk_sampler_fn fn);

/*
 * The split sampler of the selected variant with the selected thread
 * count (the hook for every variant other than ref).
 */
uint32_t hawkv_sampler_split(unsigned logn,
	void (*rng)(void *ctx, void *dst, size_t len), void *rng_context,
	const shake_context *sc_extra, int8_t *x, const uint8_t *t);

/*
 * Worker streams' counters summed over every split call since the last
 * reset (prefix permutations are not included: upstream absorbs the
 * prefix before the hook).
 */
void hawkv_rng_counters_get(cmn_rng_counters *total);

void hawkv_rng_counters_reset(void);

/*
 * The variant signer's entry points (build/hawk_sign_variant.c): renamed
 * copies of hawk_sign_start(), hawk_sign_finish(), hawk_sign_finish_alt()
 * and hawk_decode_private_key(), the decoded-key signer (as
 * hawk_shim_sign_finish_decoded()), and the copy's sig_gauss().
 */
void hawkv_sign_start(shake_context *sc_data);

int hawkv_sign_finish(unsigned logn,
	void (*rng)(void *ctx, void *dst, size_t len), void *rng_context,
	void *sig, const shake_context *sc_data,
	const void *priv, void *tmp, size_t tmp_len);

int hawkv_sign_finish_decoded(unsigned logn,
	void (*rng)(void *ctx, void *dst, size_t len), void *rng_context,
	void *sig, const shake_context *sc_data,
	const void *priv_decoded, void *tmp, size_t tmp_len);

uint32_t hawkv_sig_gauss(unsigned logn,
	void (*rng)(void *ctx, void *dst, size_t len), void *rng_context,
	const shake_context *sc_extra, int8_t *x, const uint8_t *t);

/*
 * Range samplers and tables of the split variants (hawk_sampler_hot.c,
 * compiled once per width, and hawk_sampler_ref.c), and one-coordinate
 * test hooks (parity p, fresh draw group for ref-T).
 */
int hawkv_hot64_range(const void *tables, unsigned logn, int8_t *x,
	const uint8_t *t, size_t start, size_t end, cmn_rng *stream,
	uint32_t *sn);
int hawkv_hot32_range(const void *tables, unsigned logn, int8_t *x,
	const uint8_t *t, size_t start, size_t end, cmn_rng *stream,
	uint32_t *sn);
int hawkv_ref_range(const void *tables, unsigned logn, int8_t *x,
	const uint8_t *t, size_t start, size_t end, cmn_rng *stream,
	uint32_t *sn);

const void *hawkv_hot64_tables_get(unsigned logn);
const void *hawkv_hot32_tables_get(unsigned logn);
const void *hawkv_ref_tables_get(unsigned logn);

/*
 * ref tables (upstream sig_gauss() format): hi has hi_len entries, lo has
 * lo_len, interleaved for the two parities.
 */
typedef struct {
	const uint16_t *hi;
	const uint64_t *lo;
	size_t hi_len;
	size_t lo_len;
} hawkv_ref_tables;

#endif
