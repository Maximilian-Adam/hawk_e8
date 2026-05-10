#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hawk_e8_inner.h"

#define MAXN   1024
#define KEYGEN_TRIALS   20
#define RANDOM_W_TRIALS   100
#define RANDOM_W_WEIGHT   16

typedef __int128 i128;

static uint64_t
rng_next_u64(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

static void
test_rng(void *ctx, void *dst, size_t len)
{
	uint64_t *state = ctx;
	uint8_t *buf = dst;
	while (len > 0) {
		uint64_t x = rng_next_u64(state);
		for (unsigned u = 0; u < 8 && len > 0; u ++, len --) {
			*buf ++ = (uint8_t)(x >> (u << 3));
		}
	}
}

static int32_t
next_nonzero_small(uint64_t *state)
{
	int32_t x = (int32_t)(rng_next_u64(state) % 6u);
	return x < 3 ? x - 3 : x - 2;
}

static int32_t
poly_adj_get(const int32_t *a, size_t u, size_t n)
{
	return u == 0 ? a[0] : -a[n - u];
}

static void
poly_mul_i32_i32_i128(i128 *d,
	const int32_t *a, const int32_t *b, size_t n)
{
	memset(d, 0, n * sizeof *d);
	for (size_t u = 0; u < n; u ++) {
		if (a[u] == 0) {
			continue;
		}
		for (size_t v = 0; v < n; v ++) {
			if (b[v] == 0) {
				continue;
			}
			size_t w = u + v;
			i128 x = (i128)a[u] * b[v];
			if (w >= n) {
				w -= n;
				x = -x;
			}
			d[w] += x;
		}
	}
}

static void
poly_mul_i8_i32_i32(int32_t *d,
	const int8_t *a, const int32_t *b, size_t n)
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
		d[u] = (int32_t)acc[u];
	}
}

static void
poly_mul_i32_i32_add_i128(i128 *d,
	const int32_t *a, const int32_t *b, size_t n)
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
			i128 x = (i128)a[u] * b[v];
			if (w >= n) {
				w -= n;
				x = -x;
			}
			d[w] += x;
		}
	}
}

static i128
inner_i32_i32(const int32_t *a, const int32_t *b, size_t n)
{
	i128 s = 0;
	for (size_t u = 0; u < n; u ++) {
		s += (i128)a[u] * b[u];
	}
	return s;
}

static i128
inner_i32_i128(const int32_t *a, const i128 *b, size_t n)
{
	i128 s = 0;
	for (size_t u = 0; u < n; u ++) {
		s += (i128)a[u] * b[u];
	}
	return s;
}

static int
make_basis(unsigned logn, int8_t *f, int8_t *g, int8_t *F, int8_t *G,
	uint64_t *rng_state)
{
	uint8_t tmp[HAWK_TMPSIZE_KEYGEN(10)];
	uint8_t seed[40];

	return Hawk_keygen(logn, f, g, F, G, NULL, NULL, NULL, seed,
		test_rng, rng_state, tmp, sizeof tmp) == 0;
}

static int
check_adjoint(unsigned logn, const int32_t *q01, const int32_t *q10)
{
	size_t n = (size_t)1 << logn;
	for (size_t u = 0; u < n; u ++) {
		int32_t x = poly_adj_get(q01, u, n);
		if (q10[u] != x) {
			fprintf(stderr,
				"ERR: qtilde10 != qtilde01^* for logn=%u"
				" coeff=%u\n", logn, (unsigned)u);
			return 0;
		}
	}
	return 1;
}

static int
check_determinant(unsigned logn,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11)
{
	size_t n = (size_t)1 << logn;
	static i128 m00[MAXN], m01[MAXN];
	static int32_t delta[MAXN];

	poly_mul_i32_i32_i128(m00, q00, q11, n);
	poly_mul_i32_i32_i128(m01, q01, q10, n);
	e8_make_delta(delta, logn);
	for (size_t u = 0; u < n; u ++) {
		if (m00[u] - m01[u] != (i128)delta[u]) {
			fprintf(stderr,
				"ERR: det(Q_E8) != Delta_n for logn=%u"
				" coeff=%u\n", logn, (unsigned)u);
			return 0;
		}
	}
	return 1;
}

static int
check_one_public_norm(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const int32_t *w0, const int32_t *w1,
	const char *label, unsigned keynum, unsigned trial)
{
	size_t n = (size_t)1 << logn;
	static int32_t t0[MAXN], t1[MAXN], u0[MAXN], u1[MAXN];
	static int32_t su0[MAXN], su1[MAXN];
	static i128 qw0[MAXN], qw1[MAXN];

	poly_mul_i8_i32_i32(t0, f, w0, n);
	poly_mul_i8_i32_i32(t1, F, w1, n);
	for (size_t u = 0; u < n; u ++) {
		u0[u] = t0[u] + t1[u];
	}
	poly_mul_i8_i32_i32(t0, g, w0, n);
	poly_mul_i8_i32_i32(t1, G, w1, n);
	for (size_t u = 0; u < n; u ++) {
		u1[u] = t0[u] + t1[u];
	}

	e8_apply_S(su0, su1, u0, u1, logn);
	i128 lhs = inner_i32_i32(u0, su0, n)
		+ inner_i32_i32(u1, su1, n);

	memset(qw0, 0, n * sizeof *qw0);
	memset(qw1, 0, n * sizeof *qw1);
	poly_mul_i32_i32_add_i128(qw0, q00, w0, n);
	poly_mul_i32_i32_add_i128(qw0, q01, w1, n);
	poly_mul_i32_i32_add_i128(qw1, q10, w0, n);
	poly_mul_i32_i32_add_i128(qw1, q11, w1, n);

	i128 rhs = inner_i32_i128(w0, qw0, n)
		+ inner_i32_i128(w1, qw1, n);
	if (lhs != rhs) {
		fprintf(stderr,
			"ERR: <Bw,S_nBw> != <w,Q_E8w> for logn=%u"
			" key=%u %s=%u\n", logn, keynum, label, trial);
		return 0;
	}

	return 1;
}

static void
clear_w(int32_t *w0, int32_t *w1, size_t n)
{
	memset(w0, 0, n * sizeof *w0);
	memset(w1, 0, n * sizeof *w1);
}

static int
check_edge_norms(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11, unsigned keynum)
{
	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	size_t pos[] = { 1, 2, k, n >> 1, 3 * k, n - 1 };
	static int32_t w0[MAXN], w1[MAXN];
	unsigned trial = 0;

	clear_w(w0, w1, n);
	w0[0] = 1;
	if (!check_one_public_norm(logn, f, g, F, G, q00, q01, q10, q11,
		w0, w1, "edge", keynum, trial ++))
	{
		return 0;
	}

	clear_w(w0, w1, n);
	w1[0] = 1;
	if (!check_one_public_norm(logn, f, g, F, G, q00, q01, q10, q11,
		w0, w1, "edge", keynum, trial ++))
	{
		return 0;
	}

	for (size_t u = 0; u < sizeof pos / sizeof pos[0]; u ++) {
		clear_w(w0, w1, n);
		w0[pos[u]] = 1;
		if (!check_one_public_norm(logn, f, g, F, G,
			q00, q01, q10, q11, w0, w1,
			"edge", keynum, trial ++))
		{
			return 0;
		}

		clear_w(w0, w1, n);
		w1[pos[u]] = 1;
		if (!check_one_public_norm(logn, f, g, F, G,
			q00, q01, q10, q11, w0, w1,
			"edge", keynum, trial ++))
		{
			return 0;
		}
	}

	clear_w(w0, w1, n);
	w0[0] = 1;
	w0[k] = -1;
	w0[n >> 1] = 1;
	w1[1] = -1;
	w1[k + 3] = 1;
	if (!check_one_public_norm(logn, f, g, F, G, q00, q01, q10, q11,
		w0, w1, "edge", keynum, trial ++))
	{
		return 0;
	}

	clear_w(w0, w1, n);
	w0[2] = -1;
	w0[n - 3] = 1;
	w1[0] = 1;
	w1[3 * k] = -1;
	w1[n - 1] = 1;
	if (!check_one_public_norm(logn, f, g, F, G, q00, q01, q10, q11,
		w0, w1, "edge", keynum, trial ++))
	{
		return 0;
	}

	return 1;
}

static void
make_random_w(int32_t *w0, int32_t *w1, size_t n, uint64_t *rng_state)
{
	clear_w(w0, w1, n);
	for (unsigned u = 0; u < RANDOM_W_WEIGHT; u ++) {
		w0[rng_next_u64(rng_state) % n] += next_nonzero_small(rng_state);
		w1[rng_next_u64(rng_state) % n] += next_nonzero_small(rng_state);
	}
}

static int
check_random_norms(unsigned logn,
	const int8_t *f, const int8_t *g, const int8_t *F, const int8_t *G,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	uint64_t *rng_state, unsigned keynum)
{
	size_t n = (size_t)1 << logn;
	static int32_t w0[MAXN], w1[MAXN];

	for (unsigned trial = 0; trial < RANDOM_W_TRIALS; trial ++) {
		make_random_w(w0, w1, n, rng_state);
		if (!check_one_public_norm(logn, f, g, F, G,
			q00, q01, q10, q11, w0, w1,
			"random", keynum, trial))
		{
			return 0;
		}
	}

	return 1;
}

static int
test_logn(unsigned logn)
{
	static int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	static int32_t q00[MAXN], q01[MAXN], q10[MAXN], q11[MAXN];
	uint64_t rng_state = UINT64_C(0xE809D97C3A4B1001) + logn;

	for (unsigned keynum = 0; keynum < KEYGEN_TRIALS; keynum ++) {
		if (!make_basis(logn, f, g, F, G, &rng_state)) {
			fprintf(stderr,
				"ERR: Hawk_keygen failed for logn=%u key=%u\n",
				logn, keynum);
			return 0;
		}

		e8_compute_qform(q00, q01, q10, q11, f, g, F, G, logn);
		if (!check_adjoint(logn, q01, q10)) {
			return 0;
		}
		if (!check_determinant(logn, q00, q01, q10, q11)) {
			return 0;
		}
		if (!check_edge_norms(logn, f, g, F, G,
			q00, q01, q10, q11, keynum))
		{
			return 0;
		}
		if (!check_random_norms(logn, f, g, F, G,
			q00, q01, q10, q11, &rng_state, keynum))
		{
			return 0;
		}
	}

	return 1;
}

int
main(void)
{
	for (unsigned logn = 8; logn <= 10; logn ++) {
		printf("E8 public form n=%u: ", 1u << logn);
		fflush(stdout);
		if (!test_logn(logn)) {
			return 1;
		}
		printf("done.\n");
	}

	return 0;
}
