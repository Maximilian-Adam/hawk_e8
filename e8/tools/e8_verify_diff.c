/*
 * Differential and negative tests of the HAWK-E8-CM verifier
 * (docs/verifier_range.md, docs/validation.md).
 *
 *   e8_verify_diff --logn 8|9|10 [--keys K] [--sigs-per-key S]
 *                  [--seed X] [--shard I] [--header]
 *   e8_verify_diff --test
 *
 * Per key (e8_keygen() from the shared signing RNG seeded with
 * "verify_diff" || u64 seed || u32 logn || u32 key index; key indices
 * I*K .. I*K + K - 1 of shard I) and per signature j (message j of the
 * key, signed with the ref sampler, encoded key if j is even, prepared
 * key if odd), every candidate below is verified by
 *
 *   fast_enc   e8_verify_finish(), encoded public key
 *   fast_dec   e8_verify_finish(), decoded public key
 *              (e8_decode_public_key() once per key)
 *   ref        e8_verify_finish_ref() (the reference verifier, 4 primes)
 *
 * and the three verdicts must agree (else a mismatch). Candidates:
 *
 *   honest     the signature (must be accepted by all three)
 *   boundary   the signature at the integer bounds N - 1 and N, with N
 *              its Q-norm (e8_verify_uncompressed_bound() against
 *              e8_verify_uncompressed_ref_bound(); must reject, accept)
 *   sigma      the signature at the bounds of sigma_verify scaled by
 *              0.97, 0.99, 1.01 and 1.03 (e8_verify_uncompressed_bound()
 *              at floor(8 n sigma^2) against
 *              e8_verify_uncompressed_with_sigma())
 *   flip_salt  one random bit of the salt flipped (3 per signature)
 *   flip_s0    one random bit of a random s0 coefficient (3)
 *   flip_s1    one random bit of a random s1 coefficient (3)
 *   random     random salt and s0, s1 uniform in [-2^b, 2^b) with b
 *              uniform in 0..15 (2)
 *
 * Every candidate also compares the Q-norm of its w = h - 2s
 * (e8_qnorm_completion() against e8_qnorm_completion_ref(): both fail
 * or both return the same norm). The flipped and random candidates are
 * the negative tests; a flipped one that is accepted (by all three) is
 * counted as tamper_accepts, which is not a failure in itself (a low-bit
 * flip of s can give another valid signature) but is reported.
 *
 * The decoded and encoded public key paths must also reject a public
 * key with a q00 or q01 coefficient just outside the key generation
 * limits, and a q00 that is not auto-adjoint (key_rejects counts them).
 *
 * Output: one CSV row (with --header, the header first). --test runs a
 * small fixed workload per degree and prints one line per degree (for
 * make test-e8). Exit status 0 if there is no mismatch, every honest
 * signature and boundary verdict is as expected and every bad public key
 * is rejected; 1 otherwise; 2 on a usage error.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hawk_e8_inner.h"

#define MAXN   1024

typedef struct {
	unsigned logn;
	unsigned keys;
	unsigned sigs_per_key;
	unsigned long long seed;
	unsigned shard;
} diff_options;

typedef struct {
	unsigned long long keys;
	unsigned long long signatures;
	unsigned long long candidates;
	unsigned long long accepts;
	unsigned long long mismatches;
	unsigned long long norm_mismatches;
	unsigned long long honest_failures;
	unsigned long long boundary_checks;
	unsigned long long boundary_failures;
	unsigned long long sigma_checks;
	unsigned long long tampered;
	unsigned long long tamper_accepts;
	unsigned long long random;
	unsigned long long random_accepts;
	unsigned long long key_rejects;
	unsigned long long key_reject_failures;
	unsigned long long sign_failures;
} diff_counts;

static uint8_t key_priv[E8_PRIVKEY_SIZE(10)];
static uint8_t key_pub[E8_PUBKEY_SIZE(10)];
static uint8_t key_bad[E8_PUBKEY_SIZE(10)];
static uint8_t key_prepared[E8_PRIVKEY_PREPARED_SIZE(10)];
static int32_t key_decoded[(E8_PUBKEY_DECODED_SIZE(10) + 3) / 4];
static int32_t key_bad_decoded[(E8_PUBKEY_DECODED_SIZE(10) + 3) / 4];
static uint8_t tmp_keygen[E8_TMPSIZE_KEYGEN(10)];
static uint8_t tmp_sign[E8_TMPSIZE_SIGN(10)];
static uint8_t tmp_verify[E8_TMPSIZE_VERIFY(10)];
static uint8_t sig[E8_SIG_SIZE(10)];
static uint8_t cand[E8_SIG_SIZE(10)];

static void
seeded_rng(cmn_sign_rng *rng, const char *label,
	unsigned long long seed, const uint32_t *fields, size_t num)
{
	uint8_t buf[64];
	size_t len = strlen(label);

	memcpy(buf, label, len);
	for (unsigned u = 0; u < 8; u ++) {
		buf[len ++] = (uint8_t)(seed >> (8 * u));
	}
	for (size_t i = 0; i < num; i ++) {
		for (unsigned u = 0; u < 4; u ++) {
			buf[len ++] = (uint8_t)(fields[i] >> (8 * u));
		}
	}
	cmn_sign_rng_init(rng, buf, len);
}

static uint32_t
rng_u32(cmn_sign_rng *rng)
{
	uint8_t b[4];

	cmn_sign_rng_hawk(rng, b, sizeof b);
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8)
		| ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static void
make_message(shake_context *sc, const diff_options *o, uint32_t key,
	uint32_t j)
{
	static const char prefix[] = "e8_verify_diff message";
	uint8_t buf[16];

	for (unsigned u = 0; u < 8; u ++) {
		buf[u] = (uint8_t)(o->seed >> (8 * u));
	}
	for (unsigned u = 0; u < 4; u ++) {
		buf[8 + u] = (uint8_t)(key >> (8 * u));
		buf[12 + u] = (uint8_t)(j >> (8 * u));
	}
	e8_sign_start(sc);
	shake_inject(sc, prefix, sizeof prefix - 1);
	shake_inject(sc, &o->logn, 1);
	shake_inject(sc, buf, sizeof buf);
}

static const uint8_t *
hpub_of(unsigned logn)
{
	return (const uint8_t *)(key_decoded + (2u << logn));
}

/*
 * Verify the candidate with the three verifiers; count a mismatch if
 * they disagree, and compare the Q-norms. Returns the common verdict
 * (or -1 on a mismatch).
 */
static int
check_candidate(const diff_options *o, diff_counts *c,
	const uint8_t *s, const shake_context *sc)
{
	unsigned logn = o->logn;
	size_t n = (size_t)1 << logn;
	size_t sig_len = E8_SIG_SIZE(logn);
	const int32_t *q00 = key_decoded;
	const int32_t *q01 = key_decoded + n;
	int32_t w0[MAXN], w1[MAXN];
	int64_t nf = -1, nr = -1;

	int fe = e8_verify_finish(logn, s, sig_len, sc, key_pub,
		E8_PUBKEY_SIZE(logn), tmp_verify, sizeof tmp_verify);
	int fd = e8_verify_finish(logn, s, sig_len, sc, key_decoded,
		(size_t)-1, NULL, 0);
	int rf = e8_verify_finish_ref(logn, s, sig_len, sc, key_pub,
		E8_PUBKEY_SIZE(logn), tmp_verify, sizeof tmp_verify);

	c->candidates ++;
	if (e8_sig_to_w(logn, w0, w1, s, sig_len, sc, hpub_of(logn))) {
		int okf = e8_qnorm_completion(&nf, q00, q01, w0, w1, logn);
		int okr = e8_qnorm_completion_ref(&nr, q00, q01, w0, w1, logn);

		if (okf != okr || (okf && nf != nr)) {
			c->norm_mismatches ++;
		}
	}
	if (fe != fd || fe != rf) {
		c->mismatches ++;
		return -1;
	}
	c->accepts += (unsigned)fe;
	return fe;
}

/*
 * Boundary and sigma checks of an honest signature: the integer-bound
 * entry points of both verifiers at N - 1 and N, and at the bounds of
 * scaled sigma_verify values.
 */
static void
check_bounds(const diff_options *o, diff_counts *c, const shake_context *sc)
{
	static const int SCALE_PCT[] = { 97, 99, 101, 103 };
	unsigned logn = o->logn;
	size_t n = (size_t)1 << logn;
	size_t sig_len = E8_SIG_SIZE(logn);
	size_t hpub_len = (size_t)1 << (logn - 4);
	const int32_t *q00 = key_decoded;
	const int32_t *q01 = key_decoded + n;
	int32_t w0[MAXN], w1[MAXN];
	int64_t norm;
	double sigma;

	if (!e8_sig_to_w(logn, w0, w1, sig, sig_len, sc, hpub_of(logn))
		|| !e8_qnorm_completion_ref(&norm, q00, q01, w0, w1, logn))
	{
		c->boundary_failures ++;
		return;
	}
	for (int64_t b = norm - 1; b <= norm; b ++) {
		int vf = e8_verify_uncompressed_bound(logn, sig, sig_len, sc,
			hpub_of(logn), hpub_len, q00, q01, b);
		int vr = e8_verify_uncompressed_ref_bound(logn, sig, sig_len,
			sc, hpub_of(logn), hpub_len, q00, q01, b);

		c->boundary_checks ++;
		c->candidates ++;
		if (vf != vr) {
			c->mismatches ++;
		}
		if (vf != (b >= norm)) {
			c->boundary_failures ++;
		}
	}
	(void)e8_sigma_verify_default(logn, &sigma);
	for (size_t i = 0; i < sizeof SCALE_PCT / sizeof SCALE_PCT[0]; i ++) {
		double s = sigma * SCALE_PCT[i] / 100.0;
		int64_t b;
		int vf, vr;

		if (!e8_verify_bound_from_sigma(logn, s, &b)) {
			c->boundary_failures ++;
			continue;
		}
		vf = e8_verify_uncompressed_bound(logn, sig, sig_len, sc,
			hpub_of(logn), hpub_len, q00, q01, b);
		vr = e8_verify_uncompressed_with_sigma(logn, sig, sig_len, sc,
			hpub_of(logn), hpub_len, q00, q01, NULL, NULL, s);
		c->sigma_checks ++;
		c->candidates ++;
		c->accepts += (unsigned)(vf & vr);
		if (vf != vr) {
			c->mismatches ++;
		}
	}
}

/*
 * Bad public keys: one q00 coefficient (u > 0, with its adjoint
 * partner) and one q01 coefficient just outside the limits, and a q00
 * that is not auto-adjoint, must be rejected by e8_decode_public_key()
 * and by e8_verify_finish() with the encoded key.
 */
static void
check_bad_keys(const diff_options *o, diff_counts *c,
	const shake_context *sc, cmn_sign_rng *rng)
{
	unsigned logn = o->logn;
	size_t n = (size_t)1 << logn;
	unsigned lim[5];

	e8_keygen_limits_bits(logn, lim);
	for (unsigned kind = 0; kind < 3; kind ++) {
		size_t u = 1 + rng_u32(rng) % (uint32_t)((n >> 1) - 1);
		int32_t x;

		memcpy(key_bad, key_pub, E8_PUBKEY_SIZE(logn));
		switch (kind) {
		case 0:
			x = (int32_t)1 << lim[1];
			memcpy(key_bad + 4 * u, &x, 4);
			x = -x;
			memcpy(key_bad + 4 * (n - u), &x, 4);
			break;
		case 1:
			x = -((int32_t)1 << lim[2]);
			memcpy(key_bad + 4 * (n + u), &x, 4);
			break;
		default:
			memcpy(&x, key_bad + 4 * u, 4);
			x ++;
			memcpy(key_bad + 4 * u, &x, 4);
			break;
		}
		c->key_rejects ++;
		if (e8_decode_public_key(logn, key_bad_decoded, key_bad,
			E8_PUBKEY_SIZE(logn))
			|| e8_verify_finish(logn, sig, E8_SIG_SIZE(logn), sc,
				key_bad, E8_PUBKEY_SIZE(logn),
				tmp_verify, sizeof tmp_verify))
		{
			c->key_reject_failures ++;
		}
	}
}

static void
run(const diff_options *o, diff_counts *c)
{
	unsigned logn = o->logn;
	size_t n = (size_t)1 << logn;
	size_t sig_len = E8_SIG_SIZE(logn);
	size_t salt_len = e8_salt_len(logn);

	memset(c, 0, sizeof *c);
	for (unsigned k = 0; k < o->keys; k ++) {
		uint32_t key = o->shard * o->keys + k;
		uint32_t fields[2] = { logn, key };
		cmn_sign_rng rng, trng;

		seeded_rng(&rng, "verify_diff", o->seed, fields, 2);
		seeded_rng(&trng, "verify_diff tamper", o->seed, fields, 2);
		if (!e8_keygen(logn, key_priv, key_pub, cmn_sign_rng_hawk,
			&rng, tmp_keygen, sizeof tmp_keygen)
			|| !e8_prepare_private_key(logn, key_prepared, key_priv)
			|| !e8_decode_public_key(logn, key_decoded, key_pub,
				E8_PUBKEY_SIZE(logn)))
		{
			c->sign_failures ++;
			continue;
		}
		c->keys ++;
		for (unsigned j = 0; j < o->sigs_per_key; j ++) {
			shake_context sc;
			int ok;

			make_message(&sc, o, key, j);
			if ((j & 1) == 0) {
				ok = e8_sign_finish(logn, cmn_sign_rng_hawk,
					&rng, sig, &sc, key_priv,
					tmp_sign, sizeof tmp_sign);
			} else {
				ok = e8_sign_finish_prepared(logn,
					cmn_sign_rng_hawk, &rng, sig, &sc,
					key_prepared, NULL, 0);
			}
			if (!ok) {
				c->sign_failures ++;
				continue;
			}
			c->signatures ++;
			if (check_candidate(o, c, sig, &sc) != 1) {
				c->honest_failures ++;
			}
			check_bounds(o, c, &sc);
			if (j == 0) {
				check_bad_keys(o, c, &sc, &trng);
			}

			/* One-bit flips: salt, s0, s1 (3 each). */
			for (unsigned part = 0; part < 3; part ++) {
				for (unsigned t = 0; t < 3; t ++) {
					uint32_t r = rng_u32(&trng);
					size_t byte;

					if (part == 0) {
						byte = (r >> 3) % salt_len;
					} else {
						byte = salt_len
							+ (part - 1) * 2 * n
							+ (r >> 3) % (2 * n);
					}
					memcpy(cand, sig, sig_len);
					cand[byte] ^= (uint8_t)(1u << (r & 7));
					c->tampered ++;
					if (check_candidate(o, c, cand, &sc)
						== 1)
					{
						c->tamper_accepts ++;
					}
				}
			}

			/* Random signatures. */
			for (unsigned t = 0; t < 2; t ++) {
				unsigned b = rng_u32(&trng) % 16;

				cmn_sign_rng_hawk(&trng, cand, salt_len);
				for (size_t u = 0; u < 2 * n; u ++) {
					uint32_t r = rng_u32(&trng);
					int32_t x = (int32_t)(r & ((2u << b) - 1))
						- ((int32_t)1 << b);
					uint16_t y = (uint16_t)x;

					cand[salt_len + 2 * u] = (uint8_t)y;
					cand[salt_len + 2 * u + 1] =
						(uint8_t)(y >> 8);
				}
				c->random ++;
				if (check_candidate(o, c, cand, &sc) == 1) {
					c->random_accepts ++;
				}
			}
		}
	}
}

static int
counts_ok(const diff_counts *c)
{
	return c->mismatches == 0 && c->norm_mismatches == 0
		&& c->honest_failures == 0 && c->boundary_failures == 0
		&& c->key_reject_failures == 0 && c->sign_failures == 0;
}

static void
print_row(const diff_options *o, const diff_counts *c, int header)
{
	if (header) {
		printf("logn,seed,shard,keys,signatures,candidates,accepts,"
			"mismatches,norm_mismatches,honest_failures,"
			"boundary_checks,boundary_failures,sigma_checks,"
			"tampered,tamper_accepts,random,random_accepts,"
			"key_rejects,key_reject_failures,sign_failures\n");
	}
	printf("%u,%llu,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
		"%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
		o->logn, o->seed, o->shard, c->keys, c->signatures,
		c->candidates, c->accepts, c->mismatches, c->norm_mismatches,
		c->honest_failures, c->boundary_checks, c->boundary_failures,
		c->sigma_checks, c->tampered, c->tamper_accepts, c->random,
		c->random_accepts, c->key_rejects, c->key_reject_failures,
		c->sign_failures);
}

int
main(int argc, char **argv)
{
	diff_options o;
	diff_counts c;
	int header = 0, test = 0;

	memset(&o, 0, sizeof o);
	o.keys = 2;
	o.sigs_per_key = 8;
	o.seed = 1;
	for (int i = 1; i < argc; i ++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;

		if (strcmp(a, "--header") == 0) {
			header = 1;
		} else if (strcmp(a, "--test") == 0) {
			test = 1;
		} else if (v != NULL && strcmp(a, "--logn") == 0) {
			o.logn = (unsigned)strtoul(v, NULL, 10);
			i ++;
		} else if (v != NULL && strcmp(a, "--keys") == 0) {
			o.keys = (unsigned)strtoul(v, NULL, 10);
			i ++;
		} else if (v != NULL && strcmp(a, "--sigs-per-key") == 0) {
			o.sigs_per_key = (unsigned)strtoul(v, NULL, 10);
			i ++;
		} else if (v != NULL && strcmp(a, "--seed") == 0) {
			o.seed = strtoull(v, NULL, 10);
			i ++;
		} else if (v != NULL && strcmp(a, "--shard") == 0) {
			o.shard = (unsigned)strtoul(v, NULL, 10);
			i ++;
		} else {
			fprintf(stderr, "usage: %s --logn 8|9|10 [--keys K]"
				" [--sigs-per-key S] [--seed X] [--shard I]"
				" [--header] | --test\n", argv[0]);
			return 2;
		}
	}
	e8_sampler_set_thread_count(1);
	if (test) {
		int ok = 1;

		for (unsigned logn = 8; logn <= 10; logn ++) {
			o.logn = logn;
			printf("E8 verifier differential n=%u: ", 1u << logn);
			fflush(stdout);
			run(&o, &c);
			if (!counts_ok(&c)) {
				printf("FAILED\n");
				print_row(&o, &c, 1);
				ok = 0;
				continue;
			}
			printf("%llu candidates, %llu accepted, fast (encoded,"
				" decoded) = ref; %llu bad keys rejected: done.\n",
				c.candidates, c.accepts, c.key_rejects);
		}
		return ok ? 0 : 1;
	}
	if (o.logn < 8 || o.logn > 10 || o.keys == 0) {
		fprintf(stderr, "e8_verify_diff: bad --logn or --keys\n");
		return 2;
	}
	run(&o, &c);
	print_row(&o, &c, header);
	return counts_ok(&c) ? 0 : 1;
}
