#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hawk_e8_inner.h"
#include "../compare/hawk_shim.h"

/*
 * Tests for the public HAWK-E8-CM API (hawk_e8.h) and for the HAWK
 * prepared-key shim used by the comparison benchmark:
 *
 *   - keygen, sign (encoded and prepared key), verify round trips;
 *   - tampered message, signature, salt and public key are rejected;
 *   - the per-attempt salt is exactly HAWK's construction;
 *   - both schemes call the RNG exactly twice per attempt (salt, then
 *     sampler seed), also with the E8 sampler thread pool enabled, so
 *     attempts = rng_calls / 2 for both.
 */

#define MAXN   1024

typedef struct {
	unsigned logn;
	unsigned keys;
	unsigned messages;
} api_param;

static const api_param PARAMS[] = {
	{ 8,  3, 3 },
	{ 9,  2, 3 },
	{ 10, 2, 3 }
};

/*
 * Deterministic test RNG that records its calls. Calls are expected to
 * alternate between salt_len bytes (salt) and seed_len bytes (sampler
 * seed); the last salt request is kept for the salt check.
 */
typedef struct {
	uint64_t state;
	unsigned calls;
	size_t salt_len;
	size_t seed_len;
	int pattern_ok;
	uint8_t last_salt_input[40];
} count_rng;

static void
lcg_bytes(uint64_t *state, uint8_t *buf, size_t len)
{
	while (len > 0) {
		uint64_t x;

		*state = *state * UINT64_C(6364136223846793005)
			+ UINT64_C(1442695040888963407);
		x = *state;
		for (unsigned u = 0; u < 8 && len > 0; u ++, len --) {
			*buf ++ = (uint8_t)(x >> (u << 3));
		}
	}
}

static void
plain_rng(void *ctx, void *dst, size_t len)
{
	lcg_bytes(ctx, dst, len);
}

static void
counting_rng(void *ctx, void *dst, size_t len)
{
	count_rng *rng = ctx;
	size_t expected = (rng->calls & 1u) == 0
		? rng->salt_len : rng->seed_len;

	if (len != expected) {
		rng->pattern_ok = 0;
	}
	lcg_bytes(&rng->state, dst, len);
	if ((rng->calls & 1u) == 0 && len <= sizeof rng->last_salt_input) {
		memcpy(rng->last_salt_input, dst, len);
	}
	rng->calls ++;
}

static void
count_rng_init(count_rng *rng, uint64_t seed,
	size_t salt_len, size_t seed_len)
{
	memset(rng, 0, sizeof *rng);
	rng->state = seed;
	rng->salt_len = salt_len;
	rng->seed_len = seed_len;
	rng->pattern_ok = 1;
}

static void
make_message(shake_context *sc, unsigned logn,
	unsigned key, unsigned msg, unsigned variant)
{
	static const char prefix[] = "hawk e8 api test message";
	uint8_t buf[4];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)key;
	buf[2] = (uint8_t)msg;
	buf[3] = (uint8_t)variant;
	e8_sign_start(sc);
	shake_inject(sc, prefix, sizeof prefix - 1);
	shake_inject(sc, buf, sizeof buf);
}

/*
 * Recompute HAWK's salt for the final attempt:
 *   SHAKE256(SHAKE256(message || hpub) || key || enc32le(2*(attempts-1))
 *            || rng_salt)
 * and compare with the signature's salt.
 */
static int
check_salt(unsigned logn, const uint8_t *sig, const shake_context *sc_data,
	const uint8_t *hpub, const void *key, size_t key_len,
	unsigned attempts, const uint8_t *rng_salt)
{
	size_t salt_len = e8_salt_len(logn);
	uint8_t hm[64], salt[40], tbuf[4];
	shake_context sc;

	sc = *sc_data;
	shake_inject(&sc, hpub, (size_t)1 << (logn - 4));
	shake_flip(&sc);
	shake_extract(&sc, hm, sizeof hm);
	enc32le(tbuf, (uint32_t)(attempts - 1) << 1);
	shake_init(&sc, 256);
	shake_inject(&sc, hm, sizeof hm);
	shake_inject(&sc, key, key_len);
	shake_inject(&sc, tbuf, sizeof tbuf);
	shake_inject(&sc, rng_salt, salt_len);
	shake_flip(&sc);
	shake_extract(&sc, salt, salt_len);
	return memcmp(salt, sig, salt_len) == 0;
}

static int
check_rejections(unsigned logn, const uint8_t *sig,
	const uint8_t *pub, unsigned key, unsigned msg)
{
	size_t sig_len = E8_SIG_SIZE(logn);
	size_t pub_len = E8_PUBKEY_SIZE(logn);
	size_t salt_len = e8_salt_len(logn);
	static uint8_t bad_sig[E8_SIG_SIZE(10)];
	static uint8_t bad_pub[E8_PUBKEY_SIZE(10)];
	static uint8_t tmp[E8_TMPSIZE_VERIFY(10)];
	shake_context sc;

	make_message(&sc, logn, key, msg, 1);
	if (e8_verify_finish(logn, sig, sig_len, &sc, pub, pub_len,
		tmp, sizeof tmp))
	{
		fprintf(stderr, "ERR: tampered message accepted logn=%u\n",
			logn);
		return 0;
	}

	make_message(&sc, logn, key, msg, 0);
	memcpy(bad_sig, sig, sig_len);
	bad_sig[salt_len] ^= 0x02;
	if (e8_verify_finish(logn, bad_sig, sig_len, &sc, pub, pub_len,
		tmp, sizeof tmp))
	{
		fprintf(stderr, "ERR: tampered signature accepted logn=%u\n",
			logn);
		return 0;
	}

	memcpy(bad_sig, sig, sig_len);
	bad_sig[0] ^= 0x01;
	if (e8_verify_finish(logn, bad_sig, sig_len, &sc, pub, pub_len,
		tmp, sizeof tmp))
	{
		fprintf(stderr, "ERR: tampered salt accepted logn=%u\n", logn);
		return 0;
	}

	memcpy(bad_pub, pub, pub_len);
	bad_pub[4] ^= 0x01;
	if (e8_verify_finish(logn, sig, sig_len, &sc, bad_pub, pub_len,
		tmp, sizeof tmp))
	{
		fprintf(stderr, "ERR: tampered public key accepted logn=%u\n",
			logn);
		return 0;
	}

	if (e8_verify_finish(logn, sig, sig_len - 1, &sc, pub, pub_len,
		tmp, sizeof tmp)
		|| e8_verify_finish(logn, sig, sig_len, &sc, pub, pub_len - 1,
		tmp, sizeof tmp)
		|| e8_verify_finish(logn, sig, sig_len, &sc, pub, pub_len,
		tmp, E8_TMPSIZE_VERIFY(logn) - 8))
	{
		fprintf(stderr, "ERR: bad length accepted logn=%u\n", logn);
		return 0;
	}
	return 1;
}

/*
 * Sign one message with the given key format through
 * e8_sign_finish_inner(), then check the attempt counter against the RNG
 * call count, the salt construction, and verification.
 */
static int
sign_and_check(unsigned logn, const void *priv, size_t priv_len,
	const void *salt_key, size_t salt_key_len,
	const uint8_t *hpub, const uint8_t *pub,
	uint64_t rng_seed, unsigned key, unsigned msg,
	uint8_t *sig, unsigned *attempts_out)
{
	static uint8_t tmp[E8_TMPSIZE_SIGN(10)];
	static uint8_t vtmp[E8_TMPSIZE_VERIFY(10)];
	shake_context sc;
	count_rng rng;
	unsigned attempts = 0;

	count_rng_init(&rng, rng_seed, e8_salt_len(logn), 32);
	make_message(&sc, logn, key, msg, 0);
	if (!e8_sign_finish_inner(logn, counting_rng, &rng, sig, &sc,
		priv, priv_len, tmp, sizeof tmp, &attempts, NULL))
	{
		fprintf(stderr, "ERR: E8 signing failed logn=%u key=%u"
			" msg=%u priv_len=%zu\n", logn, key, msg, priv_len);
		return 0;
	}
	if (attempts < 1 || rng.calls != 2 * attempts || !rng.pattern_ok) {
		fprintf(stderr, "ERR: E8 RNG calls logn=%u attempts=%u"
			" calls=%u pattern_ok=%d\n",
			logn, attempts, rng.calls, rng.pattern_ok);
		return 0;
	}
	if (!check_salt(logn, sig, &sc, hpub, salt_key, salt_key_len,
		attempts, rng.last_salt_input))
	{
		fprintf(stderr, "ERR: E8 salt is not HAWK's construction"
			" logn=%u key=%u msg=%u\n", logn, key, msg);
		return 0;
	}
	if (!e8_verify_finish(logn, sig, E8_SIG_SIZE(logn), &sc,
		pub, E8_PUBKEY_SIZE(logn), vtmp, sizeof vtmp))
	{
		fprintf(stderr, "ERR: E8 signature rejected logn=%u key=%u"
			" msg=%u priv_len=%zu\n", logn, key, msg, priv_len);
		return 0;
	}
	*attempts_out = attempts;
	return 1;
}

static int
test_e8_api_logn(const api_param *param)
{
	unsigned logn = param->logn;
	size_t n = (size_t)1 << logn;
	size_t seed_len = 8 + ((size_t)1 << (logn - 5));
	static uint8_t priv[E8_PRIVKEY_SIZE(10)];
	static uint8_t pub[E8_PUBKEY_SIZE(10)];
	static uint8_t prepared[E8_PRIVKEY_PREPARED_SIZE(10)];
	static uint8_t ktmp[E8_TMPSIZE_KEYGEN(10)];
	static uint8_t stmp[E8_TMPSIZE_SIGN(10)];
	static uint8_t vtmp[E8_TMPSIZE_VERIFY(10)];
	static uint8_t sig_enc[E8_SIG_SIZE(10)], sig_prep[E8_SIG_SIZE(10)];
	static uint8_t sig_pub[E8_SIG_SIZE(10)];
	int8_t f[MAXN], g[MAXN];
	uint64_t key_state = UINT64_C(0xE8A91C0DE0000000) + logn;
	unsigned signatures = 0, total_attempts = 0;

	if (E8_SIG_SIZE(logn) != e8_sig_uncompressed_size(logn)) {
		fprintf(stderr, "ERR: E8_SIG_SIZE mismatch logn=%u\n", logn);
		return 0;
	}

	printf("E8 API n=%u: privkey=%u prepared=%u pubkey=%u sig=%u\n",
		1u << logn, E8_PRIVKEY_SIZE(logn),
		E8_PRIVKEY_PREPARED_SIZE(logn), E8_PUBKEY_SIZE(logn),
		E8_SIG_SIZE(logn));
	for (unsigned key = 0; key < param->keys; key ++) {
		if (!e8_keygen(logn, priv, pub, plain_rng, &key_state,
			ktmp, sizeof ktmp))
		{
			fprintf(stderr, "ERR: e8_keygen failed logn=%u\n", logn);
			return 0;
		}
		if (e8_keygen(logn, priv, pub, plain_rng, &key_state,
			ktmp, E8_TMPSIZE_KEYGEN(logn) - 8))
		{
			fprintf(stderr, "ERR: e8_keygen short tmp logn=%u\n",
				logn);
			return 0;
		}
		if (!e8_prepare_private_key(logn, prepared, priv)) {
			fprintf(stderr, "ERR: e8_prepare_private_key failed"
				" logn=%u\n", logn);
			return 0;
		}
		Hawk_regen_fg(logn, f, g, priv);
		for (unsigned msg = 0; msg < param->messages; msg ++) {
			uint64_t rng_seed = UINT64_C(0x5EED5EED00000000)
				+ ((uint64_t)logn << 24) + (key << 8) + msg;
			unsigned threads = msg == 0 ? 1 : 4;
			unsigned rng_mode = msg == 2
				? E8_SAMPLER_RNG_PER_WORKER
				: E8_SAMPLER_RNG_PER_BLOCK;
			unsigned a_enc, a_prep;
			uint8_t fg[2 * MAXN];
			shake_context sc;
			uint64_t state;

			e8_sampler_set_thread_count(threads);
			e8_sampler_set_rng_mode(rng_mode);
			memcpy(fg, f, n);
			memcpy(fg + n, g, n);

			/* Same RNG stream for the encoded and prepared key. */
			if (!sign_and_check(logn, priv, E8_PRIVKEY_SIZE(logn),
				priv, seed_len, priv + seed_len + 2 * n, pub,
				rng_seed, key, msg, sig_enc, &a_enc)
				|| !sign_and_check(logn, prepared,
				E8_PRIVKEY_PREPARED_SIZE(logn), fg, 2 * n,
				priv + seed_len + 2 * n, pub,
				rng_seed, key, msg, sig_prep, &a_prep))
			{
				return 0;
			}
			if (memcmp(sig_enc, sig_prep, E8_SIG_SIZE(logn)) == 0) {
				fprintf(stderr, "ERR: encoded and prepared salts"
					" coincide logn=%u\n", logn);
				return 0;
			}
			if (!check_rejections(logn, sig_enc, pub, key, msg)
				|| !check_rejections(logn, sig_prep, pub, key, msg))
			{
				return 0;
			}

			/*
			 * The public wrappers produce the same signatures as
			 * the traced entry point for the same RNG stream.
			 */
			make_message(&sc, logn, key, msg, 0);
			state = rng_seed;
			if (!e8_sign_finish(logn, plain_rng, &state, sig_pub,
				&sc, priv, stmp, sizeof stmp)
				|| memcmp(sig_pub, sig_enc, E8_SIG_SIZE(logn)) != 0)
			{
				fprintf(stderr, "ERR: e8_sign_finish mismatch"
					" logn=%u\n", logn);
				return 0;
			}
			state = rng_seed;
			if (!e8_sign_finish_prepared(logn, plain_rng, &state,
				sig_pub, &sc, prepared, NULL, 0)
				|| memcmp(sig_pub, sig_prep, E8_SIG_SIZE(logn)) != 0)
			{
				fprintf(stderr, "ERR: e8_sign_finish_prepared"
					" mismatch logn=%u\n", logn);
				return 0;
			}
			if (e8_sign_finish(logn, plain_rng, &state, sig_pub,
				&sc, priv, stmp, E8_TMPSIZE_SIGN(logn) - 8))
			{
				fprintf(stderr, "ERR: e8_sign_finish short tmp"
					" logn=%u\n", logn);
				return 0;
			}
			if (!e8_verify_finish(logn, sig_pub, E8_SIG_SIZE(logn),
				&sc, pub, E8_PUBKEY_SIZE(logn), vtmp, sizeof vtmp))
			{
				fprintf(stderr, "ERR: public API signature"
					" rejected logn=%u\n", logn);
				return 0;
			}

			printf("  key=%u msg=%u threads=%u rng_mode=%s"
				" attempts_encoded=%u attempts_prepared=%u\n",
				key, msg, threads,
				rng_mode == E8_SAMPLER_RNG_PER_WORKER
				? "per_worker" : "per_block", a_enc, a_prep);
			signatures += 2;
			total_attempts += a_enc + a_prep;
		}
	}

	/* A prepared key is rejected for another degree or when corrupted. */
	{
		shake_context sc;
		uint64_t state = 1;
		unsigned other = logn == 8 ? 9 : 8;

		make_message(&sc, logn, 0, 0, 0);
		if (e8_sign_finish_prepared(other, plain_rng, &state,
			sig_pub, &sc, prepared, NULL, 0)
			|| e8_sign_finish(7, plain_rng, &state,
			sig_pub, &sc, priv, stmp, sizeof stmp))
		{
			fprintf(stderr, "ERR: bad degree accepted logn=%u\n",
				logn);
			return 0;
		}
		memset(prepared, 0, sizeof prepared);
		if (e8_sign_finish_prepared(logn, plain_rng, &state,
			sig_pub, &sc, prepared, NULL, 0))
		{
			fprintf(stderr, "ERR: unprepared key accepted logn=%u\n",
				logn);
			return 0;
		}
	}
	e8_sampler_set_thread_count(1);
	e8_sampler_set_rng_mode(E8_SAMPLER_RNG_PER_BLOCK);
	printf("E8 API n=%u summary: signatures=%u total_attempts=%u\n",
		1u << logn, signatures, total_attempts);
	return 1;
}

/*
 * HAWK's prepared-key path through the shim: signatures from a decoded
 * key verify with hawk_verify_finish(), and HAWK also calls the RNG
 * exactly twice per attempt (salt, then 40-byte sampler seed).
 */
static int
test_hawk_shim_logn(const api_param *param)
{
	unsigned logn = param->logn;
	static uint8_t priv[HAWK_PRIVKEY_SIZE(10)];
	static uint8_t priv_dec[HAWK_PRIVKEY_DECODED_SIZE(10)];
	static uint8_t pub[HAWK_PUBKEY_SIZE(10)];
	static uint8_t sig_enc[HAWK_SIG_SIZE(10)], sig_dec[HAWK_SIG_SIZE(10)];
	static uint8_t ktmp[HAWK_TMPSIZE_KEYGEN(10)];
	static uint8_t stmp[HAWK_TMPSIZE_SIGN(10)];
	static uint8_t vtmp[HAWK_TMPSIZE_VERIFY(10)];
	uint64_t key_state = UINT64_C(0x4A3B1C0DE0000000) + logn;
	unsigned signatures = 0, total_attempts = 0;

	for (unsigned key = 0; key < param->keys; key ++) {
		if (!hawk_keygen(logn, priv, pub, plain_rng, &key_state,
			ktmp, sizeof ktmp))
		{
			fprintf(stderr, "ERR: hawk_keygen failed logn=%u\n",
				logn);
			return 0;
		}
		hawk_decode_private_key(logn, priv_dec, priv);
		for (unsigned msg = 0; msg < param->messages; msg ++) {
			uint64_t rng_seed = UINT64_C(0x4A5E5EED00000000)
				+ ((uint64_t)logn << 24) + (key << 8) + msg;
			count_rng rng_enc, rng_dec;
			shake_context sc;

			count_rng_init(&rng_enc, rng_seed,
				e8_salt_len(logn), 40);
			count_rng_init(&rng_dec, rng_seed,
				e8_salt_len(logn), 40);
			make_message(&sc, logn, key, msg, 0);
			if (!hawk_sign_finish(logn, counting_rng, &rng_enc,
				sig_enc, &sc, priv, stmp, sizeof stmp)
				|| !hawk_shim_sign_finish_decoded(logn,
				counting_rng, &rng_dec, sig_dec, &sc, priv_dec,
				stmp, sizeof stmp))
			{
				fprintf(stderr, "ERR: HAWK signing failed"
					" logn=%u\n", logn);
				return 0;
			}
			if (rng_enc.calls == 0 || (rng_enc.calls & 1u) != 0
				|| rng_dec.calls == 0 || (rng_dec.calls & 1u) != 0
				|| !rng_enc.pattern_ok || !rng_dec.pattern_ok)
			{
				fprintf(stderr, "ERR: HAWK RNG calls logn=%u"
					" encoded=%u decoded=%u\n",
					logn, rng_enc.calls, rng_dec.calls);
				return 0;
			}
			if (!hawk_verify_finish(logn, sig_enc, HAWK_SIG_SIZE(logn),
				&sc, pub, HAWK_PUBKEY_SIZE(logn), vtmp, sizeof vtmp)
				|| !hawk_verify_finish(logn, sig_dec,
				HAWK_SIG_SIZE(logn), &sc, pub,
				HAWK_PUBKEY_SIZE(logn), vtmp, sizeof vtmp))
			{
				fprintf(stderr, "ERR: HAWK signature rejected"
					" logn=%u\n", logn);
				return 0;
			}
			make_message(&sc, logn, key, msg, 1);
			if (hawk_verify_finish(logn, sig_dec, HAWK_SIG_SIZE(logn),
				&sc, pub, HAWK_PUBKEY_SIZE(logn), vtmp, sizeof vtmp))
			{
				fprintf(stderr, "ERR: HAWK tampered message"
					" accepted logn=%u\n", logn);
				return 0;
			}
			signatures += 2;
			total_attempts += (rng_enc.calls + rng_dec.calls) / 2;
		}
	}
	printf("HAWK shim n=%u summary: signatures=%u total_attempts=%u\n",
		1u << logn, signatures, total_attempts);
	return 1;
}

int
main(void)
{
	for (size_t u = 0; u < sizeof PARAMS / sizeof PARAMS[0]; u ++) {
		if (!test_e8_api_logn(&PARAMS[u])
			|| !test_hawk_shim_logn(&PARAMS[u]))
		{
			return 1;
		}
	}
	return 0;
}
