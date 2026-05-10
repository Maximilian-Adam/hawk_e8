#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../hawk_e8_inner.h"

#define MAXN   1024
#define RANDOM_NORM_TRIALS   50

static uint64_t
rng_next_u64(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

static int32_t
next_small(uint64_t *state)
{
	return (int32_t)(rng_next_u64(state) % 7u) - 3;
}

static void
make_message_context(shake_context *sc_data, unsigned logn)
{
	static const char prefix[] = "synthetic e8 verifier test";
	uint8_t x = (uint8_t)logn;

	shake_init(sc_data, 256);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, &x, 1);
}

static void
make_identity_qform(unsigned logn,
	int32_t *q00, int32_t *q01, int32_t *q10, int32_t *q11)
{
	static int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	size_t n = (size_t)1 << logn;

	memset(f, 0, n);
	memset(g, 0, n);
	memset(F, 0, n);
	memset(G, 0, n);
	f[0] = 1;
	G[0] = 1;
	e8_compute_qform(q00, q01, q10, q11, f, g, F, G, logn);
}

static int
find_valid_synthetic_sig(unsigned logn, uint8_t *sig, size_t sig_len,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const uint8_t *hpub, size_t hpub_len)
{
	size_t n = (size_t)1 << logn;
	size_t salt_len = e8_salt_len(logn);
	static uint8_t salt[40];
	static int16_t s0[MAXN], s1[MAXN];

	memset(s0, 0, n * sizeof *s0);
	memset(s1, 0, n * sizeof *s1);
	for (unsigned attempt = 0; attempt < 2048; attempt ++) {
		for (size_t u = 0; u < salt_len; u ++) {
			salt[u] = (uint8_t)(0xA5u + 17u * u + attempt);
		}
		salt[0] ^= (uint8_t)attempt;
		salt[salt_len - 1] ^= (uint8_t)(attempt >> 8);
		if (!e8_encode_sig_uncompressed(logn,
			sig, sig_len, salt, s0, s1))
		{
			return 0;
		}

		shake_context sc_data;
		make_message_context(&sc_data, logn);
		if (e8_verify_uncompressed(logn, sig, sig_len, &sc_data,
			hpub, hpub_len, q00, q01, q10, q11))
		{
			return 1;
		}
	}

	fprintf(stderr,
		"ERR: could not find synthetic valid E8 signature"
		" for logn=%u\n", logn);
	return 0;
}

static int
test_uncompressed_codec(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t salt_len = e8_salt_len(logn);
	size_t sig_len = e8_sig_uncompressed_size(logn);
	static uint8_t salt[40], salt2[40], sig[40 + 4 * MAXN];
	static int16_t s0[MAXN], s1[MAXN], t0[MAXN], t1[MAXN];

	if (salt_len == 0 || sig_len != salt_len + 4 * n) {
		fprintf(stderr, "ERR: wrong E8 signature size for logn=%u\n",
			logn);
		return 0;
	}
	for (size_t u = 0; u < salt_len; u ++) {
		salt[u] = (uint8_t)(u + logn);
	}
	for (size_t u = 0; u < n; u ++) {
		s0[u] = (int16_t)((int)(u % 31) - 15);
		s1[u] = (int16_t)(15 - (int)(u % 31));
	}

	if (!e8_encode_sig_uncompressed(logn,
		sig, sig_len, salt, s0, s1))
	{
		fprintf(stderr, "ERR: E8 signature encode failed\n");
		return 0;
	}
	if (!e8_decode_sig_uncompressed(logn,
		salt2, t0, t1, sig, sig_len))
	{
		fprintf(stderr, "ERR: E8 signature decode failed\n");
		return 0;
	}
	if (memcmp(salt, salt2, salt_len) != 0
		|| memcmp(s0, t0, n * sizeof *s0) != 0
		|| memcmp(s1, t1, n * sizeof *s1) != 0)
	{
		fprintf(stderr, "ERR: E8 signature codec mismatch\n");
		return 0;
	}
	if (e8_decode_sig_uncompressed(logn,
		salt2, t0, t1, sig, sig_len - 1))
	{
		fprintf(stderr, "ERR: truncated E8 signature decoded\n");
		return 0;
	}
	if (e8_encode_sig_uncompressed(logn,
		sig, sig_len - 1, salt, s0, s1))
	{
		fprintf(stderr, "ERR: truncated E8 signature encoded\n");
		return 0;
	}

	return 1;
}

static int
test_direct_norm(unsigned logn,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11)
{
	size_t n = (size_t)1 << logn;
	static int32_t w0[MAXN], w1[MAXN], pw0[MAXN], pw1[MAXN];
	int64_t norm, pnorm;
	uint64_t rng_state = UINT64_C(0xE8D15EC700000000) + logn;

	memset(w0, 0, n * sizeof *w0);
	memset(w1, 0, n * sizeof *w1);
	w0[0] = 1;
	if (!e8_qnorm_direct(&norm, q00, q01, q10, q11, w0, w1, logn)
		|| norm != 8)
	{
		fprintf(stderr, "ERR: direct E8 qnorm mismatch for (1,0)\n");
		return 0;
	}

	memset(w0, 0, n * sizeof *w0);
	memset(w1, 0, n * sizeof *w1);
	w1[0] = 1;
	if (!e8_qnorm_direct(&norm, q00, q01, q10, q11, w0, w1, logn)
		|| norm != 8)
	{
		fprintf(stderr, "ERR: direct E8 qnorm mismatch for (0,1)\n");
		return 0;
	}

	memset(w0, 0, n * sizeof *w0);
	memset(w1, 0, n * sizeof *w1);
	w0[0] = 1;
	w1[0] = 1;
	if (!e8_qnorm_direct(&norm, q00, q01, q10, q11, w0, w1, logn)
		|| norm != 16)
	{
		fprintf(stderr,
			"ERR: direct E8 off-diagonal qnorm mismatch"
			" for (1,1)\n");
		return 0;
	}

	memset(w0, 0, n * sizeof *w0);
	memset(w1, 0, n * sizeof *w1);
	w0[2] = 1;
	w1[(n >> 1) - 1] = -1;
	if (!e8_qnorm_direct(&norm, q00, q01, q10, q11, w0, w1, logn))
	{
		fprintf(stderr, "ERR: direct E8 off-diagonal qnorm failed\n");
		return 0;
	}
	e8_apply_P(pw0, pw1, w0, w1, logn);
	pnorm = 0;
	for (size_t u = 0; u < n; u ++) {
		pnorm += (int64_t)pw0[u] * pw0[u]
			+ (int64_t)pw1[u] * pw1[u];
	}
	if (norm != pnorm) {
		fprintf(stderr,
			"ERR: direct E8 off-diagonal qnorm/Pnorm mismatch\n");
		return 0;
	}

	for (unsigned trial = 0; trial < RANDOM_NORM_TRIALS; trial ++) {
		for (size_t u = 0; u < n; u ++) {
			w0[u] = next_small(&rng_state);
			w1[u] = next_small(&rng_state);
		}
		if (!e8_qnorm_direct(&norm, q00, q01, q10, q11,
			w0, w1, logn))
		{
			fprintf(stderr,
				"ERR: direct E8 random qnorm failed"
				" for trial=%u\n", trial);
			return 0;
		}
		e8_apply_P(pw0, pw1, w0, w1, logn);
		pnorm = 0;
		for (size_t u = 0; u < n; u ++) {
			pnorm += (int64_t)pw0[u] * pw0[u]
				+ (int64_t)pw1[u] * pw1[u];
		}
		if (norm != pnorm) {
			fprintf(stderr,
				"ERR: direct E8 random qnorm/Pnorm mismatch"
				" for trial=%u\n", trial);
			return 0;
		}
	}

	return 1;
}

static int
test_verify_logn(unsigned logn)
{
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t hpub_len = (size_t)1 << (logn - 4);
	static uint8_t sig[40 + 4 * MAXN], bad[40 + 4 * MAXN];
	static uint8_t hpub[64];
	static int32_t q00[MAXN], q01[MAXN], q10[MAXN], q11[MAXN];

	make_identity_qform(logn, q00, q01, q10, q11);
	for (size_t u = 0; u < hpub_len; u ++) {
		hpub[u] = (uint8_t)(0x31u + 13u * u + logn);
	}

	if (!test_uncompressed_codec(logn)
		|| !test_direct_norm(logn, q00, q01, q10, q11)
		|| !find_valid_synthetic_sig(logn, sig, sig_len,
			q00, q01, q10, q11, hpub, hpub_len))
	{
		return 0;
	}

	shake_context sc_data;
	make_message_context(&sc_data, logn);
	if (!e8_verify_uncompressed(logn, sig, sig_len, &sc_data,
		hpub, hpub_len, q00, q01, q10, q11))
	{
		fprintf(stderr, "ERR: valid synthetic E8 signature rejected\n");
		return 0;
	}

	make_message_context(&sc_data, logn);
	if (e8_verify_uncompressed(logn, sig, sig_len - 1, &sc_data,
		hpub, hpub_len, q00, q01, q10, q11))
	{
		fprintf(stderr, "ERR: malformed E8 signature accepted\n");
		return 0;
	}

	memcpy(bad, sig, sig_len);
	size_t salt_len = e8_salt_len(logn);
	bad[salt_len + 0] = 0xFF;
	bad[salt_len + 1] = 0x7F;
	make_message_context(&sc_data, logn);
	if (e8_verify_uncompressed(logn, bad, sig_len, &sc_data,
		hpub, hpub_len, q00, q01, q10, q11))
	{
		fprintf(stderr, "ERR: tampered s0 E8 signature accepted\n");
		return 0;
	}

	memcpy(bad, sig, sig_len);
	bad[salt_len + 0] = 0x00;
	bad[salt_len + 1] = 0x80;
	make_message_context(&sc_data, logn);
	if (e8_verify_uncompressed(logn, bad, sig_len, &sc_data,
		hpub, hpub_len, q00, q01, q10, q11))
	{
		fprintf(stderr,
			"ERR: large negative s0 E8 signature accepted\n");
		return 0;
	}

	memcpy(bad, sig, sig_len);
	bad[salt_len + (2 << logn) + 0] = 0xFF;
	bad[salt_len + (2 << logn) + 1] = 0x7F;
	make_message_context(&sc_data, logn);
	if (e8_verify_uncompressed(logn, bad, sig_len, &sc_data,
		hpub, hpub_len, q00, q01, q10, q11))
	{
		fprintf(stderr, "ERR: tampered s1 E8 signature accepted\n");
		return 0;
	}

	memcpy(bad, sig, sig_len);
	bad[salt_len + (2 << logn) + 0] = 0x00;
	bad[salt_len + (2 << logn) + 1] = 0x80;
	make_message_context(&sc_data, logn);
	if (e8_verify_uncompressed(logn, bad, sig_len, &sc_data,
		hpub, hpub_len, q00, q01, q10, q11))
	{
		fprintf(stderr,
			"ERR: large negative s1 E8 signature accepted\n");
		return 0;
	}

	make_message_context(&sc_data, logn);
	if (e8_verify_uncompressed(logn, sig, sig_len, &sc_data,
		hpub, hpub_len - 1, q00, q01, q10, q11))
	{
		fprintf(stderr, "ERR: short hpub length accepted\n");
		return 0;
	}

	make_message_context(&sc_data, logn);
	if (e8_verify_uncompressed(logn, sig, sig_len, &sc_data,
		hpub, hpub_len + 1, q00, q01, q10, q11))
	{
		fprintf(stderr, "ERR: long hpub length accepted\n");
		return 0;
	}

	return 1;
}

int
main(void)
{
	for (unsigned logn = 8; logn <= 10; logn ++) {
		printf("E8 uncompressed verify n=%u: ", 1u << logn);
		fflush(stdout);
		if (!test_verify_logn(logn)) {
			return 1;
		}
		printf("done.\n");
	}

	return 0;
}
