#include <stdint.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "../hawk_e8_inner.h"

#define MAXN             1024
#define SAMPLE_BOUND     2
#define SAMPLE_SIGMA     1.25
#define BLOCK_SAMPLES    4
#define FULL_SAMPLES     2
#define CM_BLOCK_SAMPLES 8

static const int32_t CA_PRODUCT_BASIS[8][8] = {
	{ -4,  0,  0,  0,  0,  0,  0, -4 },
	{ -4,  0,  0,  0,  0,  0,  0,  4 },
	{  0, -4,  0,  0, -4,  0,  0,  0 },
	{  0, -4,  0,  0,  4,  0,  0,  0 },
	{  0,  0, -4,  0,  0, -4,  0,  0 },
	{  0,  0, -4,  0,  0,  4,  0,  0 },
	{  0,  0,  0, -4,  0,  0, -4,  0 },
	{  0,  0,  0, -4,  0,  0,  4,  0 }
};

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

static uint8_t
test_component_rep(uint8_t component)
{
	uint8_t b1 = (uint8_t)((component >> 1) & 1u);
	uint8_t b2 = (uint8_t)((component >> 2) & 1u);
	uint8_t b4 = (uint8_t)((component >> 3) & 1u);
	uint8_t b0 = (uint8_t)((component & 1u) ^ b1 ^ b2 ^ b4);

	return (uint8_t)(b0 | (b1 << 1) | (b2 << 2) | (b4 << 4));
}

static uint8_t
lift_parity_from_z(uint8_t tau, const int32_t *zblk)
{
	uint8_t p = 0;

	for (unsigned u = 0; u < 8; u ++) {
		int32_t d = zblk[u] - (int32_t)((tau >> u) & 1u);

		if ((d & 1) != 0) {
			return 0;
		}
		p |= (uint8_t)((((uint32_t)(d / 2)) & 1u) << u);
	}
	return p;
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

static void
make_tau_block(int32_t *taublk, uint8_t tau)
{
	for (unsigned u = 0; u < 8; u ++) {
		taublk[u] = (int32_t)((tau >> u) & 1u);
	}
}

static int
check_block_coset_membership(const int32_t *zblk, uint8_t tau,
	unsigned id)
{
	int32_t taublk[8], xblk[8], xtau[8], yblk[8], py[8];

	make_tau_block(taublk, tau);
	e8_block_apply_P(xblk, zblk);
	e8_block_apply_P(xtau, taublk);
	for (unsigned u = 0; u < 8; u ++) {
		int32_t d = zblk[u] - taublk[u];
		if ((d & 1) != 0) {
			fprintf(stderr,
				"ERR: CM block non-integral lift id=%u coord=%u\n",
				id, u);
			return 0;
		}
		yblk[u] = d / 2;
	}
	e8_block_apply_P(py, yblk);
	for (unsigned u = 0; u < 8; u ++) {
		if (xblk[u] - xtau[u] != 2 * py[u]) {
			fprintf(stderr,
				"ERR: CM block 2M membership failed"
				" id=%u coord=%u\n", id, u);
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
test_sigma_convention(void)
{
	double sigma = 1.25;
	double s = e8_sigma_to_rho_s(sigma);
	double norm2 = 12.0;
	double c_weight = exp(-norm2 / (2.0 * sigma * sigma));
	double rho_weight = exp(-3.141592653589793238462643383279502884
		* norm2 / (s * s));

	if (fabs(c_weight - rho_weight) > 1e-14) {
		fprintf(stderr, "ERR: sigma/rho_s convention mismatch\n");
		return 0;
	}
	return 1;
}

static int
mass_close(double a, double b, double rel_tol, double abs_tol)
{
	double scale = fmax(fabs(a), fabs(b));

	return fabs(a - b) <= abs_tol + rel_tol * scale;
}

static int
test_cache_1d_masses(void)
{
	static const double sigmas[] = { 1.25, 1.28, 1.30 };
	static const int64_t centers_num32[] = {
		-96, -65, -33, -17, -8, -1, 0,
		1, 7, 8, 17, 33, 64, 96
	};

	for (size_t si = 0; si < sizeof sigmas / sizeof sigmas[0]; si ++) {
		if (!e8_sampler_warm_cache(sigmas[si])) {
			fprintf(stderr,
				"ERR: E8 sampler cache warm-up failed"
				" sigma=%.17g\n", sigmas[si]);
			return 0;
		}
		for (size_t ci = 0;
			ci < sizeof centers_num32 / sizeof centers_num32[0];
			ci ++)
		{
			double reference, cached;

			if (!e8_sampler_cache_compare_1d_mass(sigmas[si],
				centers_num32[ci], &reference, &cached))
			{
				fprintf(stderr,
					"ERR: E8 sampler cache 1D mass"
					" comparison failed sigma=%.17g"
					" center_num32=%lld\n",
					sigmas[si],
					(long long)centers_num32[ci]);
				return 0;
			}
			if (!mass_close(reference, cached, 1e-14, 1e-15)) {
				fprintf(stderr,
					"ERR: E8 sampler cache 1D mass mismatch"
					" sigma=%.17g center_num32=%lld"
					" reference=%.17g cached=%.17g\n",
					sigmas[si],
					(long long)centers_num32[ci],
					reference, cached);
				return 0;
			}
		}
	}
	return 1;
}

static int
test_cache_component_masses(void)
{
	if (!e8_sampler_warm_cache(SAMPLE_SIGMA)) {
		fprintf(stderr, "ERR: E8 sampler cache warm-up failed\n");
		return 0;
	}

	for (unsigned tau = 0; tau < 256; tau ++) {
		for (unsigned component = 0; component < 16; component ++) {
			double reference, cached;

			if (!e8_sampler_cache_compare_component_mass(
				(uint8_t)tau, (uint8_t)component,
				SAMPLE_SIGMA, &reference, &cached))
			{
				fprintf(stderr,
					"ERR: E8 sampler cache component"
					" comparison failed tau=%u component=%u\n",
					tau, component);
				return 0;
			}
			if (!mass_close(reference, cached, 1e-12, 1e-15)) {
				fprintf(stderr,
					"ERR: E8 sampler cache component"
					" mass mismatch tau=%u component=%u"
					" reference=%.17g cached=%.17g\n",
					tau, component, reference, cached);
				return 0;
			}
		}
	}
	return 1;
}

static int
test_rm13_table(void)
{
	for (unsigned u = 0; u < 16; u ++) {
		uint8_t cu = e8_rm13_codeword(u);

		if (e8_rm13_syndrome(cu) != 0) {
			fprintf(stderr,
				"ERR: RM(1,3) codeword has nonzero syndrome"
				" index=%u\n", u);
			return 0;
		}
		for (unsigned v = u + 1; v < 16; v ++) {
			if (cu == e8_rm13_codeword(v)) {
				fprintf(stderr,
					"ERR: RM(1,3) duplicate codeword"
					" indexes=%u,%u\n", u, v);
				return 0;
			}
		}
		for (unsigned v = 0; v < 16; v ++) {
			uint8_t x = (uint8_t)(cu ^ e8_rm13_codeword(v));
			int found = 0;

			for (unsigned w = 0; w < 16; w ++) {
				found |= e8_rm13_codeword(w) == x;
			}
			if (!found) {
				fprintf(stderr,
					"ERR: RM(1,3) table not closed"
					" indexes=%u,%u\n", u, v);
				return 0;
			}
		}
	}
	return 1;
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
test_construction_a_trace_path(void)
{
	uint64_t rng_state = UINT64_C(0xE8CA7ACE00000000);
	uint8_t taus[] = { 0x00, 0x01, 0x5A, 0xA5, 0xFF };

	for (unsigned t = 0; t < sizeof taus / sizeof taus[0]; t ++) {
		uint8_t tau = taus[t];
		int32_t zblk[8], check_z[8], pz[8], xrec[8];
		int32_t taublk[8], repblk[8], offset[8], prep[8];
		uint64_t norm2;
		e8_sampler_stats stats;
		e8_ca_sample_trace trace;

		memset(&stats, 0, sizeof stats);
		memset(&trace, 0, sizeof trace);
		if (!e8_sample_block_construction_a_cm_trace(zblk, tau,
			SAMPLE_SIGMA, test_rng, &rng_state,
			&norm2, &stats, &trace))
		{
			fprintf(stderr, "ERR: CM trace sampler failed tau=%u\n",
				tau);
			return 0;
		}
		if (trace.component >= 16
			|| trace.component_codeword
				!= e8_rm13_codeword(trace.component)
			|| e8_rm13_syndrome(lift_parity_from_z(tau, zblk))
				!= trace.component)
		{
			fprintf(stderr,
				"ERR: CM trace component mismatch tau=%u\n",
				tau);
			return 0;
		}
		if (stats.blocks != 1 || stats.one_dim_samples != 8
			|| stats.construction_a_cosets[trace.component] != 1)
		{
			fprintf(stderr,
				"ERR: CM trace stats do not show product path\n");
			return 0;
		}
		e8_block_apply_P(pz, zblk);
		if (memcmp(trace.zblk, zblk, sizeof zblk) != 0
			|| memcmp(trace.xblk, pz, sizeof pz) != 0
			|| !e8_block_solve_P_checked(check_z,
				trace.xblk, tau)
			|| memcmp(check_z, zblk, sizeof zblk) != 0)
		{
			fprintf(stderr,
				"ERR: CM trace x/z bridge mismatch tau=%u\n",
				tau);
			return 0;
		}

		make_tau_block(taublk, tau);
		make_tau_block(repblk, test_component_rep(trace.component));
		e8_block_apply_P(offset, taublk);
		e8_block_apply_P(prep, repblk);
		for (unsigned u = 0; u < 8; u ++) {
			int64_t x = offset[u] + 2 * prep[u];

			for (unsigned v = 0; v < 8; v ++) {
				x += (int64_t)CA_PRODUCT_BASIS[v][u]
					* trace.product_coords[v];
			}
			if (x < INT32_MIN || x > INT32_MAX) {
				fprintf(stderr,
					"ERR: CM trace reconstruction overflow\n");
				return 0;
			}
			xrec[u] = (int32_t)x;
		}
		if (memcmp(xrec, trace.xblk, sizeof xrec) != 0
			|| norm2 != (uint64_t)e8_block_norm2(zblk))
		{
			fprintf(stderr,
				"ERR: CM trace product reconstruction failed"
				" tau=%u\n", tau);
			return 0;
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
test_all_tau_block_support_bounded(void)
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
					"ERR: bounded E8 block sampler failed"
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
test_all_tau_block_support_cm(void)
{
	uint64_t rng_state = UINT64_C(0xE8CA000000000000);
	e8_sampler_stats stats;

	if (!e8_sampler_warm_cache(SAMPLE_SIGMA)) {
		fprintf(stderr, "ERR: CM E8 cache warm-up failed\n");
		return 0;
	}
	memset(&stats, 0, sizeof stats);
	for (unsigned tau = 0; tau < 256; tau ++) {
		for (unsigned sample = 0; sample < CM_BLOCK_SAMPLES; sample ++) {
			int32_t zblk[8], xblk[8];
			uint64_t norm2;
			int64_t check_norm = 0;
			unsigned id = tau * CM_BLOCK_SAMPLES + sample;

			if (!e8_sample_block_construction_a_cm(zblk,
				(uint8_t)tau, SAMPLE_SIGMA,
				test_rng, &rng_state, &norm2, &stats))
			{
				fprintf(stderr,
					"ERR: CM E8 block sampler failed"
					" tau=%u sample=%u\n", tau, sample);
				return 0;
			}
			if (!check_block_parity(zblk, (uint8_t)tau,
				"CM sampled", 8, id)
				|| !check_block_coset_membership(zblk,
					(uint8_t)tau, id))
			{
				return 0;
			}
			e8_block_apply_P(xblk, zblk);
			for (unsigned u = 0; u < 8; u ++) {
				check_norm += (int64_t)xblk[u] * xblk[u];
			}
			if (norm2 != (uint64_t)check_norm
				|| norm2 != (uint64_t)e8_block_norm2(zblk))
			{
				fprintf(stderr,
					"ERR: CM E8 block norm mismatch id=%u\n",
					id);
				return 0;
			}
			if (!test_block_norm_against_full(8, zblk, id)) {
				return 0;
			}
		}
	}
	if (stats.blocks != 256u * CM_BLOCK_SAMPLES) {
		fprintf(stderr, "ERR: CM E8 sampler stats block mismatch\n");
		return 0;
	}
	return 1;
}

static int
test_full_support_and_norm_bounded(unsigned logn)
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
				"ERR: bounded E8 full sampler failed"
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
test_full_support_and_norm_cm(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	uint64_t rng_state = UINT64_C(0xE8CAFC0000000000) + logn;
	static uint8_t t0[MAXN], t1[MAXN];
	static int32_t z0[MAXN], z1[MAXN];
	e8_sampler_stats stats;

	memset(&stats, 0, sizeof stats);
	for (unsigned sample = 0; sample < FULL_SAMPLES; sample ++) {
		uint64_t norm2;
		int64_t check_norm;

		make_random_bits(t0, t1, n, &rng_state);
		if (!e8_sample_z_construction_a_cm(z0, z1, &norm2,
			t0, t1, logn, SAMPLE_SIGMA,
			test_rng, &rng_state, &stats))
		{
			fprintf(stderr,
				"ERR: CM E8 full sampler failed"
				" logn=%u sample=%u\n", logn, sample);
			return 0;
		}
		for (size_t u = 0; u < n; u ++) {
			if ((((uint32_t)z0[u]) & 1u) != t0[u]
				|| (((uint32_t)z1[u]) & 1u) != t1[u])
			{
				fprintf(stderr,
					"ERR: CM E8 full sampler coset mismatch"
					" logn=%u sample=%u coeff=%u\n",
					logn, sample, (unsigned)u);
				return 0;
			}
		}
		check_norm = full_norm_from_apply(logn, z0, z1);
		if (norm2 != (uint64_t)check_norm) {
			fprintf(stderr,
				"ERR: CM E8 full sampler norm mismatch"
				" logn=%u sample=%u\n", logn, sample);
			return 0;
		}
	}
	return 1;
}

static int
test_logn(unsigned logn)
{
	if (!test_sigma_convention()
		|| !test_tau_extraction(logn) || !test_block_roundtrip(logn))
	{
		return 0;
	}
	if (logn == 8
		&& (!test_rm13_table()
			|| !test_cache_1d_masses()
			|| !test_cache_component_masses()
			|| !test_construction_a_trace_path()
			|| !test_all_tau_block_support_bounded()
			|| !test_all_tau_block_support_cm()))
	{
		return 0;
	}
	return test_full_support_and_norm_bounded(logn)
		&& test_full_support_and_norm_cm(logn);
}

int
main(void)
{
	for (unsigned logn = 8; logn <= 10; logn ++) {
		printf("E8 floating/Construction-A-CM sampler n=%u: ",
			1u << logn);
		fflush(stdout);
		if (!test_logn(logn)) {
			return 1;
		}
		printf("done.\n");
	}

	return 0;
}
