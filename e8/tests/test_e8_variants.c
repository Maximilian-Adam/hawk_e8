#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hawk_e8_inner.h"
#include "cmn_pool.h"

/*
 * Tests of every E8 sampler variant in the registry (hawk_e8_inner.h), at
 * every logn:
 *
 *   - SIGS signatures (alternately encoded and prepared key) sign through
 *     e8_sign_finish_inner() with the variant and verify with
 *     e8_verify_finish(); a few more with 4 sampler threads;
 *   - stream layout: a serial call equals the documented single stream
 *     (prefix || "HAWK-E8-CM worker sampler v2" || seed || u8 logn ||
 *     u8 0 || u64le(0) || u64le(n/4) || u64le(sigma_sign bits)) sampled
 *     block by block, and consumes exactly the bytes that stream delivers;
 *     so T = 1 equals serial by construction;
 *   - per-worker outputs are deterministic for fixed T (T = 2, 4, 8) and
 *     the worker split matches the documented ranges;
 *   - binding: with the signing RNG output held fixed, the sampler output
 *     changes when only hm, only K or only the attempt counter changes.
 */

#define MAXN   1024
#define SIGS   64
#define THREADED_SIGS   8

static const double SIGMA_SIGN[3] = { 3.592, 3.631, 3.669 };

static uint64_t
lcg_next(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

static void
lcg_rng(void *ctx, void *dst, size_t len)
{
	uint8_t *buf = dst;

	while (len -- > 0) {
		*buf ++ = (uint8_t)(lcg_next(ctx) >> 56);
	}
}

static uint64_t
sigma_bits(unsigned logn)
{
	uint64_t b;

	memcpy(&b, &SIGMA_SIGN[logn - 8], sizeof b);
	return b;
}

static void
make_message(shake_context *sc, unsigned logn, unsigned msg)
{
	static const char prefix[] = "hawk e8 variant test message";
	uint8_t buf[3];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)msg;
	buf[2] = (uint8_t)(msg >> 8);
	e8_sign_start(sc);
	shake_inject(sc, prefix, sizeof prefix - 1);
	shake_inject(sc, buf, sizeof buf);
}

static int
sign_verify(const e8_sampler_variant *v, unsigned logn)
{
	static uint8_t priv[E8_PRIVKEY_SIZE(10)], pub[E8_PUBKEY_SIZE(10)];
	static uint8_t prepared[E8_PRIVKEY_PREPARED_SIZE(10)];
	static uint8_t ktmp[E8_TMPSIZE_KEYGEN(10)], stmp[E8_TMPSIZE_SIGN(10)];
	static uint8_t vtmp[E8_TMPSIZE_VERIFY(10)], sig[E8_SIG_SIZE(10)];
	uint64_t key_state = UINT64_C(0xE8FA41A700000000) + logn;
	unsigned attempts_total = 0;

	if (!e8_keygen(logn, priv, pub, lcg_rng, &key_state,
		ktmp, sizeof ktmp)
		|| !e8_prepare_private_key(logn, prepared, priv))
	{
		fprintf(stderr, "ERR: keygen logn=%u\n", logn);
		return 0;
	}
	for (unsigned u = 0; u < SIGS + THREADED_SIGS; u ++) {
		uint64_t state = UINT64_C(0xE8FA415100000000)
			+ ((uint64_t)logn << 16) + u;
		int prep = u & 1;
		unsigned attempts = 0;
		shake_context sc;

		e8_sampler_set_thread_count(u < SIGS ? 1 : 4);
		make_message(&sc, logn, u);
		if (!e8_sign_finish_inner(logn, lcg_rng, &state, sig, &sc,
			prep ? (const void *)prepared : (const void *)priv,
			prep ? E8_PRIVKEY_PREPARED_SIZE(logn)
				: E8_PRIVKEY_SIZE(logn),
			stmp, sizeof stmp, v, &attempts, NULL)
			|| !e8_verify_finish(logn, sig, E8_SIG_SIZE(logn), &sc,
			pub, E8_PUBKEY_SIZE(logn), vtmp, sizeof vtmp))
		{
			fprintf(stderr, "ERR: variant %s logn=%u signature %u"
				" failed\n", v->name, logn, u);
			return 0;
		}
		attempts_total += attempts;
	}
	e8_sampler_set_thread_count(1);
	cmn_pool_shrink(0);
	printf("  %s: %u signatures (%u with 4 threads) verified,"
		" attempts=%u\n", v->name, SIGS + THREADED_SIGS,
		THREADED_SIGS, attempts_total);
	return 1;
}

/*
 * Stream of worker w of T for the bound per-worker mode, as documented.
 */
static void
worker_stream(cmn_rng *s, const shake_context *prefix, const uint8_t *seed,
	unsigned logn, unsigned w, size_t start, size_t end)
{
	static const char domain[] = "HAWK-E8-CM worker sampler v2";
	uint8_t label[26];

	label[0] = (uint8_t)logn;
	label[1] = (uint8_t)w;
	enc64le(label + 2, start);
	enc64le(label + 10, end);
	enc64le(label + 18, sigma_bits(logn));
	cmn_rng_init_prefixed(s, prefix, domain, sizeof domain - 1,
		seed, 40, label, sizeof label);
}

static void
random_target(uint8_t *t0, uint8_t *t1, size_t n, uint64_t *state)
{
	for (size_t u = 0; u < n; u ++) {
		t0[u] = (uint8_t)(lcg_next(state) >> 63);
		t1[u] = (uint8_t)(lcg_next(state) >> 63);
	}
}

static int
sample(const e8_sampler_variant *v, unsigned logn, unsigned threads,
	const shake_context *prefix, uint64_t rng_state,
	int32_t *z0, int32_t *z1, uint64_t *norm,
	const uint8_t *t0, const uint8_t *t1)
{
	int ok;

	e8_sampler_set_thread_count(threads);
	ok = v->sample_z(v->tables(logn), z0, z1, norm, t0, t1, logn,
		E8_SAMPLER_RNG_PER_WORKER, prefix, 0, lcg_rng, &rng_state,
		NULL);
	e8_sampler_set_thread_count(1);
	return ok;
}

/*
 * Serial = documented single stream; T workers = documented split;
 * determinism for fixed T.
 */
static int
stream_layout(const e8_sampler_variant *v, unsigned logn)
{
	static const unsigned thread_cases[] = { 1, 2, 4, 8 };
	size_t n = (size_t)1 << logn, k = n >> 2;
	static uint8_t t0[MAXN], t1[MAXN];
	static int32_t za0[MAXN], za1[MAXN], zb0[MAXN], zb1[MAXN];
	static int32_t zs0[MAXN], zs1[MAXN];
	uint64_t bits = UINT64_C(0xE8FA1A7000000000) + logn;
	uint8_t hm[64], key[48];
	shake_context prefix;
	e8_sampler_profile profile;

	random_target(t0, t1, n, &bits);
	lcg_rng(&bits, hm, sizeof hm);
	lcg_rng(&bits, key, sizeof key);
	(void)e8_sampler_prefix_init(&prefix, hm, key, sizeof key, 7);
	for (size_t c = 0; c < sizeof thread_cases / sizeof thread_cases[0];
		c ++)
	{
		unsigned T = thread_cases[c];
		uint64_t state = UINT64_C(0xE8FA1A75EED00000) + logn;
		uint64_t seed_state = state;
		uint64_t na, nb, ns = 0, delivered = 0;
		uint8_t seed[40];

		/* Expected: the documented streams, sampled directly. */
		lcg_rng(&seed_state, seed, sizeof seed);
		memset(zs0, 0, sizeof zs0);
		memset(zs1, 0, sizeof zs1);
		for (unsigned w = 0; w < T; w ++) {
			size_t start = (k * w) / T, end = (k * (w + 1)) / T;
			cmn_rng s;
			cmn_rng_counters cnt;
			uint64_t nw;

			worker_stream(&s, &prefix, seed, logn, w, start, end);
			if (!v->sample_stream(v->tables(logn), zs0, zs1, &nw,
				t0, t1, logn, start, end, &s))
			{
				return 0;
			}
			cmn_rng_counters_get(&s, &cnt);
			delivered += cnt.bytes_delivered;
			ns += nw;
		}

		e8_sampler_profile_reset();
		if (!sample(v, logn, T, &prefix, state, za0, za1, &na, t0, t1)
			|| !e8_sampler_profile_get(&profile)
			|| !sample(v, logn, T, &prefix, state, zb0, zb1, &nb,
				t0, t1))
		{
			fprintf(stderr, "ERR: variant %s sampling failed\n",
				v->name);
			return 0;
		}
		if (na != nb || memcmp(za0, zb0, n * sizeof *za0) != 0
			|| memcmp(za1, zb1, n * sizeof *za1) != 0)
		{
			fprintf(stderr, "ERR: variant %s logn=%u T=%u not"
				" deterministic\n", v->name, logn, T);
			return 0;
		}
		if (na != ns || memcmp(za0, zs0, n * sizeof *za0) != 0
			|| memcmp(za1, zs1, n * sizeof *za1) != 0
			|| profile.rng.bytes_delivered != delivered)
		{
			fprintf(stderr, "ERR: variant %s logn=%u T=%u differs"
				" from the documented streams\n",
				v->name, logn, T);
			return 0;
		}
	}
	return 1;
}

/*
 * Binding: same signing RNG output, prefix differing only in hm, only in
 * K or only in the attempt counter.
 */
static int
binding(const e8_sampler_variant *v, unsigned logn)
{
	size_t n = (size_t)1 << logn;
	static uint8_t t0[MAXN], t1[MAXN];
	static int32_t za0[MAXN], za1[MAXN], zb0[MAXN], zb1[MAXN];
	uint64_t bits = UINT64_C(0xE8FAB14D00000000) + logn;
	uint64_t state = UINT64_C(0xE8FAB14D5EED0000) + logn;
	uint8_t hm[64], key[2 * MAXN];
	shake_context base;
	uint64_t na, nb;

	random_target(t0, t1, n, &bits);
	lcg_rng(&bits, hm, sizeof hm);
	lcg_rng(&bits, key, sizeof key);
	(void)e8_sampler_prefix_init(&base, hm, key, 2 * n, 3);
	if (!sample(v, logn, 1, &base, state, za0, za1, &na, t0, t1)) {
		return 0;
	}
	for (unsigned which = 0; which < 3; which ++) {
		shake_context alt;

		if (which == 0) {
			hm[5] ^= 1;
			(void)e8_sampler_prefix_init(&alt, hm, key, 2 * n, 3);
			hm[5] ^= 1;
		} else if (which == 1) {
			key[n] ^= 0x80;
			(void)e8_sampler_prefix_init(&alt, hm, key, 2 * n, 3);
			key[n] ^= 0x80;
		} else {
			(void)e8_sampler_prefix_init(&alt, hm, key, 2 * n, 5);
		}
		if (!sample(v, logn, 1, &alt, state, zb0, zb1, &nb, t0, t1)) {
			return 0;
		}
		if (memcmp(za0, zb0, n * sizeof *za0) == 0
			&& memcmp(za1, zb1, n * sizeof *za1) == 0)
		{
			fprintf(stderr, "ERR: variant %s logn=%u sampler not"
				" bound to %s\n", v->name, logn,
				which == 0 ? "hm" : which == 1 ? "K"
				: "the attempt counter");
			return 0;
		}
	}
	if (!sample(v, logn, 1, &base, state, zb0, zb1, &nb, t0, t1)
		|| na != nb || memcmp(za0, zb0, n * sizeof *za0) != 0)
	{
		fprintf(stderr, "ERR: variant %s binding baseline not"
			" reproducible\n", v->name);
		return 0;
	}
	return 1;
}

int
main(void)
{
	for (unsigned logn = 8; logn <= 10; logn ++) {
		printf("E8 variants n=%u:\n", 1u << logn);
		for (size_t u = 0; u < e8_sampler_variant_count; u ++) {
			const e8_sampler_variant *v = &e8_sampler_variants[u];

			if (v->tables(logn) == NULL || !sign_verify(v, logn)
				|| !stream_layout(v, logn) || !binding(v, logn))
			{
				return 1;
			}
		}
		printf("  serial = documented stream, T = 2, 4, 8 = documented"
			" split, deterministic, bound: done.\n");
	}
	return 0;
}
