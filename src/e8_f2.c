#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <string.h>

/* see hawk_e8_inner.h */
void
e8_f2_pack(uint64_t *d, const uint8_t *a, size_t n)
{
	size_t nw = n >> 6;

	memset(d, 0, nw * sizeof *d);
	for (size_t u = 0; u < n; u ++) {
		d[u >> 6] |= (uint64_t)(a[u] & 1u) << (u & 63);
	}
}

/* see hawk_e8_inner.h */
void
e8_f2_pack_i8_mod2(uint64_t *d, const int8_t *a, size_t n)
{
	size_t nw = n >> 6;

	memset(d, 0, nw * sizeof *d);
	for (size_t u = 0; u < n; u ++) {
		d[u >> 6] |= (uint64_t)(((uint8_t)a[u]) & 1u)
			<< (u & 63);
	}
}

static void
f2_xor_shift(uint64_t *d, const uint64_t *a, size_t nw, size_t shift)
{
	size_t sw = shift >> 6;
	unsigned sb = (unsigned)shift & 63u;

	if (sb == 0) {
		for (size_t u = 0; u < nw; u ++) {
			d[(u + sw) & (nw - 1u)] ^= a[u];
		}
	} else {
		unsigned rb = 64u - sb;

		for (size_t u = 0; u < nw; u ++) {
			size_t v = (u + sw) & (nw - 1u);
			uint64_t x = a[u];

			d[v] ^= x << sb;
			d[(v + 1u) & (nw - 1u)] ^= x >> rb;
		}
	}
}

/* see hawk_e8_inner.h */
void
e8_f2_window_prepare(e8_f2_window_table *table,
	const uint64_t *b, size_t n)
{
	size_t nw = n >> 6;

	memset(table, 0, sizeof *table);
	for (unsigned v = 1; v < 16; v ++) {
		for (unsigned i = 0; i < 4; i ++) {
			if (((v >> i) & 1u) != 0) {
				f2_xor_shift(table->row[v], b, nw, i);
			}
		}
	}
}

/* see hawk_e8_inner.h */
void
e8_f2_window_mul_add(uint64_t *d, const uint64_t *a,
	const e8_f2_window_table *table, size_t n)
{
	size_t nw = n >> 6;

	for (size_t u = 0; u < nw; u ++) {
		uint64_t x = a[u];

		for (unsigned v = 0; v < 64; v += 4) {
			f2_xor_shift(d, table->row[x & 15u], nw,
				(u << 6) + v);
			x >>= 4;
		}
	}
}

/* see hawk_e8_inner.h */
void
e8_f2_unpack(uint8_t *d, const uint64_t *a, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		d[u] = (uint8_t)((a[u >> 6] >> (u & 63)) & 1u);
	}
}

/* see hawk_e8_inner.h */
void
e8_coset_f2_prepare(e8_coset_f2_basis *basis,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	size_t n)
{
	const int8_t *src[4] = { f, g, F, G };

	for (unsigned u = 0; u < 4; u ++) {
		e8_f2_pack_i8_mod2(basis->packed[u], src[u], n);
		e8_f2_window_prepare(&basis->table[u], basis->packed[u], n);
	}
	/* Four 16-row tables use 8 KB at n=1024; packed inputs add 512 B. */
}

/* see hawk_e8_inner.h */
void
e8_compute_t_mod2_prepared(uint8_t *t0, uint8_t *t1,
	const e8_coset_f2_basis *basis,
	const uint8_t *h0, const uint8_t *h1, size_t n)
{
	uint64_t ph0[E8_F2_MAXW], ph1[E8_F2_MAXW];
	uint64_t pt0[E8_F2_MAXW], pt1[E8_F2_MAXW];
	size_t nw = n >> 6;

	e8_f2_pack(ph0, h0, n);
	e8_f2_pack(ph1, h1, n);

	memset(pt0, 0, nw * sizeof *pt0);
	memset(pt1, 0, nw * sizeof *pt1);
	e8_f2_window_mul_add(pt0, ph0, &basis->table[0], n);
	e8_f2_window_mul_add(pt0, ph1, &basis->table[2], n);
	e8_f2_window_mul_add(pt1, ph0, &basis->table[1], n);
	e8_f2_window_mul_add(pt1, ph1, &basis->table[3], n);
	e8_f2_unpack(t0, pt0, n);
	e8_f2_unpack(t1, pt1, n);
}

/* see hawk_e8_inner.h */
void
e8_compute_t_mod2(uint8_t *t0, uint8_t *t1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const uint8_t *h0, const uint8_t *h1, size_t n)
{
	e8_coset_f2_basis basis;

	e8_coset_f2_prepare(&basis, f, g, F, G, n);
	e8_compute_t_mod2_prepared(t0, t1, &basis, h0, h1, n);
}

#endif
