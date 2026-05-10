#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hawk_e8_inner.h"

#define MAXN             1024
#define SAMPLE_BOUND     2
#define SAMPLE_SIGMA     1.25
#define BLOCK_SAMPLES    4
#define FULL_SAMPLES     2

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

static void
make_random_bits(uint8_t *t0, uint8_t *t1, size_t n, uint64_t *rng_state)
{
	for (size_t u = 0; u < n; u ++) {
		t0[u] = (uint8_t)(rng_next_u64(rng_state) & 1u);
		t1[u] = (uint8_t)(rng_next_u64(rng_state) & 1u);
	}
}

static int
check_block_parity(const int32_t *zblk, uint8_t tau,
	const char *label, unsigned logn, unsigned id)
{
	for (unsigned u = 0; u < 8; u ++) {
		if ((((uint32_t)zblk[u]) & 1u) != ((tau >> u) & 1u)) {
			fprintf(stderr,
				"ERR: %s block parity mismatch"
				" logn=%u id=%u coord=%u\n",
				label, logn, id, u);
			return 0;
		}
	}
	return 1;
}

static int64_t
full_norm_from_apply(unsigned logn, const int32_t *z0, const int32_t *z1)
{
	size_t n = (size_t)1 << logn;
	static int32_t pz0[MAXN], pz1[MAXN];
	int64_t norm2 = 0;

	e8_apply_P(pz0, pz1, z0, z1, logn);
	for (size_t u = 0; u < n; u ++) {
		norm2 += (int64_t)pz0[u] * pz0[u]
			+ (int64_t)pz1[u] * pz1[u];
	}
	return norm2;
}

static int
test_tau_extraction(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	uint64_t rng_state = UINT64_C(0xE8A77A0000000000) + logn;
	static uint8_t t0[MAXN], t1[MAXN];

	make_random_bits(t0, t1, n, &rng_state);
	for (size_t r = 0; r < k; r ++) {
		uint8_t tau = e8_extract_tau(t0, t1, r, logn);
		for (unsigned u = 0; u < 4; u ++) {
			if (((tau >> u) & 1u) != t0[r + u * k]
				|| ((tau >> (u + 4)) & 1u) != t1[r + u * k])
			{
				fprintf(stderr,
					"ERR: tau extraction mismatch"
					" logn=%u r=%u coord=%u\n",
					logn, (unsigned)r, u);
				return 0;
			}
		}
	}
	return 1;
}

static int
test_block_roundtrip(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	size_t pos[] = { 0, 1, k >> 1, k - 1 };
	int32_t cases[][8] = {
		{ 0, 1, -1, 2, -2, 3, -3, 4 },
		{ 5, -4, 3, -2, 1, 0, -1, 2 },
		{ -1, -1, 1, 1, 2, -2, 0, 3 }
	};
	static int32_t z0[MAXN], z1[MAXN], got[8];

	for (size_t i = 0; i < sizeof pos / sizeof pos[0]; i ++) {
		for (size_t j = 0; j < sizeof cases / sizeof cases[0]; j ++) {
			memset(z0, 0, n * sizeof *z0);
			memset(z1, 0, n * sizeof *z1);
			e8_write_block(z0, z1, pos[i], cases[j], logn);
			memset(got, 0, sizeof got);
			e8_read_block(got, z0, z1, pos[i], logn);
			if (memcmp(got, cases[j], sizeof got) != 0) {
				fprintf(stderr,
					"ERR: E8 block write/read mismatch"
					" logn=%u pos=%u case=%u\n",
					logn, (unsigned)pos[i], (unsigned)j);
				return 0;
			}
		}
	}
	return 1;
}

static int
test_block_norm_against_full(unsigned logn,
	const int32_t *zblk, unsigned id)
{
	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;
	size_t r = id % k;
	static int32_t z0[MAXN], z1[MAXN];
	int64_t bnorm, fnorm;

	memset(z0, 0, n * sizeof *z0);
	memset(z1, 0, n * sizeof *z1);
	e8_write_block(z0, z1, r, zblk, logn);
	bnorm = e8_block_norm2(zblk);
	fnorm = full_norm_from_apply(logn, z0, z1);
	if (bnorm != fnorm) {
		fprintf(stderr,
			"ERR: E8 block/full norm mismatch"
			" logn=%u id=%u\n", logn, id);
		return 0;
	}
	return 1;
}

static int
test_all_tau_block_support(void)
{
	uint64_t rng_state = UINT64_C(0xE8B10C0000000000);

	for (unsigned tau = 0; tau < 256; tau ++) {
		for (unsigned sample = 0; sample < BLOCK_SAMPLES; sample ++) {
			int32_t zblk[8];
			unsigned id = tau * BLOCK_SAMPLES + sample;

			if (!e8_sample_block_float(zblk, (uint8_t)tau,
				SAMPLE_SIGMA, SAMPLE_BOUND,
				test_rng, &rng_state))
			{
				fprintf(stderr,
					"ERR: E8 block sampler failed"
					" tau=%u sample=%u\n", tau, sample);
				return 0;
			}
			if (!check_block_parity(zblk, (uint8_t)tau,
				"sampled", 8, id))
			{
				return 0;
			}
			if (!test_block_norm_against_full(8, zblk, id)) {
				return 0;
			}
		}
	}
	return 1;
}

static int
test_full_support_and_norm(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	uint64_t rng_state = UINT64_C(0xE8F0110000000000) + logn;
	static uint8_t t0[MAXN], t1[MAXN];
	static int32_t z0[MAXN], z1[MAXN];

	for (unsigned sample = 0; sample < FULL_SAMPLES; sample ++) {
		int64_t norm2, check_norm;

		make_random_bits(t0, t1, n, &rng_state);
		if (!e8_sample_z_float(z0, z1, &norm2, t0, t1, logn,
			SAMPLE_SIGMA, SAMPLE_BOUND, test_rng, &rng_state))
		{
			fprintf(stderr,
				"ERR: E8 full sampler failed"
				" logn=%u sample=%u\n", logn, sample);
			return 0;
		}
		for (size_t u = 0; u < n; u ++) {
			if ((((uint32_t)z0[u]) & 1u) != t0[u]
				|| (((uint32_t)z1[u]) & 1u) != t1[u])
			{
				fprintf(stderr,
					"ERR: E8 full sampler coset mismatch"
					" logn=%u sample=%u coeff=%u\n",
					logn, sample, (unsigned)u);
				return 0;
			}
		}
		check_norm = full_norm_from_apply(logn, z0, z1);
		if (norm2 != check_norm) {
			fprintf(stderr,
				"ERR: E8 full sampler norm mismatch"
				" logn=%u sample=%u\n", logn, sample);
			return 0;
		}
	}
	return 1;
}

static int
test_logn(unsigned logn)
{
	if (!test_tau_extraction(logn) || !test_block_roundtrip(logn)) {
		return 0;
	}
	if (logn == 8 && !test_all_tau_block_support()) {
		return 0;
	}
	return test_full_support_and_norm(logn);
}

int
main(void)
{
	for (unsigned logn = 8; logn <= 10; logn ++) {
		printf("E8 floating sampler n=%u: ", 1u << logn);
		fflush(stdout);
		if (!test_logn(logn)) {
			return 1;
		}
		printf("done.\n");
	}

	return 0;
}
