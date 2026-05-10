#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hawk_e8_inner.h"

#define MAXN   1024
#define NTRIALS   200

static uint64_t rng_state = UINT64_C(0x6D1F7A9C3B42E587);

static uint32_t
next_u32(void)
{
	rng_state = rng_state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return (uint32_t)(rng_state >> 32);
}

static int32_t
next_small(void)
{
	return (int32_t)(next_u32() % 17u) - 8;
}

static int
check_p_s_consistency(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	static int32_t z0[MAXN], z1[MAXN];
	static int32_t pz0[MAXN], pz1[MAXN];
	static int32_t sz0[MAXN], sz1[MAXN];

	for (unsigned trial = 0; trial < NTRIALS; trial ++) {
		for (size_t u = 0; u < n; u ++) {
			z0[u] = next_small();
			z1[u] = next_small();
		}

		e8_apply_P(pz0, pz1, z0, z1, logn);
		e8_apply_S(sz0, sz1, z0, z1, logn);

		int64_t lhs = 0;
		int64_t rhs = 0;
		for (size_t u = 0; u < n; u ++) {
			lhs += (int64_t)pz0[u] * pz0[u]
				+ (int64_t)pz1[u] * pz1[u];
			rhs += (int64_t)z0[u] * sz0[u]
				+ (int64_t)z1[u] * sz1[u];
		}

		if (lhs != rhs) {
			fprintf(stderr,
				"ERR: <Pz,Pz> != <z,Sz> for logn=%u"
				" trial=%u (%lld != %lld)\n",
				logn, trial, (long long)lhs, (long long)rhs);
			return 0;
		}
	}

	return 1;
}

static void
poly_mul_negacyclic_i64(int64_t *d,
	const int64_t *a, const int64_t *b, size_t n)
{
	memset(d, 0, n * sizeof *d);
	for (size_t u = 0; u < n; u ++) {
		for (size_t v = 0; v < n; v ++) {
			int64_t x = a[u] * b[v];
			size_t w = u + v;
			if (w >= n) {
				w -= n;
				x = -x;
			}
			d[w] += x;
		}
	}
}

static int
check_delta_relation(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	static int64_t s00[MAXN], s01[MAXN], s10[MAXN], s11[MAXN];
	static int64_t m00[MAXN], m01[MAXN], det[MAXN];
	static int32_t delta[MAXN];

	memset(s00, 0, n * sizeof *s00);
	memset(s01, 0, n * sizeof *s01);
	memset(s10, 0, n * sizeof *s10);
	memset(s11, 0, n * sizeof *s11);

	s00[0] = 8;
	s00[k] = 4;
	s00[3 * k] = -4;
	s01[2 * k] = 4;
	s10[2 * k] = -4;
	s11[0] = 8;

	poly_mul_negacyclic_i64(m00, s00, s11, n);
	poly_mul_negacyclic_i64(m01, s01, s10, n);
	for (size_t u = 0; u < n; u ++) {
		det[u] = m00[u] - m01[u];
	}

	e8_make_delta(delta, logn);
	for (size_t u = 0; u < n; u ++) {
		if (det[u] != delta[u]) {
			fprintf(stderr,
				"ERR: det(S_n) != Delta_n for logn=%u"
				" coeff=%u (%lld != %ld)\n",
				logn, (unsigned)u,
				(long long)det[u], (long)delta[u]);
			return 0;
		}
	}

	return 1;
}

int
main(void)
{
	for (unsigned logn = 8; logn <= 10; logn ++) {
		printf("E8 math helpers n=%u: ", 1u << logn);
		fflush(stdout);
		if (!check_p_s_consistency(logn)) {
			return 1;
		}
		if (!check_delta_relation(logn)) {
			return 1;
		}
		printf("done.\n");
	}

	return 0;
}
