/*
 * HAWK hot-table sampler variants (benchmark only, variable time): the
 * skeleton of E8's 1D path (e8/src/e8_sampler_hot.c) applied to HAWK's two
 * coordinate distributions. Coordinate v of x (2n coordinates) has parity
 * p = bit v of t; it reads one B-bit word against the hot table of p (the
 * K = 6 highest-weight values of x in 2Z + p, x = 2y + p, y ~ D(-p/2,
 * sigma_sign)) and, on a miss, a second B-bit word against p's integer
 * tail, with target <= threshold. Tables: e8/tables/hawk_tables_hot*.h.
 * Integer-only. Compiled once per width: HAWK_HOT_BITS = 64 (hot64) or
 * 32 (hot32).
 */

#include "hawk_variants.h"

#ifndef HAWK_HOT_BITS
#define HAWK_HOT_BITS   64
#endif

#if HAWK_HOT_BITS == 64
#include "hawk_tables_hot64.h"
typedef uint64_t hh_word;
typedef hawk_hot64_row hh_row;
typedef hawk_hot64_tables hh_tables;
#define hh_read(s)        cmn_rng_read_u64le(s)
#define HH_RANGE          hawkv_hot64_range
#define HH_TABLES_GET     hawkv_hot64_tables_get
#define HH_TABLES_LOGN8   hawk_hot64_tables_logn8
#define HH_TABLES_LOGN9   hawk_hot64_tables_logn9
#define HH_TABLES_LOGN10  hawk_hot64_tables_logn10
#elif HAWK_HOT_BITS == 32
#include "hawk_tables_hot32.h"
typedef uint32_t hh_word;
typedef hawk_hot32_row hh_row;
typedef hawk_hot32_tables hh_tables;
#define hh_read(s)        cmn_rng_read_u32le(s)
#define HH_RANGE          hawkv_hot32_range
#define HH_TABLES_GET     hawkv_hot32_tables_get
#define HH_TABLES_LOGN8   hawk_hot32_tables_logn8
#define HH_TABLES_LOGN9   hawk_hot32_tables_logn9
#define HH_TABLES_LOGN10  hawk_hot32_tables_logn10
#else
#error "unsupported HAWK_HOT_BITS"
#endif

/*
 * One coordinate; the last tail threshold (or hot threshold, without a
 * tail) is the maximum word, so the final return is unreachable.
 */
static inline int
hh_sample_1d(int32_t *out, const hh_row *row, cmn_rng *s)
{
	hh_word u = hh_read(s);

	for (unsigned i = 0; i < row->tiny_len; i ++) {
		if (u <= row->tiny_cdf[i]) {
			*out = row->tiny_value[i];
			return 1;
		}
	}
	u = hh_read(s);
	for (unsigned i = 0; i < row->tail_len; i ++) {
		if (u <= row->tail_cdf[i]) {
			*out = row->tail_value[i];
			return 1;
		}
	}
	return 0;
}

/* see hawk_variants.h */
int
HH_RANGE(const void *tables, unsigned logn, int8_t *x, const uint8_t *t,
	size_t start, size_t end, cmn_rng *stream, uint32_t *sn)
{
	const hh_tables *tab = tables;
	uint32_t acc = 0;

	(void)logn;
	for (size_t v = start; v < end; v ++) {
		unsigned p = (t[v >> 3] >> (v & 7)) & 1;
		int32_t val;

		if (!hh_sample_1d(&val, &tab->rows[p], stream)) {
			return 0;
		}
		x[v] = (int8_t)val;
		acc += (uint32_t)(val * val);
	}
	*sn += acc;
	return 1;
}

/* see hawk_variants.h */
const void *
HH_TABLES_GET(unsigned logn)
{
	switch (logn) {
	case 8:
		return &HH_TABLES_LOGN8;
	case 9:
		return &HH_TABLES_LOGN9;
	case 10:
		return &HH_TABLES_LOGN10;
	default:
		return NULL;
	}
}
