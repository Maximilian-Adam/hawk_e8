/*
 * Derived from upstream hawk_sign.c, modified for benchmarking.
 *
 * HAWK ref-T range sampler: the per-coordinate computation of upstream
 * sig_gauss() (non-AVX2 path: 78-bit table, constant-time full scan,
 * sign bit, parity column selected by mask), on the generated copy of
 * upstream's tables (e8/tables/hawk_tables_ref.h, equal to upstream's;
 * checked by test_hawk_variants), reading one worker stream sequentially:
 * coordinate start + 4i + k uses word k (low part and sign) and lane k of
 * word 4 (high part) of the stream's i-th group of five little-endian
 * words. A range whose length is not a multiple of 4 reads its last group
 * whole. Integer-only.
 */

#include "hawk_variants.h"
#include "hawk_tables_ref.h"

/* see hawk_variants.h */
int
hawkv_ref_range(const void *tables, unsigned logn, int8_t *x,
	const uint8_t *t, size_t start, size_t end, cmn_rng *stream,
	uint32_t *sn)
{
	const hawkv_ref_tables *tab = tables;
	const uint16_t *tab_hi = tab->hi;
	const uint64_t *tab_lo = tab->lo;
	size_t hi_len = tab->hi_len, lo_len = tab->lo_len;
	uint32_t acc = 0;

	(void)logn;
	for (size_t u = start; u < end; u += 4) {
		uint8_t buf[40];

		cmn_rng_read_bytes(stream, buf, sizeof buf);
		for (size_t k = 0; k < 4 && u + k < end; k ++) {
			size_t v = u + k;
			uint64_t lo = dec64le(buf + (k << 3));
			uint32_t hi = dec16le(buf + 32 + (k << 1));

			/* Extract sign bit. */
			uint32_t neg = -(uint32_t)(lo >> 63);
			lo &= 0x7FFFFFFFFFFFFFFF;
			hi &= 0x7FFF;

			/* Use even or odd column depending on
			   parity of t. */
			uint32_t pbit = (t[v >> 3] >> (v & 7)) & 1;
			uint64_t p_odd = -(uint64_t)pbit;
			uint32_t p_oddw = (uint32_t)p_odd;

			uint32_t r = 0;
			for (size_t i = 0; i < hi_len; i += 2) {
				uint64_t tlo0 = tab_lo[i + 0];
				uint64_t tlo1 = tab_lo[i + 1];
				uint64_t tlo = tlo0
					^ (p_odd & (tlo0 ^ tlo1));
				uint32_t cc =
					(uint32_t)((lo - tlo) >> 63);
				uint32_t thi0 = tab_hi[i + 0];
				uint32_t thi1 = tab_hi[i + 1];
				uint32_t thi = thi0
					^ (p_oddw & (thi0 ^ thi1));
				r += (hi - thi - cc) >> 31;
			}
			uint32_t hinz = (hi - 1) >> 31;
			for (size_t i = hi_len; i < lo_len; i += 2) {
				uint64_t tlo0 = tab_lo[i + 0];
				uint64_t tlo1 = tab_lo[i + 1];
				uint64_t tlo = tlo0
					^ (p_odd & (tlo0 ^ tlo1));
				uint32_t cc =
					(uint32_t)((lo - tlo) >> 63);
				r += hinz & cc;
			}

			/* Multiply by 2 and apply parity. */
			r = (r << 1) - p_oddw;

			/* Apply sign bit. */
			r = (r ^ neg) - neg;

			x[v] = (int8_t)*(int32_t *)&r;
			acc += r * r;
		}
	}
	*sn += acc;
	return 1;
}

#define HAWKV_REF_TABLES(logn) { \
	hawk_ref_hi_logn ## logn, hawk_ref_lo_logn ## logn, \
	sizeof hawk_ref_hi_logn ## logn / sizeof(uint16_t), \
	sizeof hawk_ref_lo_logn ## logn / sizeof(uint64_t) }

static const hawkv_ref_tables hawkv_ref_tables_logn[3] = {
	HAWKV_REF_TABLES(8), HAWKV_REF_TABLES(9), HAWKV_REF_TABLES(10)
};

/* see hawk_variants.h */
const void *
hawkv_ref_tables_get(unsigned logn)
{
	return logn >= 8 && logn <= 10 ? &hawkv_ref_tables_logn[logn - 8]
		: NULL;
}
