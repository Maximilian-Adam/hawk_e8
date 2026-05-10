#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <assert.h>

#define E8_MAXN   1024

/*
 * Return coefficient u of X^s*a in R_n = Z[X]/(X^n + 1), with
 * 0 <= s < n. Multiplication by X^s is a signed permutation.
 */
static int32_t
poly_xshift_get(const int32_t *a, size_t u, size_t s, size_t n)
{
	if (s == 0) {
		return a[u];
	}
	if (u >= s) {
		return a[u - s];
	}
	return -a[n + u - s];
}

static void
poly_set_small(int32_t *d, const int8_t *a, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		d[u] = a[u];
	}
}

static void
poly_adjoint(int32_t *d, const int32_t *a, size_t n)
{
	d[0] = a[0];
	for (size_t u = 1; u < n; u ++) {
		d[u] = -a[n - u];
	}
}

static void
poly_mul_add(int64_t *acc, const int32_t *a, const int32_t *b, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		if (a[u] == 0) {
			continue;
		}
		for (size_t v = 0; v < n; v ++) {
			if (b[v] == 0) {
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
}

static void
poly_muladj_add(int64_t *acc, const int32_t *a, const int32_t *b, size_t n)
{
	int32_t aa[E8_MAXN];

	assert(n <= E8_MAXN);
	poly_adjoint(aa, a, n);
	poly_mul_add(acc, aa, b, n);
}

static void
poly_clear_i64(int64_t *a, size_t n)
{
	memset(a, 0, n * sizeof *a);
}

static void
poly_i64_to_i32(int32_t *d, const int64_t *a, size_t n)
{
	for (size_t u = 0; u < n; u ++) {
		assert(a[u] >= INT32_MIN && a[u] <= INT32_MAX);
		d[u] = (int32_t)a[u];
	}
}

static void
make_qentry(int32_t *q, const int32_t *b0, const int32_t *b1,
	const int32_t *sb0, const int32_t *sb1, size_t n)
{
	int64_t acc[E8_MAXN];

	assert(n <= E8_MAXN);
	poly_clear_i64(acc, n);
	poly_muladj_add(acc, b0, sb0, n);
	poly_muladj_add(acc, b1, sb1, n);
	poly_i64_to_i32(q, acc, n);
}

/* see hawk_e8_inner.h */
void
e8_apply_P(int32_t *out0, int32_t *out1,
	const int32_t *z0, const int32_t *z1, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	size_t k2 = k << 1;
	size_t k3 = k + k2;

	for (size_t u = 0; u < n; u ++) {
		int32_t z0_y0 = z0[u];
		int32_t z0_y1 = poly_xshift_get(z0, u, k, n);
		int32_t z1_y0 = z1[u];
		int32_t z1_y1 = poly_xshift_get(z1, u, k, n);
		int32_t z1_y2 = poly_xshift_get(z1, u, k2, n);
		int32_t z1_y3 = poly_xshift_get(z1, u, k3, n);

		out0[u] = 2 * (z0_y0 + z0_y1)
			+ z1_y0 - z1_y1 + z1_y2 + z1_y3;
		out1[u] = z1_y0 + z1_y1 + z1_y2 + z1_y3;
	}
}

/* see hawk_e8_inner.h */
void
e8_apply_S(int32_t *out0, int32_t *out1,
	const int32_t *z0, const int32_t *z1, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	size_t k2 = k << 1;
	size_t k3 = k + k2;

	for (size_t u = 0; u < n; u ++) {
		int32_t z0_y0 = z0[u];
		int32_t z0_y1 = poly_xshift_get(z0, u, k, n);
		int32_t z0_y2 = poly_xshift_get(z0, u, k2, n);
		int32_t z0_y3 = poly_xshift_get(z0, u, k3, n);
		int32_t z1_y0 = z1[u];
		int32_t z1_y2 = poly_xshift_get(z1, u, k2, n);

		out0[u] = 8 * z0_y0 + 4 * z0_y1
			- 4 * z0_y3 + 4 * z1_y2;
		out1[u] = -4 * z0_y2 + 8 * z1_y0;
	}
}

/* see hawk_e8_inner.h */
void
e8_make_delta(int32_t *delta, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	memset(delta, 0, n * sizeof *delta);
	delta[0] = 48;
	delta[k] = 32;
	delta[3 * k] = -32;
}

/* see hawk_e8_inner.h */
void
e8_compute_qform(int32_t *q00, int32_t *q01,
	int32_t *q10, int32_t *q11, const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, unsigned logn)
{
	assert(logn >= 8 && logn <= 10);

	size_t n = (size_t)1 << logn;
	int32_t bf0[E8_MAXN], bg0[E8_MAXN];
	int32_t bF1[E8_MAXN], bG1[E8_MAXN];
	int32_t sf0[E8_MAXN], sg0[E8_MAXN];
	int32_t sF1[E8_MAXN], sG1[E8_MAXN];

	assert(n <= E8_MAXN);
	poly_set_small(bf0, f, n);
	poly_set_small(bg0, g, n);
	poly_set_small(bF1, F, n);
	poly_set_small(bG1, G, n);

	/*
	 * With B = [[f,F],[g,G]], the two columns are (f,g) and (F,G).
	 * Each Q entry is column_i^* S column_j.
	 */
	e8_apply_S(sf0, sg0, bf0, bg0, logn);
	e8_apply_S(sF1, sG1, bF1, bG1, logn);

	make_qentry(q00, bf0, bg0, sf0, sg0, n);
	make_qentry(q01, bf0, bg0, sF1, sG1, n);
	make_qentry(q10, bF1, bG1, sf0, sg0, n);
	make_qentry(q11, bF1, bG1, sF1, sG1, n);
}

#endif
