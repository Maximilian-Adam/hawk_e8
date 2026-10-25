#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hawk_e8_inner.h"
#include "hawk_shim.h"
#include "hawk_variants.h"
#include "cmn_pool.h"
#include "hawk_tables_hot64.h"
#include "hawk_tables_hot32.h"

/*
 * Tests of the benchmark-only HAWK variants (compare/hawk_variants/):
 *
 *   - the generated 78-bit tables equal upstream sig_gauss()'s;
 *   - with the hook set to upstream sig_gauss() (through the shim), the
 *     variant signer's signatures are byte-identical to hawk_sign_finish()
 *     (encoded key) and hawk_shim_sign_finish_decoded() (decoded key), so
 *     the signer is otherwise upstream; the signer's own copy of
 *     sig_gauss() also equals the shim's;
 *   - the prefix the signer gives the hook for attempt i equals E8's
 *     e8_sampler_prefix_init(hm, K, 2i + 1), for K = kgseed (encoded) and
 *     f || g (decoded): both schemes bind their sampler streams
 *     identically to upstream HAWK;
 *   - binding: with the signing RNG output held fixed, every split
 *     variant's output changes when only hm, only K or only the attempt
 *     counter changes;
 *   - every variant signs SIGS messages (encoded and decoded keys) that
 *     verify with hawk_verify_finish(), serially and with 4 threads;
 *   - every split variant equals its documented worker streams for
 *     T = 1, 2, 4, 8, 16 (so T = 1 is the serial variant) and is
 *     deterministic for fixed T;
 *   - a coarse distribution check of each 1D table (hot64, hot32 and the
 *     ref-T mapping) against the moments implied by the integer tables.
 */

#define MAXN           1024
#define SIGS           64
#define THREADED_SIGS  8
#define DIST_SAMPLES   200000
#define MAX_CAPTURE    8

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

static void
make_message(shake_context *sc, unsigned logn, unsigned msg)
{
	static const char prefix[] = "hawk variant test message";
	uint8_t buf[3];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)msg;
	buf[2] = (uint8_t)(msg >> 8);
	hawk_sign_start(sc);
	shake_inject(sc, prefix, sizeof prefix - 1);
	shake_inject(sc, buf, sizeof buf);
}

static int
same_state(const shake_context *a, const shake_context *b)
{
	return memcmp(a->A, b->A, sizeof a->A) == 0
		&& a->dptr == b->dptr && a->rate == b->rate;
}

static uint8_t priv[HAWK_PRIVKEY_SIZE(10)], pub[HAWK_PUBKEY_SIZE(10)];
static uint8_t priv_dec[HAWK_PRIVKEY_DECODED_SIZE(10)];
static uint8_t ktmp[HAWK_TMPSIZE_KEYGEN(10)], stmp[HAWK_TMPSIZE_SIGN(10)];
static uint8_t vtmp[HAWK_TMPSIZE_VERIFY(10)];
static uint8_t sig_a[HAWK_SIG_SIZE(10)], sig_b[HAWK_SIG_SIZE(10)];

static int
keygen(unsigned logn, unsigned key)
{
	uint64_t state = UINT64_C(0x4A4B5EED00000000)
		+ ((uint64_t)logn << 16) + key;

	if (!hawk_keygen(logn, priv, pub, lcg_rng, &state,
		ktmp, sizeof ktmp))
	{
		fprintf(stderr, "ERR: hawk_keygen logn=%u\n", logn);
		return 0;
	}
	hawk_decode_private_key(logn, priv_dec, priv);
	return 1;
}

static int
tables_equal_upstream(unsigned logn)
{
	const hawkv_ref_tables *g = hawkv_ref_tables_get(logn);
	const uint16_t *hi;
	const uint64_t *lo;
	size_t hi_len, lo_len;

	if (!hawk_shim_sig_gauss_tables(logn, &hi, &hi_len, &lo, &lo_len)
		|| g == NULL || g->hi_len != hi_len || g->lo_len != lo_len
		|| memcmp(g->hi, hi, hi_len * sizeof *hi) != 0
		|| memcmp(g->lo, lo, lo_len * sizeof *lo) != 0)
	{
		fprintf(stderr, "ERR: generated HAWK 78-bit table differs from"
			" upstream logn=%u\n", logn);
		return 0;
	}
	return 1;
}

/*
 * The variant signer with the upstream sampler is upstream signing.
 */
static int
signer_is_upstream(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	static int8_t xa[2 * MAXN], xb[2 * MAXN];
	static uint8_t t[MAXN / 4];

	hawkv_set_hook(hawk_shim_sig_gauss);
	for (unsigned key = 0; key < 2; key ++) {
		if (!keygen(logn, key)) {
			return 0;
		}
		for (unsigned msg = 0; msg < 8; msg ++) {
			uint64_t seed = UINT64_C(0x4A4B51C000000000)
				+ ((uint64_t)logn << 24) + (key << 8) + msg;
			uint64_t sa = seed, sb = seed;
			shake_context sc;

			make_message(&sc, logn, msg);
			if (!hawk_sign_finish(logn, lcg_rng, &sa, sig_a, &sc,
				priv, stmp, sizeof stmp)
				|| !hawkv_sign_finish(logn, lcg_rng, &sb, sig_b, &sc,
				priv, stmp, sizeof stmp)
				|| sa != sb
				|| memcmp(sig_a, sig_b, HAWK_SIG_SIZE(logn)) != 0)
			{
				fprintf(stderr, "ERR: variant signer differs from"
					" hawk_sign_finish logn=%u\n", logn);
				return 0;
			}
			sa = sb = seed + 1;
			if (!hawk_shim_sign_finish_decoded(logn, lcg_rng, &sa,
				sig_a, &sc, priv_dec, stmp, sizeof stmp)
				|| !hawkv_sign_finish_decoded(logn, lcg_rng, &sb,
				sig_b, &sc, priv_dec, stmp, sizeof stmp)
				|| sa != sb
				|| memcmp(sig_a, sig_b, HAWK_SIG_SIZE(logn)) != 0)
			{
				fprintf(stderr, "ERR: variant signer differs from"
					" the decoded-key upstream signer"
					" logn=%u\n", logn);
				return 0;
			}
		}
	}

	/* The signer's copy of sig_gauss() is upstream's. */
	for (unsigned rep = 0; rep < 4; rep ++) {
		uint64_t sa = UINT64_C(0x4A4B5160A0000000) + rep, sb = sa;
		uint32_t na, nb;

		lcg_rng(&sa, t, n >> 2);
		sb = sa;
		na = hawk_shim_sig_gauss(logn, lcg_rng, &sa, NULL, xa, t);
		nb = hawkv_sig_gauss(logn, lcg_rng, &sb, NULL, xb, t);
		if (na != nb || memcmp(xa, xb, 2 * n) != 0) {
			fprintf(stderr, "ERR: hawkv_sig_gauss differs\n");
			return 0;
		}
	}
	return 1;
}

static shake_context captured[MAX_CAPTURE];
static unsigned captured_count;

static uint32_t
capture_hook(unsigned logn, void (*rng)(void *ctx, void *dst, size_t len),
	void *rng_context, const shake_context *sc_extra, int8_t *x,
	const uint8_t *t)
{
	if (captured_count < MAX_CAPTURE) {
		captured[captured_count] = *sc_extra;
	}
	captured_count ++;
	return hawk_shim_sig_gauss(logn, rng, rng_context, sc_extra, x, t);
}

/*
 * hm = SHAKE256(message || hpub) for the key in priv.
 */
static void
compute_hm(unsigned logn, uint8_t *hm, const shake_context *sc_data)
{
	size_t n = (size_t)1 << logn;
	size_t seed_len = 8 + ((size_t)1 << (logn - 5));
	shake_context sc = *sc_data;

	shake_inject(&sc, priv + seed_len + (n >> 2), (size_t)1 << (logn - 4));
	shake_flip(&sc);
	shake_extract(&sc, hm, 64);
}

static int
prefix_matches_e8(unsigned logn)
{
	size_t n = (size_t)1 << logn;
	size_t seed_len = 8 + ((size_t)1 << (logn - 5));
	unsigned checked = 0;

	if (!keygen(logn, 7)) {
		return 0;
	}
	hawkv_set_hook(capture_hook);
	for (unsigned msg = 0; msg < 16; msg ++) {
		for (unsigned dec = 0; dec < 2; dec ++) {
			uint64_t state = UINT64_C(0x4A4B9F1E00000000)
				+ ((uint64_t)logn << 16) + 2 * msg + dec;
			shake_context sc;
			uint8_t hm[64];

			make_message(&sc, logn, msg);
			captured_count = 0;
			if (!(dec ? hawkv_sign_finish_decoded(logn, lcg_rng,
				&state, sig_a, &sc, priv_dec, stmp, sizeof stmp)
				: hawkv_sign_finish(logn, lcg_rng, &state,
				sig_a, &sc, priv, stmp, sizeof stmp)))
			{
				fprintf(stderr, "ERR: capture signing failed"
					" logn=%u\n", logn);
				return 0;
			}
			compute_hm(logn, hm, &sc);
			for (unsigned i = 0; i < captured_count
				&& i < MAX_CAPTURE; i ++)
			{
				shake_context e8p;

				(void)e8_sampler_prefix_init(&e8p, hm,
					dec ? (const void *)priv_dec
						: (const void *)priv,
					dec ? 2 * n : seed_len, 2 * i + 1);
				if (!same_state(&e8p, &captured[i])) {
					fprintf(stderr, "ERR: E8 prefix differs"
						" from HAWK's logn=%u attempt=%u"
						" decoded=%u\n", logn, i, dec);
					return 0;
				}
				checked ++;
			}
		}
	}
	hawkv_set_hook(hawk_shim_sig_gauss);
	printf("  prefix: %u attempts, E8 prefix = upstream HAWK prefix"
		" (encoded and decoded keys)\n", checked);
	return 1;
}

static void
random_t(uint8_t *t, size_t len, uint64_t *state)
{
	lcg_rng(state, t, len);
}

static int
binding(const hawk_sampler_variant *v, unsigned logn)
{
	size_t n = (size_t)1 << logn;
	static int8_t xa[2 * MAXN], xb[2 * MAXN];
	static uint8_t t[MAXN / 4], key[2 * MAXN];
	uint64_t bits = UINT64_C(0x4A4BB14D00000000) + logn;
	uint8_t hm[64];
	shake_context base;
	uint32_t na, nb;
	uint64_t s;

	random_t(t, n >> 2, &bits);
	lcg_rng(&bits, hm, sizeof hm);
	lcg_rng(&bits, key, 2 * n);
	(void)e8_sampler_prefix_init(&base, hm, key, 2 * n, 3);
	hawkv_select(v, 1);
	s = 99;
	na = hawk_variant_sampler(logn, lcg_rng, &s, &base, xa, t);
	for (unsigned which = 0; which < 3; which ++) {
		shake_context alt;

		if (which == 0) {
			hm[0] ^= 4;
			(void)e8_sampler_prefix_init(&alt, hm, key, 2 * n, 3);
			hm[0] ^= 4;
		} else if (which == 1) {
			key[1] ^= 1;
			(void)e8_sampler_prefix_init(&alt, hm, key, 2 * n, 3);
			key[1] ^= 1;
		} else {
			(void)e8_sampler_prefix_init(&alt, hm, key, 2 * n, 5);
		}
		s = 99;
		nb = hawk_variant_sampler(logn, lcg_rng, &s, &alt, xb, t);
		(void)nb;
		if (memcmp(xa, xb, 2 * n) == 0) {
			fprintf(stderr, "ERR: HAWK variant %s logn=%u not bound"
				" to %s\n", v->name, logn, which == 0 ? "hm"
				: which == 1 ? "K" : "the attempt counter");
			return 0;
		}
	}
	s = 99;
	nb = hawk_variant_sampler(logn, lcg_rng, &s, &base, xb, t);
	if (na != nb || memcmp(xa, xb, 2 * n) != 0) {
		fprintf(stderr, "ERR: HAWK variant %s logn=%u binding baseline"
			" not reproducible\n", v->name, logn);
		return 0;
	}
	return 1;
}

static int
sign_verify(const hawk_sampler_variant *v, unsigned logn)
{
	unsigned count = 0;

	if (!keygen(logn, 3)) {
		return 0;
	}
	for (unsigned u = 0; u < SIGS + THREADED_SIGS; u ++) {
		uint64_t state = UINT64_C(0x4A4B515100000000)
			+ ((uint64_t)logn << 16) + u;
		shake_context sc;
		int ok;

		hawkv_select(v, u < SIGS ? 1 : 4);
		make_message(&sc, logn, u);
		ok = (u & 1) ? hawkv_sign_finish_decoded(logn, lcg_rng, &state,
			sig_a, &sc, priv_dec, stmp, sizeof stmp)
			: hawkv_sign_finish(logn, lcg_rng, &state, sig_a, &sc,
			priv, stmp, sizeof stmp);
		if (!ok || !hawk_verify_finish(logn, sig_a, HAWK_SIG_SIZE(logn),
			&sc, pub, HAWK_PUBKEY_SIZE(logn), vtmp, sizeof vtmp))
		{
			fprintf(stderr, "ERR: HAWK variant %s logn=%u signature"
				" %u failed\n", v->name, logn, u);
			return 0;
		}
		count ++;
	}
	hawkv_select(&hawk_sampler_variants[0], 1);
	cmn_pool_shrink(0);
	printf("  %s: %u signatures (%u with 4 threads) verified\n",
		v->name, count, THREADED_SIGS);
	return 1;
}

/*
 * Split variant = documented worker streams, for T = 1, 2, 4, 8, 16.
 */
static int
stream_layout(const hawk_sampler_variant *v, unsigned logn)
{
	static const unsigned thread_cases[] = { 1, 2, 4, 8, 16 };
	static const char domain[] = "HAWK bench sampler worker v1";
	size_t n = (size_t)1 << logn, m = 2 * n;
	static int8_t xa[2 * MAXN], xb[2 * MAXN], xs[2 * MAXN];
	static uint8_t t[MAXN / 4], key[48];
	uint64_t bits = UINT64_C(0x4A4B1A7000000000) + logn;
	uint8_t hm[64];
	shake_context prefix;

	random_t(t, n >> 2, &bits);
	lcg_rng(&bits, hm, sizeof hm);
	lcg_rng(&bits, key, sizeof key);
	(void)e8_sampler_prefix_init(&prefix, hm, key, sizeof key, 9);
	for (size_t c = 0; c < sizeof thread_cases / sizeof thread_cases[0];
		c ++)
	{
		unsigned T = thread_cases[c];
		uint64_t sa = UINT64_C(0x4A4B1A75EED00000) + logn, sb = sa;
		uint64_t ss = sa;
		uint8_t seed[40];
		uint32_t na, nb, ns = 0;

		lcg_rng(&ss, seed, sizeof seed);
		for (unsigned w = 0; w < T; w ++) {
			size_t start = (m * w) / T, end = (m * (w + 1)) / T;
			uint8_t label[18];
			cmn_rng s;

			label[0] = (uint8_t)logn;
			label[1] = (uint8_t)w;
			enc64le(label + 2, start);
			enc64le(label + 10, end);
			cmn_rng_init_prefixed(&s, &prefix, domain,
				sizeof domain - 1, seed, 40, label, sizeof label);
			if (!v->range(v->tables(logn), logn, xs, t,
				start, end, &s, &ns))
			{
				fprintf(stderr, "ERR: HAWK variant %s range"
					" failed\n", v->name);
				return 0;
			}
		}
		hawkv_select(v, T);
		na = hawk_variant_sampler(logn, lcg_rng, &sa, &prefix, xa, t);
		nb = hawk_variant_sampler(logn, lcg_rng, &sb, &prefix, xb, t);
		if (na != nb || memcmp(xa, xb, m) != 0) {
			fprintf(stderr, "ERR: HAWK variant %s logn=%u T=%u not"
				" deterministic\n", v->name, logn, T);
			return 0;
		}
		if (na != ns || memcmp(xa, xs, m) != 0) {
			fprintf(stderr, "ERR: HAWK variant %s logn=%u T=%u"
				" differs from the documented streams\n",
				v->name, logn, T);
			return 0;
		}
	}
	hawkv_select(&hawk_sampler_variants[0], 1);
	cmn_pool_shrink(0);
	return 1;
}

static int
moments_ok(const char *what, unsigned logn, unsigned p,
	long double mean, long double var, const int8_t *x, size_t count)
{
	long double s1 = 0.0L, s2 = 0.0L, emean, evar;

	for (size_t u = 0; u < count; u ++) {
		s1 += x[u];
		s2 += (long double)x[u] * x[u];
	}
	emean = s1 / count;
	evar = s2 / count - emean * emean;
	if (fabsl(emean - mean) > 6.0L * sqrtl(var / count)
		|| fabsl(evar - var) > 6.0L * sqrtl(3.0L * var * var / count))
	{
		fprintf(stderr, "ERR: %s distribution logn=%u parity=%u"
			" mean %.6Lf (exact %.6Lf) var %.6Lf (exact %.6Lf)\n",
			what, logn, p, emean, mean, evar, var);
		return 0;
	}
	return 1;
}

static void
hot_moments(unsigned bits, unsigned tiny_len, const int8_t *tv,
	const uint64_t *tq, unsigned tail_len, const int8_t *lv,
	const uint64_t *lq, long double *mean, long double *var)
{
	long double scale = ldexpl(1.0L, (int)bits);
	long double m1 = 0.0L, m2 = 0.0L, prev = -1.0L, p_tail;

	for (unsigned i = 0; i < tiny_len; i ++) {
		long double p = ((long double)tq[i] - prev) / scale;

		m1 += p * tv[i];
		m2 += p * tv[i] * tv[i];
		prev = (long double)tq[i];
	}
	p_tail = (scale - 1.0L - prev) / scale;
	prev = -1.0L;
	for (unsigned i = 0; i < tail_len; i ++) {
		long double p = p_tail * ((long double)lq[i] - prev) / scale;

		m1 += p * lv[i];
		m2 += p * lv[i] * lv[i];
		prev = (long double)lq[i];
	}
	*mean = m1;
	*var = m2 - m1 * m1;
}

/*
 * Exact moments of the ref mapping for parity p: P(r >= k + 1) =
 * T[2k + p] / 2^78, |x| = 2r + p, symmetric sign.
 */
static void
ref_moments(unsigned logn, unsigned p, long double *mean, long double *var)
{
	const hawkv_ref_tables *g = hawkv_ref_tables_get(logn);
	long double scale = ldexpl(1.0L, 78), prev = scale, m2 = 0.0L;
	size_t pairs = g->lo_len / 2;

	for (size_t k = 0; k <= pairs; k ++) {
		long double tk = 0.0L, a = 2.0L * k + p;

		if (k < pairs) {
			size_t i = 2 * k + p;

			tk = (long double)g->lo[i] + (i < g->hi_len
				? ldexpl((long double)g->hi[i], 63) : 0.0L);
		}
		m2 += (prev - tk) / scale * a * a;
		prev = tk;
	}
	*mean = 0.0L;
	*var = m2;
}

/*
 * Exact moments of variant v's 1D table for parity p.
 */
static void
variant_moments(const hawk_sampler_variant *v, unsigned logn, unsigned p,
	long double *mean, long double *var)
{
	if (v->threshold_bits == 78) {
		ref_moments(logn, p, mean, var);
	} else if (v->threshold_bits == 64) {
		const hawk_hot64_row *r = &((const hawk_hot64_tables *)
			v->tables(logn))->rows[p];

		hot_moments(64, r->tiny_len, r->tiny_value, r->tiny_cdf,
			r->tail_len, r->tail_value, r->tail_cdf, mean, var);
	} else {
		const hawk_hot32_row *r = &((const hawk_hot32_tables *)
			v->tables(logn))->rows[p];
		uint64_t tq[HAWK_HOT32_TINY_LEN];
		uint64_t lq[HAWK_HOT32_TAIL_LEN_MAX];

		for (unsigned i = 0; i < r->tiny_len; i ++) {
			tq[i] = r->tiny_cdf[i];
		}
		for (unsigned i = 0; i < r->tail_len; i ++) {
			lq[i] = r->tail_cdf[i];
		}
		hot_moments(32, r->tiny_len, r->tiny_value, tq,
			r->tail_len, r->tail_value, lq, mean, var);
	}
}

static int
distribution(unsigned logn)
{
	static int8_t x[DIST_SAMPLES];
	static uint8_t t[DIST_SAMPLES / 8];

	for (size_t vi = 1; vi < hawk_sampler_variant_count; vi ++) {
		const hawk_sampler_variant *v = &hawk_sampler_variants[vi];

		for (unsigned p = 0; p < 2; p ++) {
			long double mean, var;
			uint8_t seed[3] = { (uint8_t)logn, (uint8_t)vi,
				(uint8_t)p };
			uint32_t sn = 0;
			cmn_rng s;

			memset(t, p ? 0xFF : 0x00, sizeof t);
			variant_moments(v, logn, p, &mean, &var);
			cmn_rng_init(&s, "test_hawk_variants", 18, seed, 3,
				NULL, 0);
			if (!v->range(v->tables(logn), logn, x, t, 0,
				DIST_SAMPLES, &s, &sn))
			{
				fprintf(stderr, "ERR: HAWK variant %s range"
					" failed\n", v->name);
				return 0;
			}
			if (!moments_ok(v->name, logn, p, mean, var, x,
				DIST_SAMPLES))
			{
				return 0;
			}
		}
	}
	return 1;
}

/*
 * --moments: print the exact moments of every variant's 1D tables (the
 * values the distribution check uses) as CSV lines, for the precision
 * audit (tools/precision_audit.py --c-moments), and exit.
 */
static void
print_moments(void)
{
	for (unsigned logn = 8; logn <= 10; logn ++) {
		for (size_t vi = 1; vi < hawk_sampler_variant_count; vi ++) {
			const hawk_sampler_variant *v =
				&hawk_sampler_variants[vi];

			for (unsigned p = 0; p < 2; p ++) {
				long double mean, var;

				variant_moments(v, logn, p, &mean, &var);
				printf("moments,hawk,%s,%u,%u,%.21Le,%.21Le\n",
					v->name, logn, p, mean, var);
			}
		}
	}
}

int
main(int argc, char **argv)
{
	if (argc > 1) {
		if (argc == 2 && strcmp(argv[1], "--moments") == 0) {
			print_moments();
			return 0;
		}
		fprintf(stderr, "usage: %s [--moments]\n", argv[0]);
		return 2;
	}
	for (unsigned logn = 8; logn <= 10; logn ++) {
		printf("HAWK variants n=%u:\n", 1u << logn);
		if (!tables_equal_upstream(logn) || !signer_is_upstream(logn)) {
			return 1;
		}
		printf("  generated 78-bit table = upstream; variant signer"
			" with upstream sig_gauss = upstream signing (encoded"
			" and decoded keys)\n");
		if (!prefix_matches_e8(logn)) {
			return 1;
		}
		for (size_t u = 0; u < hawk_sampler_variant_count; u ++) {
			const hawk_sampler_variant *v =
				&hawk_sampler_variants[u];

			if (!sign_verify(v, logn)) {
				return 1;
			}
			if (v->range != NULL && (!stream_layout(v, logn)
				|| !binding(v, logn)))
			{
				return 1;
			}
		}
		if (!distribution(logn)) {
			return 1;
		}
		printf("  split variants: T = 1, 2, 4, 8, 16 = documented"
			" streams, deterministic, bound; distributions:"
			" done.\n");
	}
	return 0;
}
