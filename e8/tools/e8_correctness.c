#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hawk_e8_inner.h"
#include "cmn_pool.h"
#include "cmn_rng.h"
#include "cmn_timing.h"
#include "hawk_shim.h"
#include "hawk_variants.h"

/*
 * Signature correctness of one sampler variant (the validation,
 * e8/docs/validation.md): signs with real keys and
 * verifies every signature with the scheme's verifier.
 *
 *   e8_correctness --scheme hawk|e8 --variant NAME --logn 8|9|10
 *                  [--keys K] [--sigs-per-key S] [--seed X] [--shard I]
 *                  [--threads T] [--header]
 *
 * The shard signs with keys I*K .. I*K + K - 1 (disjoint key indices for
 * disjoint shards), S signatures per key. Signature j of a key uses the
 * encoded private key if j is even (HAWK hawk_sign_finish() or the
 * variant signer's hawkv_sign_finish(), E8 e8_sign_finish_inner() with an
 * encoded key) and the decoded / prepared key if j is odd
 * (hawk_shim_sign_finish_decoded(), hawkv_sign_finish_decoded(),
 * e8_sign_finish_inner() with a prepared key). Every signature is checked
 * with hawk_verify_finish() / e8_verify_finish(), with the encoded and
 * with the decoded public key (hawk_decode_public_key() /
 * e8_decode_public_key()), and for E8 also with the reference verifier
 * e8_verify_finish_ref(); the verdicts must agree (verifier_mismatches
 * counts those that do not). The first signature of
 * each key is also checked against a different message, which must be
 * rejected by every verifier (a control that the verifier can fail).
 *
 * Determinism: every input is derived from the seed with the shared
 * signing RNG (cmn_sign_rng, seeded with a label and little-endian
 * fields):
 *   key k:        "key" || u64 seed || u32 scheme || u32 logn || u32 k
 *   message:      "e8_correctness message" || u64 seed || logn || k || j
 *   signing RNG:  "sign" || u64 seed || u32 scheme || u32 logn || u32 k
 *                 || u32 j || u32 threads || variant name
 * Keys therefore depend only on (seed, scheme, logn, key index), so every
 * variant of a scheme signs with the same keys. sig_digest is the first 8
 * bytes of SHAKE256 over every signature in order, so equal seeds give
 * equal digests.
 *
 * Threads: T > 1 runs the variant's split sampler on the shared worker
 * pool (HAWK ref-T, hot64, hot32 via hawkv_select(); every E8 variant via
 * e8_sampler_set_thread_count()); the pool is sized before signing.
 * Upstream HAWK ref is serial and rejects T > 1.
 *
 * Attempts are counted from the signing RNG (two calls per attempt for
 * every variant of both schemes, as in e8_sign_bench); for E8 they are
 * also compared with the signer's own count.
 *
 * Output (stdout): one CSV row (with --header, the header first). Exit
 * status 0 if every signature was produced and verified and every control
 * was rejected, 1 otherwise, 2 on a usage error.
 */

#define SCHEME_HAWK   0
#define SCHEME_E8     1

static const char *const SCHEME_NAME[] = { "hawk", "e8" };

static uint8_t key_priv[E8_PRIVKEY_SIZE(10)];
static uint8_t key_pub[E8_PUBKEY_SIZE(10)];
static uint8_t key_prepared[E8_PRIVKEY_PREPARED_SIZE(10)];
static int32_t key_decoded[(E8_PUBKEY_DECODED_SIZE(10) + 3) / 4];
static uint8_t tmp_keygen[E8_TMPSIZE_KEYGEN(10)];
static uint8_t tmp_sign[E8_TMPSIZE_SIGN(10)];
static uint8_t tmp_verify[E8_TMPSIZE_VERIFY(10) + HAWK_TMPSIZE_VERIFY(10)];
static uint8_t sig_buf[E8_SIG_SIZE(10)];

typedef char correctness_hawk_buffers_check[
	(sizeof key_priv >= HAWK_PRIVKEY_SIZE(10)
	&& sizeof key_pub >= HAWK_PUBKEY_SIZE(10)
	&& sizeof key_prepared >= HAWK_PRIVKEY_DECODED_SIZE(10)
	&& sizeof key_decoded >= 2 * HAWK_DECODED_PUB_LENGTH(10)
	&& sizeof tmp_keygen >= HAWK_TMPSIZE_KEYGEN(10)
	&& sizeof tmp_sign >= HAWK_TMPSIZE_SIGN(10)
	&& sizeof tmp_verify >= HAWK_TMPSIZE_VERIFY(10)
	&& sizeof sig_buf >= HAWK_SIG_SIZE(10)) ? 1 : -1];

typedef struct {
	unsigned scheme;
	const char *variant;
	unsigned logn;
	unsigned keys;
	unsigned sigs_per_key;
	uint64_t seed;
	unsigned shard;
	unsigned threads;
	int header;
} correctness_options;

typedef struct {
	uint64_t signatures;
	uint64_t sign_failures;
	uint64_t verify_failures;
	uint64_t encoded;
	uint64_t prepared;
	uint64_t attempts;
	uint64_t attempts_max;
	uint64_t attempt_mismatches;
	uint64_t controls;
	uint64_t control_accepts;
	uint64_t verifier_mismatches;
	uint64_t keygen_failures;
} correctness_counts;

static void
put_u32le(uint8_t *dst, uint32_t x)
{
	for (unsigned u = 0; u < 4; u ++) {
		dst[u] = (uint8_t)(x >> (u << 3));
	}
}

static void
put_u64le(uint8_t *dst, uint64_t x)
{
	for (unsigned u = 0; u < 8; u ++) {
		dst[u] = (uint8_t)(x >> (u << 3));
	}
}

/*
 * cmn_sign_rng seeded with label || u64le(seed) || u32le(fields[i]) ...
 * || tail.
 */
static void
seeded_rng(cmn_sign_rng *rng, const char *label, uint64_t seed,
	const uint32_t *fields, size_t nfields, const char *tail)
{
	uint8_t buf[128];
	size_t len = strlen(label), tail_len = tail != NULL ? strlen(tail) : 0;

	if (len + 8 + 4 * nfields + tail_len > sizeof buf) {
		abort();
	}
	memcpy(buf, label, len);
	put_u64le(buf + len, seed);
	len += 8;
	for (size_t u = 0; u < nfields; u ++) {
		put_u32le(buf + len, fields[u]);
		len += 4;
	}
	if (tail_len > 0) {
		memcpy(buf + len, tail, tail_len);
		len += tail_len;
	}
	cmn_sign_rng_init(rng, buf, len);
}

static void
make_message(shake_context *sc_data, uint64_t seed, unsigned logn,
	uint32_t key, uint32_t sig)
{
	static const char prefix[] = "e8_correctness message";
	uint8_t buf[20];

	put_u64le(buf, seed);
	put_u32le(buf + 8, logn);
	put_u32le(buf + 12, key);
	put_u32le(buf + 16, sig);
	hawk_sign_start(sc_data);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

static int
keygen(const correctness_options *o, uint32_t key)
{
	uint32_t fields[3] = { o->scheme, o->logn, key };
	cmn_sign_rng rng;

	seeded_rng(&rng, "key", o->seed, fields, 3, NULL);
	if (o->scheme == SCHEME_HAWK) {
		if (!hawk_keygen(o->logn, key_priv, key_pub,
			cmn_sign_rng_hawk, &rng, tmp_keygen, sizeof tmp_keygen))
		{
			return 0;
		}
		hawk_decode_private_key(o->logn, key_prepared, key_priv);
		return hawk_decode_public_key(o->logn, (int16_t *)key_decoded,
			key_pub, HAWK_PUBKEY_SIZE(o->logn));
	}
	return e8_keygen(o->logn, key_priv, key_pub, cmn_sign_rng_hawk, &rng,
		tmp_keygen, sizeof tmp_keygen)
		&& e8_prepare_private_key(o->logn, key_prepared, key_priv)
		&& e8_decode_public_key(o->logn, key_decoded, key_pub,
			E8_PUBKEY_SIZE(o->logn));
}

static int
sign_one(const correctness_options *o, const hawk_sampler_variant *hv,
	const e8_sampler_variant *ev, int prepared,
	const shake_context *sc_data, cmn_sign_rng *rng,
	unsigned *signer_attempts)
{
	unsigned logn = o->logn;

	*signer_attempts = 0;
	if (o->scheme == SCHEME_E8) {
		if (!prepared) {
			return e8_sign_finish_inner(logn, cmn_sign_rng_hawk, rng,
				sig_buf, sc_data, key_priv,
				E8_PRIVKEY_SIZE(logn), tmp_sign,
				sizeof tmp_sign, ev, signer_attempts, NULL);
		}
		return e8_sign_finish_inner(logn, cmn_sign_rng_hawk, rng,
			sig_buf, sc_data, key_prepared,
			E8_PRIVKEY_PREPARED_SIZE(logn), NULL, 0, ev,
			signer_attempts, NULL);
	}
	if (hv->range == NULL) {
		if (!prepared) {
			return hawk_sign_finish(logn, cmn_sign_rng_hawk, rng,
				sig_buf, sc_data, key_priv,
				tmp_sign, sizeof tmp_sign);
		}
		return hawk_shim_sign_finish_decoded(logn, cmn_sign_rng_hawk,
			rng, sig_buf, sc_data, key_prepared,
			tmp_sign, sizeof tmp_sign);
	}
	if (!prepared) {
		return hawkv_sign_finish(logn, cmn_sign_rng_hawk, rng,
			sig_buf, sc_data, key_priv, tmp_sign, sizeof tmp_sign);
	}
	return hawkv_sign_finish_decoded(logn, cmn_sign_rng_hawk, rng,
		sig_buf, sc_data, key_prepared, tmp_sign, sizeof tmp_sign);
}

/*
 * Verify sig_buf with the encoded and the decoded public key (and for E8
 * the reference verifier); returns 1 only if every verifier accepts and
 * counts a verifier mismatch if they disagree.
 */
static int
verify_one(const correctness_options *o, const shake_context *sc_data,
	correctness_counts *c)
{
	unsigned logn = o->logn;
	int enc, dec, ref;

	if (o->scheme == SCHEME_HAWK) {
		enc = hawk_verify_finish(logn, sig_buf, HAWK_SIG_SIZE(logn),
			sc_data, key_pub, HAWK_PUBKEY_SIZE(logn),
			tmp_verify, sizeof tmp_verify);
		dec = hawk_verify_finish(logn, sig_buf, HAWK_SIG_SIZE(logn),
			sc_data, key_decoded, (size_t)-1,
			tmp_verify, sizeof tmp_verify);
		ref = enc;
	} else {
		enc = e8_verify_finish(logn, sig_buf, E8_SIG_SIZE(logn),
			sc_data, key_pub, E8_PUBKEY_SIZE(logn),
			tmp_verify, sizeof tmp_verify);
		dec = e8_verify_finish(logn, sig_buf, E8_SIG_SIZE(logn),
			sc_data, key_decoded, (size_t)-1, NULL, 0);
		ref = e8_verify_finish_ref(logn, sig_buf, E8_SIG_SIZE(logn),
			sc_data, key_pub, E8_PUBKEY_SIZE(logn),
			tmp_verify, sizeof tmp_verify);
	}
	if (enc != dec || enc != ref) {
		c->verifier_mismatches ++;
	}
	return enc && dec && ref;
}

static int
run(const correctness_options *o, const hawk_sampler_variant *hv,
	const e8_sampler_variant *ev, correctness_counts *c,
	uint8_t *digest)
{
	shake_context dsc;
	size_t sig_len = o->scheme == SCHEME_HAWK
		? HAWK_SIG_SIZE(o->logn) : E8_SIG_SIZE(o->logn);

	shake_init(&dsc, 256);
	for (unsigned k = 0; k < o->keys; k ++) {
		uint32_t key = o->shard * o->keys + k;

		if (!keygen(o, key)) {
			c->keygen_failures ++;
			continue;
		}
		for (unsigned j = 0; j < o->sigs_per_key; j ++) {
			uint32_t fields[5] = { o->scheme, o->logn, key, j,
				o->threads };
			shake_context sc_data;
			cmn_sign_rng rng;
			unsigned signer_attempts;
			uint64_t attempts;
			int prepared = (int)(j & 1);

			make_message(&sc_data, o->seed, o->logn, key, j);
			seeded_rng(&rng, "sign", o->seed, fields, 5,
				o->variant);
			c->signatures ++;
			if (prepared) {
				c->prepared ++;
			} else {
				c->encoded ++;
			}
			if (!sign_one(o, hv, ev, prepared, &sc_data, &rng,
				&signer_attempts))
			{
				c->sign_failures ++;
				continue;
			}
			attempts = rng.calls / 2;
			c->attempts += attempts;
			if (attempts > c->attempts_max) {
				c->attempts_max = attempts;
			}
			if (o->scheme == SCHEME_E8
				&& (signer_attempts != attempts
				|| rng.calls != 2 * attempts))
			{
				c->attempt_mismatches ++;
			}
			shake_inject(&dsc, sig_buf, sig_len);
			if (!verify_one(o, &sc_data, c)) {
				c->verify_failures ++;
			}
			if (j == 0) {
				make_message(&sc_data, o->seed, o->logn,
					key, o->sigs_per_key);
				c->controls ++;
				if (verify_one(o, &sc_data, c)) {
					c->control_accepts ++;
				}
			}
		}
	}
	shake_flip(&dsc);
	shake_extract(&dsc, digest, 8);
	return 1;
}

static int
parse_u64(const char *s, uint64_t max, uint64_t *out)
{
	char *end;
	unsigned long long v;

	if (s == NULL || *s == 0 || *s == '-') {
		return 0;
	}
	v = strtoull(s, &end, 0);
	if (*end != 0 || v > max) {
		return 0;
	}
	*out = v;
	return 1;
}

static int
parse_args(int argc, char **argv, correctness_options *o)
{
	memset(o, 0, sizeof *o);
	o->scheme = 2;
	o->keys = 4;
	o->sigs_per_key = 16;
	o->seed = 1;
	o->threads = 1;
	for (int i = 1; i < argc; i ++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		uint64_t x;

		if (strcmp(a, "--header") == 0) {
			o->header = 1;
			continue;
		}
		if (v == NULL) {
			return 0;
		}
		i ++;
		if (strcmp(a, "--scheme") == 0) {
			if (strcmp(v, "hawk") == 0) {
				o->scheme = SCHEME_HAWK;
			} else if (strcmp(v, "e8") == 0) {
				o->scheme = SCHEME_E8;
			} else {
				return 0;
			}
		} else if (strcmp(a, "--variant") == 0) {
			o->variant = v;
		} else if (strcmp(a, "--logn") == 0) {
			if (!parse_u64(v, 10, &x) || x < 8) {
				return 0;
			}
			o->logn = (unsigned)x;
		} else if (strcmp(a, "--keys") == 0) {
			if (!parse_u64(v, 1u << 20, &x) || x == 0) {
				return 0;
			}
			o->keys = (unsigned)x;
		} else if (strcmp(a, "--sigs-per-key") == 0) {
			if (!parse_u64(v, 1u << 24, &x) || x == 0) {
				return 0;
			}
			o->sigs_per_key = (unsigned)x;
		} else if (strcmp(a, "--seed") == 0) {
			if (!parse_u64(v, UINT64_MAX, &o->seed)) {
				return 0;
			}
		} else if (strcmp(a, "--shard") == 0) {
			if (!parse_u64(v, 1u << 20, &x)) {
				return 0;
			}
			o->shard = (unsigned)x;
		} else if (strcmp(a, "--threads") == 0) {
			if (!parse_u64(v, 24, &x) || x == 0) {
				return 0;
			}
			o->threads = (unsigned)x;
		} else {
			return 0;
		}
	}
	return o->scheme < 2 && o->variant != NULL && o->logn != 0
		&& (uint64_t)o->shard * o->keys + o->keys <= UINT32_MAX;
}

int
main(int argc, char **argv)
{
	correctness_options o;
	const hawk_sampler_variant *hv = NULL;
	const e8_sampler_variant *ev = NULL;
	correctness_counts c;
	uint8_t digest[8];
	uint64_t t0, t1;
	int ok;

	if (!parse_args(argc, argv, &o)) {
		fprintf(stderr, "usage: %s --scheme hawk|e8 --variant NAME"
			" --logn 8|9|10 [--keys K] [--sigs-per-key S]"
			" [--seed X] [--shard I] [--threads T] [--header]\n",
			argv[0]);
		return 2;
	}
	if (o.scheme == SCHEME_HAWK) {
		hv = hawk_sampler_variant_find(o.variant);
		if (hv == NULL || (hv->range == NULL && o.threads != 1)) {
			fprintf(stderr, "ERR: unknown HAWK variant %s or"
				" threads %u not supported\n", o.variant,
				o.threads);
			return 2;
		}
		if (!hawkv_select(hv, o.threads)) {
			fprintf(stderr, "ERR: cannot select %s\n", o.variant);
			return 1;
		}
	} else {
		ev = e8_sampler_variant_find(o.variant);
		if (ev == NULL) {
			fprintf(stderr, "ERR: unknown E8 variant %s\n",
				o.variant);
			return 2;
		}
		e8_sampler_set_thread_count(o.threads);
		if (e8_sampler_get_thread_count(o.logn) != o.threads) {
			fprintf(stderr, "ERR: E8 thread count %u not"
				" available\n", o.threads);
			return 1;
		}
	}
	if (o.threads > 1 && !cmn_pool_ensure(o.threads - 1)) {
		fprintf(stderr, "ERR: cannot start %u pool threads\n",
			o.threads - 1);
		return 1;
	}

	memset(&c, 0, sizeof c);
	t0 = cmn_wall_ns();
	ok = run(&o, hv, ev, &c, digest);
	t1 = cmn_wall_ns();
	cmn_pool_shrink(0);

	if (o.header) {
		printf("scheme,variant,threshold_bits,technique,logn,n,threads,"
			"debug_checks,seed,shard,keys,sigs_per_key,signatures,"
			"encoded,prepared,sign_failures,verify_failures,"
			"keygen_failures,attempts,attempts_max,"
			"attempt_mismatches,controls,control_accepts,"
			"verifier_mismatches,sig_digest,seconds\n");
	}
	printf("%s,%s,%u,%s,%u,%u,%u,%d,%llu,%u,%u,%u,%llu,%llu,%llu,%llu,"
		"%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,",
		SCHEME_NAME[o.scheme], o.variant,
		hv != NULL ? hv->threshold_bits : ev->threshold_bits,
		hv != NULL ? hv->technique : ev->technique,
		o.logn, 1u << o.logn, o.threads, HAWK_E8_DEBUG_CHECKS,
		(unsigned long long)o.seed, o.shard, o.keys, o.sigs_per_key,
		(unsigned long long)c.signatures,
		(unsigned long long)c.encoded,
		(unsigned long long)c.prepared,
		(unsigned long long)c.sign_failures,
		(unsigned long long)c.verify_failures,
		(unsigned long long)c.keygen_failures,
		(unsigned long long)c.attempts,
		(unsigned long long)c.attempts_max,
		(unsigned long long)c.attempt_mismatches,
		(unsigned long long)c.controls,
		(unsigned long long)c.control_accepts,
		(unsigned long long)c.verifier_mismatches);
	for (unsigned u = 0; u < sizeof digest; u ++) {
		printf("%02x", digest[u]);
	}
	printf(",%.3f\n", (double)(t1 - t0) * 1e-9);
	return ok && c.sign_failures == 0 && c.verify_failures == 0
		&& c.keygen_failures == 0 && c.attempt_mismatches == 0
		&& c.control_accepts == 0 && c.verifier_mismatches == 0 ? 0 : 1;
}
