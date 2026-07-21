#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hawk_e8_inner.h"

#define MAXN          1024
#define ARITH_TRIALS  64
#define NTT_TRIALS    8

typedef struct {
	unsigned logn;
	int32_t z0_range;
	int32_t z1_range;
	uint64_t observed_w0;
	uint64_t observed_w1;
} arith_range;

typedef void (*compute_t_mod2_fn)(uint8_t *t0, uint8_t *t1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const uint8_t *h0, const uint8_t *h1, size_t n);

typedef void (*compute_inverse_w_fn)(int64_t *w0, int64_t *w1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *z0, const int32_t *z1, size_t n);

typedef struct {
	const char *name;
	compute_t_mod2_fn compute_t_mod2;
	compute_inverse_w_fn compute_inverse_w;
} arith_impl;

typedef struct {
	const char *name;
	unsigned count;
	unsigned primes[3];
} ntt_subset;

static const arith_range RANGES[] = {
	{ 8,  20, 12,  2051,  445 },
	{ 9,  22, 13,  5339,  794 },
	{ 10, 22, 13, 17416, 1702 }
};

static const ntt_subset NTT_SUBSETS[] = {
	{ "p0_p1", 2, { 0, 1, 0 } },
	{ "p0_p2", 2, { 0, 2, 0 } },
	{ "p0_p3", 2, { 0, 3, 0 } },
	{ "p1_p2", 2, { 1, 2, 0 } },
	{ "p1_p3", 2, { 1, 3, 0 } },
	{ "p2_p3", 2, { 2, 3, 0 } },
	{ "p0_p1_p2", 3, { 0, 1, 2 } },
	{ "p1_p2_p3", 3, { 1, 2, 3 } }
};

static uint64_t
rng_next_u64(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

static int32_t
random_i32_range(uint64_t *rng_state, int32_t range)
{
	uint64_t width = (uint64_t)range * 2u + 1u;

	return (int32_t)(rng_next_u64(rng_state) % width) - range;
}

static int8_t
random_basis_coeff(uint64_t *rng_state)
{
	uint64_t x = rng_next_u64(rng_state) & 31u;

	if (x == 0) {
		return -2;
	}
	if (x < 5) {
		return -1;
	}
	if (x < 27) {
		return 0;
	}
	if (x < 31) {
		return 1;
	}
	return 2;
}

static void
make_basis_like(int8_t *f, int8_t *g, int8_t *F, int8_t *G,
	size_t n, uint64_t *rng_state)
{
	for (size_t u = 0; u < n; u ++) {
		f[u] = random_basis_coeff(rng_state);
		g[u] = random_basis_coeff(rng_state);
		F[u] = random_basis_coeff(rng_state);
		G[u] = random_basis_coeff(rng_state);
	}
	if (f[0] == 0) {
		f[0] = 1;
	}
	if (G[0] == 0) {
		G[0] = 1;
	}
}

static void
make_random_bits(uint8_t *h0, uint8_t *h1, size_t n, uint64_t *rng_state)
{
	for (size_t u = 0; u < n; u ++) {
		h0[u] = (uint8_t)(rng_next_u64(rng_state) & 1u);
		h1[u] = (uint8_t)(rng_next_u64(rng_state) & 1u);
	}
}

static void
make_random_z(int32_t *z0, int32_t *z1,
	size_t n, const arith_range *range, uint64_t *rng_state)
{
	for (size_t u = 0; u < n; u ++) {
		z0[u] = random_i32_range(rng_state, range->z0_range);
		z1[u] = random_i32_range(rng_state, range->z1_range);
	}
}

static unsigned
logn_from_n(size_t n)
{
	unsigned logn = 0;
	size_t x = 1;

	while (x < n) {
		x <<= 1;
		logn ++;
	}
	return x == n ? logn : 0;
}

static void
ref_poly_mul_mod2_add(uint8_t *d,
	const int8_t *a, const uint8_t *b, size_t n)
{
	for (size_t v = 0; v < n; v ++) {
		if (b[v] == 0) {
			continue;
		}
		for (size_t u = 0; u < n; u ++) {
			if ((((uint8_t)a[u]) & 1u) == 0) {
				continue;
			}
			size_t w = u + v;
			if (w >= n) {
				w -= n;
			}
			d[w] ^= 1;
		}
	}
}

static void
ref_f2_xor_shift(uint64_t *d, const uint64_t *a,
	size_t nw, size_t shift)
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

static void
ref_f2_bitwise_mul_add(uint64_t *d,
	const uint64_t *a, const uint64_t *b, size_t n)
{
	size_t nw = n >> 6;

	for (size_t u = 0; u < nw; u ++) {
		uint64_t x = a[u];

		for (unsigned v = 0; v < 64; v ++) {
			if (((x >> v) & 1u) != 0) {
				ref_f2_xor_shift(d, b, nw, (u << 6) + v);
			}
		}
	}
}

static void
ref_compute_t_mod2(uint8_t *t0, uint8_t *t1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const uint8_t *h0, const uint8_t *h1, size_t n)
{
	memset(t0, 0, n);
	memset(t1, 0, n);
	ref_poly_mul_mod2_add(t0, f, h0, n);
	ref_poly_mul_mod2_add(t0, F, h1, n);
	ref_poly_mul_mod2_add(t1, g, h0, n);
	ref_poly_mul_mod2_add(t1, G, h1, n);
}

static void
ref_poly_mul_i8_i32_add_i64(int64_t *d,
	const int8_t *a, const int32_t *b, size_t n, int sign)
{
	for (size_t v = 0; v < n; v ++) {
		if (b[v] == 0) {
			continue;
		}
		for (size_t u = 0; u < n; u ++) {
			if (a[u] == 0) {
				continue;
			}
			size_t w = u + v;
			int64_t x = (int64_t)a[u] * b[v];
			if (w >= n) {
				w -= n;
				x = -x;
			}
			d[w] += sign < 0 ? -x : x;
		}
	}
}

static void
ref_compute_inverse_w(int64_t *w0, int64_t *w1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *z0, const int32_t *z1, size_t n)
{
	memset(w0, 0, n * sizeof *w0);
	memset(w1, 0, n * sizeof *w1);
	ref_poly_mul_i8_i32_add_i64(w0, G, z0, n, 1);
	ref_poly_mul_i8_i32_add_i64(w0, F, z1, n, -1);
	ref_poly_mul_i8_i32_add_i64(w1, g, z0, n, -1);
	ref_poly_mul_i8_i32_add_i64(w1, f, z1, n, 1);
}

static int
ref_poly_mul_i32_i32(int32_t *d,
	const int32_t *a, const int32_t *b, size_t n)
{
	int64_t acc[MAXN];

	memset(acc, 0, n * sizeof *acc);
	for (size_t v = 0; v < n; v ++) {
		if (b[v] == 0) {
			continue;
		}
		for (size_t u = 0; u < n; u ++) {
			if (a[u] == 0) {
				continue;
			}
			size_t w = u + v;
			int64_t x = (int64_t)a[u] * b[v];
			if (w >= n) {
				w -= n;
				x = -x;
			}
			acc[w] += x;
		}
	}
	for (size_t u = 0; u < n; u ++) {
		if (acc[u] < INT32_MIN || acc[u] > INT32_MAX) {
			return 0;
		}
		d[u] = (int32_t)acc[u];
	}
	return 1;
}

static void
candidate_compute_t_mod2(uint8_t *t0, uint8_t *t1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const uint8_t *h0, const uint8_t *h1, size_t n)
{
	e8_coset_f2_basis basis;

	e8_coset_f2_prepare(&basis, f, g, F, G, n);
	e8_compute_t_mod2_prepared(t0, t1, &basis, h0, h1, n);
}

static void
candidate_compute_inverse_w(int64_t *w0, int64_t *w1,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *z0, const int32_t *z1, size_t n)
{
	unsigned logn = logn_from_n(n);
	e8_inverse_w_ntt_basis basis;

	if (logn == 0) {
		memset(w0, 0xA5, n * sizeof *w0);
		memset(w1, 0x5A, n * sizeof *w1);
		return;
	}
	e8_inverse_w_ntt_prepare(&basis, f, g, F, G, logn);
	if (!e8_compute_inverse_w_ntt(w0, w1, &basis, z0, z1, logn)) {
		memset(w0, 0xA5, n * sizeof *w0);
		memset(w1, 0x5A, n * sizeof *w1);
	}
}

static const arith_impl IMPLEMENTATIONS[] = {
	{ "packed_f2_ntt_inverse", candidate_compute_t_mod2,
		candidate_compute_inverse_w }
};

static int
check_equal_u8(const char *label, const char *impl_name,
	const uint8_t *ref, const uint8_t *got,
	unsigned logn, unsigned trial, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		if (ref[u] != got[u]) {
			fprintf(stderr,
				"ERR: %s mismatch impl=%s logn=%u"
				" trial=%u coeff=%u ref=%u got=%u\n",
				label, impl_name, logn, trial, (unsigned)u,
				ref[u], got[u]);
			return 0;
		}
	}
	return 1;
}

static int
check_equal_i64(const char *label, const char *impl_name,
	const int64_t *ref, const int64_t *got,
	unsigned logn, unsigned trial, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		if (ref[u] != got[u]) {
			fprintf(stderr,
				"ERR: %s mismatch impl=%s logn=%u"
				" trial=%u coeff=%u ref=%lld got=%lld\n",
				label, impl_name, logn, trial, (unsigned)u,
				(long long)ref[u], (long long)got[u]);
			return 0;
		}
	}
	return 1;
}

static int
check_equal_i32(const char *label, const char *impl_name,
	const int32_t *ref, const int32_t *got,
	unsigned logn, unsigned trial, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		if (ref[u] != got[u]) {
			fprintf(stderr,
				"ERR: %s mismatch impl=%s logn=%u"
				" trial=%u coeff=%u ref=%ld got=%ld\n",
				label, impl_name, logn, trial, (unsigned)u,
				(long)ref[u], (long)got[u]);
			return 0;
		}
	}
	return 1;
}

static int
check_f2_window_product(unsigned logn, unsigned trial,
	const int8_t *f, const uint8_t *h0, const uint8_t *h1, size_t n)
{
	size_t nw = n >> 6;
	uint64_t a[E8_F2_MAXW], b[E8_F2_MAXW];
	uint64_t ref[E8_F2_MAXW], got[E8_F2_MAXW];
	e8_f2_window_table table;

	e8_f2_pack(a, h0, n);
	e8_f2_pack_i8_mod2(b, f, n);
	e8_f2_pack(ref, h1, n);
	memcpy(got, ref, nw * sizeof *got);
	ref_f2_bitwise_mul_add(ref, a, b, n);
	e8_f2_window_prepare(&table, b, n);
	e8_f2_window_mul_add(got, a, &table, n);
	for (size_t u = 0; u < nw; u ++) {
		if (ref[u] != got[u]) {
			fprintf(stderr,
				"ERR: f2_window mismatch logn=%u trial=%u"
				" word=%u ref=%016llx got=%016llx\n",
				logn, trial, (unsigned)u,
				(unsigned long long)ref[u],
				(unsigned long long)got[u]);
			return 0;
		}
	}
	return 1;
}

static int
check_ntt_product(unsigned logn, const ntt_subset *subset,
	const int32_t *a, const int32_t *b, const int32_t *ref,
	unsigned trial, size_t n)
{
	uint32_t ta[3][MAXN], tb[3][MAXN], tc[3][MAXN];
	const uint32_t *residues[3];
	const e8_ntt_prime *primes[3];
	int32_t got[MAXN];

	for (unsigned u = 0; u < subset->count; u ++) {
		unsigned prime_index = subset->primes[u];
		const e8_ntt_prime *prime;

		if (prime_index >= E8_NTT_PRIME_COUNT) {
			fprintf(stderr,
				"ERR: invalid NTT prime index %u\n",
				prime_index);
			return 0;
		}
		prime = &E8_NTT_PRIMES[prime_index];
		memset(tc[u], 0, n * sizeof tc[u][0]);
		e8_ntt_to_ntt(logn, ta[u], a, prime);
		e8_ntt_to_ntt(logn, tb[u], b, prime);
		e8_ntt_mul_add(logn, tc[u], ta[u], tb[u], prime);
		e8_ntt_from_ntt(logn, tc[u], prime);
		residues[u] = tc[u];
		primes[u] = prime;
	}
	if (!e8_ntt_crt_reconstruct(got, residues,
		primes, subset->count, n))
	{
		fprintf(stderr,
			"ERR: NTT CRT reconstruction failed impl=%s"
			" logn=%u trial=%u\n",
			subset->name, logn, trial);
		return 0;
	}
	return check_equal_i32("ntt_product", subset->name,
		ref, got, logn, trial, n);
}

static int
test_arith_logn(const arith_range *range)
{
	unsigned logn = range->logn;
	size_t n = (size_t)1 << logn;
	uint64_t rng_state = UINT64_C(0xE8A2178000000000) + logn;
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	uint8_t h0[MAXN], h1[MAXN];
	uint8_t ref_t0[MAXN], ref_t1[MAXN], got_t0[MAXN], got_t1[MAXN];
	int32_t z0[MAXN], z1[MAXN];
	int64_t ref_w0[MAXN], ref_w1[MAXN], got_w0[MAXN], got_w1[MAXN];
	int32_t ref_prod[MAXN];

	for (unsigned trial = 0; trial < ARITH_TRIALS; trial ++) {
		make_basis_like(f, g, F, G, n, &rng_state);
		make_random_bits(h0, h1, n, &rng_state);
		make_random_z(z0, z1, n, range, &rng_state);
		if (!check_f2_window_product(logn, trial, f, h0, h1, n)) {
			return 0;
		}
		ref_compute_t_mod2(ref_t0, ref_t1, f, g, F, G, h0, h1, n);
		ref_compute_inverse_w(ref_w0, ref_w1, f, g, F, G, z0, z1, n);

		for (size_t v = 0;
			v < sizeof IMPLEMENTATIONS / sizeof IMPLEMENTATIONS[0];
			v ++)
		{
			const arith_impl *impl = &IMPLEMENTATIONS[v];

			impl->compute_t_mod2(got_t0, got_t1,
				f, g, F, G, h0, h1, n);
			impl->compute_inverse_w(got_w0, got_w1,
				f, g, F, G, z0, z1, n);
			if (!check_equal_u8("t0", impl->name,
					ref_t0, got_t0, logn, trial, n)
				|| !check_equal_u8("t1", impl->name,
					ref_t1, got_t1, logn, trial, n)
				|| !check_equal_i64("w0", impl->name,
					ref_w0, got_w0, logn, trial, n)
				|| !check_equal_i64("w1", impl->name,
					ref_w1, got_w1, logn, trial, n))
			{
				return 0;
			}
		}
	}
	for (unsigned trial = 0; trial < NTT_TRIALS; trial ++) {
		make_random_z(z0, z1, n, range, &rng_state);
		if (!ref_poly_mul_i32_i32(ref_prod, z0, z1, n)) {
			fprintf(stderr,
				"ERR: NTT reference product overflow"
				" logn=%u trial=%u\n", logn, trial);
			return 0;
		}
		for (size_t u = 0;
			u < sizeof NTT_SUBSETS / sizeof NTT_SUBSETS[0];
			u ++)
		{
			if (!check_ntt_product(logn, &NTT_SUBSETS[u],
				z0, z1, ref_prod, trial, n))
			{
				return 0;
			}
		}
	}

	printf("E8 arithmetic differential n=%u: trials=%u"
		" ntt_trials=%u ntt_subsets=%u z0_range=%d z1_range=%d"
		" observed_w0=%llu observed_w1=%llu done.\n",
		1u << logn, ARITH_TRIALS, NTT_TRIALS,
		(unsigned)(sizeof NTT_SUBSETS / sizeof NTT_SUBSETS[0]),
		range->z0_range, range->z1_range,
		(unsigned long long)range->observed_w0,
		(unsigned long long)range->observed_w1);
	return 1;
}

int
main(void)
{
	for (size_t u = 0; u < sizeof RANGES / sizeof RANGES[0]; u ++) {
		if (!test_arith_logn(&RANGES[u])) {
			return 1;
		}
	}
	return 0;
}
