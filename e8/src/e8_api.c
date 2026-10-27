#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <assert.h>

#if HAWK_E8_PROFILE_SIGN
#include "cmn_timing.h"
#endif

/*
 * Implementation of the public HAWK-E8-CM API (hawk_e8.h): key pair
 * generation, key encoding, key preparation, signing with an encoded or
 * prepared private key, and verification against an encoded public key.
 * Signing itself is e8_sign_sampler_core() in e8_sign.c.
 */

/*
 * Attempt cap for the public signing API. HAWK loops until success; an
 * E8 signature needs one attempt in nearly all cases, so this cap is
 * never reached in practice.
 */
#define E8_SIGN_MAX_ATTEMPTS   65536u

#define E8_PREPARED_KEY_MAGIC   UINT32_C(0x45385052)

/*
 * Key generation limits on the public form (as HAWK's bits_lim00,
 * bits_lim01 and lim11 in make_q001()): every coefficient x of an entry
 * satisfies |x| < 2^bits, with the constant terms q00[0] and q11[0]
 * limited apart. Per logn - 8: q00[0], q00[u] (u > 0), q01, q11[0],
 * q11[u] (u > 0). e8_keygen() discards and regenerates a key outside
 * them; e8_decode_public_key() rejects a public key whose q00 or q01 is
 * outside them. They are at least one bit above the maxima observed over
 * 10^4 keys per degree (tools/e8_keygen_limits.c), and with them the
 * verifier's E8_VRFY_PRIMES primes are sound (docs/verifier_range.md).
 */
static const uint8_t E8_KEYGEN_LIM_BITS[3][5] = {
	{ 14, 12, 15, 19, 18 },
	{ 16, 14, 16, 21, 20 },
	{ 18, 16, 18, 24, 23 }
};

/* see hawk_e8_inner.h */
void
e8_keygen_limits_bits(unsigned logn, unsigned *bits)
{
	for (unsigned u = 0; u < 5; u ++) {
		bits[u] = (logn >= 8 && logn <= 10)
			? E8_KEYGEN_LIM_BITS[logn - 8][u] : 0;
	}
}

/*
 * Return 1 if every coefficient of a (n values, constant term limited
 * apart when lim0 != 0) is within the limits.
 */
static int
poly_within(const int32_t *a, size_t n, unsigned lim0, unsigned lim)
{
	int32_t b0 = (int32_t)1 << (lim0 != 0 ? lim0 : lim);
	int32_t b = (int32_t)1 << lim;
	uint32_t r = 0;

	r |= (uint32_t)(a[0] <= -b0) | (uint32_t)(a[0] >= b0);
	for (size_t u = 1; u < n; u ++) {
		r |= (uint32_t)(a[u] <= -b) | (uint32_t)(a[u] >= b);
	}
	return r == 0;
}

/* see hawk_e8_inner.h */
int
e8_keygen_limits_ok(unsigned logn, const int32_t *q00, const int32_t *q01,
	const int32_t *q11)
{
	if (logn < 8 || logn > 10) {
		return 0;
	}
	size_t n = (size_t)1 << logn;
	const uint8_t *lim = E8_KEYGEN_LIM_BITS[logn - 8];

	return poly_within(q00, n, lim[0], lim[1])
		&& poly_within(q01, n, 0, lim[2])
		&& (q11 == NULL || poly_within(q11, n, lim[3], lim[4]));
}

/*
 * Prepared private key layout (8-byte aligned inside the caller's
 * E8_PRIVKEY_PREPARED_SIZE(logn) buffer). f, g, F and G are stored
 * contiguously, n bytes each, so that f || g is the 2*n-byte salt key
 * material, as in HAWK's decoded private key.
 */
typedef struct {
	uint32_t magic;
	uint32_t logn;
	int8_t fgFG[4 * E8_NTT_MAXN];
	uint8_t hpub[64];
	e8_inverse_w_ntt_basis basis_ntt;
	e8_coset_f2_basis basis_f2;
} e8_prepared_key;

typedef char e8_prepared_key_size_check[
	(sizeof(e8_prepared_key) + 7 <= E8_PRIVKEY_PREPARED_SIZE(10))
	? 1 : -1];

static size_t
kgseed_len(unsigned logn)
{
	return 8 + ((size_t)1 << (logn - 5));
}

/*
 * Align a buffer to 8 bytes, as HAWK does for its tmp buffers. Returns
 * NULL if fewer than need bytes remain after alignment.
 */
static void *
align8(void *buf, size_t len, size_t need)
{
	uintptr_t u1 = (uintptr_t)buf;
	uintptr_t u2 = (u1 + 7) & ~(uintptr_t)7;

	if (buf == NULL || len < (size_t)(u2 - u1)
		|| len - (size_t)(u2 - u1) < need)
	{
		return NULL;
	}
	return (void *)u2;
}

static void
enc_i32le(uint8_t *buf, int32_t x)
{
	uint32_t y = (uint32_t)x;

	buf[0] = (uint8_t)y;
	buf[1] = (uint8_t)(y >> 8);
	buf[2] = (uint8_t)(y >> 16);
	buf[3] = (uint8_t)(y >> 24);
}

static int32_t
dec_i32le(const uint8_t *buf)
{
	uint32_t x = (uint32_t)buf[0]
		| ((uint32_t)buf[1] << 8)
		| ((uint32_t)buf[2] << 16)
		| ((uint32_t)buf[3] << 24);

	return x < 0x80000000u
		? (int32_t)x : -(int32_t)(~x) - 1;
}

/*
 * hpub = SHAKE256(pub), truncated to n/16 bytes (same rule as HAWK).
 */
static void
hash_public_key(unsigned logn, uint8_t *hpub, const void *pub)
{
	shake_context sc;

	shake_init(&sc, 256);
	shake_inject(&sc, pub, E8_PUBKEY_SIZE(logn));
	shake_flip(&sc);
	shake_extract(&sc, hpub, (size_t)1 << (logn - 4));
}

/*
 * The verifier divides by q00 in the NTT domain modulo each of its
 * E8_VRFY_PRIMES primes; a key whose q00 has a zero NTT coefficient
 * modulo one of them could never verify. q00 is auto-adjoint, so the
 * first n/2 NTT values are all of them. tq is scratch space for n/2
 * words.
 */
static int
q00_invertible(unsigned logn, const int32_t *q00, uint32_t *tq)
{
	size_t hn = (size_t)1 << (logn - 1);

	for (unsigned u = 0; u < E8_VRFY_PRIMES; u ++) {
		uint32_t z = 0;

		e8_poly_to_ntt_autoadj_tab(logn, tq, q00, e8_ntt_tab_get(u));
		for (size_t v = 0; v < hn; v ++) {
			z |= (uint32_t)(tq[v] == 0);
		}
		if (z != 0) {
			return 0;
		}
	}
	return 1;
}

/* see hawk_e8.h */
int
e8_keygen(unsigned logn, void *priv, void *pub,
	hawk_rng rng, void *rng_context, void *tmp, size_t tmp_len)
{
	if (logn < 8 || logn > 10 || priv == NULL || pub == NULL
		|| rng == NULL)
	{
		return 0;
	}
	size_t n = (size_t)1 << logn;
	size_t seed_len = kgseed_len(logn);

	/*
	 * Memory layout (8-byte aligned):
	 *    f, g, F, G   n bytes each
	 *    work         20*n bytes (Hawk_keygen temporary area), then
	 *                 q00, q01, q11 (n x int32 each) and the
	 *                 invertibility check's scratch (n/2 words)
	 */
	int8_t *f = align8(tmp, tmp_len, (size_t)24 << logn);
	if (f == NULL) {
		return 0;
	}
	int8_t *g = f + n;
	int8_t *F = g + n;
	int8_t *G = F + n;
	uint8_t *work = (uint8_t *)(G + n);
	int32_t *q00 = (int32_t *)work;
	int32_t *q01 = q00 + n;
	int32_t *q11 = q01 + n;
	uint32_t *tq = (uint32_t *)(q11 + n);
	uint8_t seed[40];

	for (;;) {
		int fg_ok = 1;

		if (Hawk_keygen(logn, f, g, F, G, NULL, NULL, NULL, seed,
			rng, rng_context, work, (size_t)20 << logn) != 0)
		{
			return 0;
		}

		/*
		 * The NTRU solver rejects F and G unless every coefficient
		 * is in [-127, +127] (coeff_FG_limit in the Hawk solving
		 * profiles), so they fit the int8 key format.
		 */
		for (size_t u = 0; u < n; u ++) {
			if (F[u] < -127 || G[u] < -127) {
				fg_ok = 0;
			}
		}
		assert(fg_ok);
		if (!fg_ok) {
			continue;
		}

		/*
		 * The public form (q00, q01, q11; q10 = adj(q01) is not
		 * needed), its coefficient limits and the invertibility of
		 * q00 modulo the verifier primes; a key failing either is
		 * discarded, as HAWK discards keys in make_q001().
		 */
		e8_compute_keygen_qform(q00, q01, q11, f, g, F, G, logn);
		if (!e8_keygen_limits_ok(logn, q00, q01, q11)
			|| !q00_invertible(logn, q00, tq))
		{
			continue;
		}

		uint8_t *buf = pub;
		for (size_t u = 0; u < n; u ++, buf += 4) {
			enc_i32le(buf, q00[u]);
		}
		for (size_t u = 0; u < n; u ++, buf += 4) {
			enc_i32le(buf, q01[u]);
		}

		buf = priv;
		memcpy(buf, seed, seed_len);
		buf += seed_len;
		memcpy(buf, F, n);
		buf += n;
		memcpy(buf, G, n);
		buf += n;
		hash_public_key(logn, buf, pub);
		return 1;
	}
}

/* see hawk_e8.h */
void
e8_sign_start(shake_context *sc_data)
{
	shake_init(sc_data, 256);
}

/*
 * Expand an encoded private key into pk: regenerate f and g from kgseed,
 * copy F, G and hpub, and build the NTT and F2 basis tables.
 */
static int
prepare_key(unsigned logn, e8_prepared_key *pk, const void *priv)
{
	size_t n = (size_t)1 << logn;
	size_t seed_len = kgseed_len(logn);
	const uint8_t *buf = priv;
	int8_t *f = pk->fgFG;
	int8_t *g = f + n;
	int8_t *F = g + n;
	int8_t *G = F + n;

	pk->magic = 0;
	pk->logn = 0;
	Hawk_regen_fg(logn, f, g, buf);
	buf += seed_len;
	memcpy(F, buf, n);
	buf += n;
	memcpy(G, buf, n);
	buf += n;
	memcpy(pk->hpub, buf, (size_t)1 << (logn - 4));
	e8_inverse_w_ntt_prepare(&pk->basis_ntt, f, g, F, G, logn);
	e8_coset_f2_prepare(&pk->basis_f2, f, g, F, G, n);
	if (pk->basis_ntt.magic != E8_INVERSE_W_NTT_MAGIC
		|| pk->basis_f2.magic != E8_COSET_F2_MAGIC)
	{
		return 0;
	}
	pk->logn = logn;
	pk->magic = E8_PREPARED_KEY_MAGIC;
	return 1;
}

/* see hawk_e8.h */
int
e8_prepare_private_key(unsigned logn,
	void *priv_prepared, const void *priv)
{
	if (logn < 8 || logn > 10 || priv == NULL) {
		return 0;
	}
	e8_prepared_key *pk = align8(priv_prepared,
		E8_PRIVKEY_PREPARED_SIZE(logn), sizeof *pk);
	if (pk == NULL) {
		return 0;
	}
	return prepare_key(logn, pk, priv);
}

/* see hawk_e8_inner.h */
int
e8_sign_finish_inner(unsigned logn,
	hawk_rng rng, void *rng_context,
	void *sig, const shake_context *sc_data,
	const void *priv, size_t priv_len, void *tmp, size_t tmp_len,
	const e8_sampler_variant *variant,
	unsigned *trace_attempts, e8_sign_trace_timing *trace_timing)
{
	const e8_prepared_key *pk;
	const void *salt_key;
	size_t salt_key_len;
	int r;
#if HAWK_E8_PROFILE_SIGN
	uint64_t prep_cycles = 0, prep_wall_ns = 0;
#endif

	if (trace_timing != NULL) {
		memset(trace_timing, 0, sizeof *trace_timing);
	}
	if (trace_attempts != NULL) {
		*trace_attempts = 0;
	}
	if (logn < 8 || logn > 10 || rng == NULL || sig == NULL
		|| sc_data == NULL || priv == NULL)
	{
		return 0;
	}
	if (variant == NULL) {
		variant = e8_sampler_variant_default();
	}
	size_t n = (size_t)1 << logn;

	/*
	 * Check whether the private key is encoded or prepared. The
	 * encoded key is expanded into tmp on every call, and its salt key
	 * material is kgseed; a prepared key uses f || g, as HAWK does
	 * with a decoded private key.
	 */
	if (priv_len == E8_PRIVKEY_SIZE(logn)) {
#if HAWK_E8_PROFILE_SIGN
		uint64_t c0 = cmn_tsc_start();
		uint64_t w0 = cmn_wall_ns();
#endif
		e8_prepared_key *tpk = tmp_len < E8_TMPSIZE_SIGN(logn)
			? NULL : align8(tmp, tmp_len, sizeof *tpk);

		if (tpk == NULL || !prepare_key(logn, tpk, priv)) {
			return 0;
		}
#if HAWK_E8_PROFILE_SIGN
		prep_wall_ns = cmn_wall_ns() - w0;
		prep_cycles = cmn_tsc_end() - c0;
#endif
		pk = tpk;
		salt_key = priv;
		salt_key_len = kgseed_len(logn);
	} else if (priv_len == E8_PRIVKEY_PREPARED_SIZE(logn)) {
		pk = align8((void *)priv, priv_len, sizeof *pk);
		if (pk == NULL || pk->magic != E8_PREPARED_KEY_MAGIC
			|| pk->logn != logn)
		{
			return 0;
		}
		salt_key = pk->fgFG;
		salt_key_len = n << 1;
	} else {
		return 0;
	}

	r = e8_sign_sampler_core(logn, sig, E8_SIG_SIZE(logn), sc_data,
		pk->hpub, (size_t)1 << (logn - 4),
		pk->fgFG, pk->fgFG + n, pk->fgFG + 2 * n, pk->fgFG + 3 * n,
		&pk->basis_ntt, &pk->basis_f2, NULL, salt_key, salt_key_len,
		variant->sample_z, variant->tables(logn),
		e8_norm_bound_default(logn), E8_SIGN_MAX_ATTEMPTS,
		rng, rng_context, NULL, NULL, NULL, trace_attempts,
		trace_timing);
#if HAWK_E8_PROFILE_SIGN
	if (trace_timing != NULL) {
		trace_timing->cycles_prepare_total += prep_cycles;
		trace_timing->wall_ns_prepare_total += prep_wall_ns;
		trace_timing->cycles_sign_total += prep_cycles;
		trace_timing->wall_ns_sign_total += prep_wall_ns;
	}
#endif
	return r;
}

/* see hawk_e8.h */
int
e8_sign_finish(unsigned logn,
	hawk_rng rng, void *rng_context,
	void *sig, const shake_context *sc_data,
	const void *priv, void *tmp, size_t tmp_len)
{
	if (logn < 8 || logn > 10) {
		return 0;
	}
	return e8_sign_finish_inner(logn, rng, rng_context, sig, sc_data,
		priv, E8_PRIVKEY_SIZE(logn), tmp, tmp_len, NULL, NULL, NULL);
}

/* see hawk_e8.h */
int
e8_sign_finish_prepared(unsigned logn,
	hawk_rng rng, void *rng_context,
	void *sig, const shake_context *sc_data,
	const void *priv_prepared, void *tmp, size_t tmp_len)
{
	if (logn < 8 || logn > 10) {
		return 0;
	}
	return e8_sign_finish_inner(logn, rng, rng_context, sig, sc_data,
		priv_prepared, E8_PRIVKEY_PREPARED_SIZE(logn),
		tmp, tmp_len, NULL, NULL, NULL);
}

/*
 * Decode the encoded public key pub (E8_PUBKEY_SIZE(logn) bytes) into
 * q00, q01 (n x int32 each) and hpub (n/16 bytes), with the checks of
 * e8_decode_public_key(): q00 and q01 within the key generation limits,
 * q00 auto-adjoint (q00[n/2] = 0, q00[n - u] = -q00[u]).
 */
static int
decode_public_key_inner(unsigned logn, int32_t *q00, int32_t *q01,
	uint8_t *hpub, const void *pub)
{
	size_t n = (size_t)1 << logn;
	const uint8_t *buf = pub;
	uint32_t r = 0;

	for (size_t u = 0; u < n; u ++, buf += 4) {
		q00[u] = dec_i32le(buf);
	}
	for (size_t u = 0; u < n; u ++, buf += 4) {
		q01[u] = dec_i32le(buf);
	}
	r |= (uint32_t)q00[n >> 1];
	for (size_t u = 1; u < (n >> 1); u ++) {
		r |= (uint32_t)(q00[n - u] + q00[u]);
	}
	if (r != 0 || !e8_keygen_limits_ok(logn, q00, q01, NULL)) {
		return 0;
	}
	hash_public_key(logn, hpub, pub);
	return 1;
}

/* see hawk_e8.h */
int
e8_decode_public_key(unsigned logn,
	void *pub_decoded, const void *pub, size_t pub_len)
{
	if (logn < 8 || logn > 10 || pub_decoded == NULL || pub == NULL
		|| pub_len != E8_PUBKEY_SIZE(logn)
		|| ((uintptr_t)pub_decoded & 3) != 0)
	{
		return 0;
	}
	size_t n = (size_t)1 << logn;
	int32_t *q00 = pub_decoded;

	return decode_public_key_inner(logn, q00, q00 + n,
		(uint8_t *)(q00 + 2 * n), pub);
}

/* see hawk_e8.h */
int
e8_verify_finish(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data,
	const void *pub, size_t pub_len,
	void *tmp, size_t tmp_len)
{
	const int32_t *q00, *q01;
	const uint8_t *hpub;
	uint8_t hpub_buf[64];

	if (logn < 8 || logn > 10 || sig == NULL || sc_data == NULL
		|| pub == NULL || sig_len != E8_SIG_SIZE(logn))
	{
		return 0;
	}
	size_t n = (size_t)1 << logn;

	if (pub_len == (size_t)-1) {
		/*
		 * Decoded public key (e8_decode_public_key()).
		 */
		if (((uintptr_t)pub & 3) != 0) {
			return 0;
		}
		q00 = pub;
		q01 = q00 + n;
		hpub = (const uint8_t *)(q01 + n);
	} else {
		/*
		 * Encoded public key: decoded (with the limit checks) and
		 * hashed into tmp on every call.
		 */
		int32_t *dq00 = align8(tmp, tmp_len, (size_t)8 << logn);

		if (pub_len != E8_PUBKEY_SIZE(logn) || dq00 == NULL
			|| !decode_public_key_inner(logn, dq00, dq00 + n,
				hpub_buf, pub))
		{
			return 0;
		}
		q00 = dq00;
		q01 = dq00 + n;
		hpub = hpub_buf;
	}
	return e8_verify_uncompressed(logn, sig, sig_len, sc_data,
		hpub, (size_t)1 << (logn - 4), q00, q01);
}

/*
 * The reference verifier: the submission's e8_verify_finish() (encoded
 * public key, decoded and hashed on every call, no key limit check),
 * unchanged; with pub_len == (size_t)-1, pub is a decoded public key
 * (e8_decode_public_key()) and the decoding and hashing are skipped.
 */
/* see hawk_e8_inner.h */
int
e8_verify_finish_ref(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data,
	const void *pub, size_t pub_len,
	void *tmp, size_t tmp_len)
{
	uint8_t hpub[64];

	if (logn < 8 || logn > 10 || sig == NULL || sc_data == NULL
		|| pub == NULL || sig_len != E8_SIG_SIZE(logn))
	{
		return 0;
	}
	size_t n = (size_t)1 << logn;
	if (pub_len == (size_t)-1) {
		const int32_t *dq00 = pub;

		if (((uintptr_t)pub & 3) != 0) {
			return 0;
		}
		return e8_verify_uncompressed_ref(logn, sig, sig_len, sc_data,
			dq00 + 2 * n, (size_t)1 << (logn - 4),
			dq00, dq00 + n, NULL, NULL);
	}
	if (pub_len != E8_PUBKEY_SIZE(logn)) {
		return 0;
	}
	int32_t *q00 = align8(tmp, tmp_len, (size_t)8 << logn);
	if (q00 == NULL) {
		return 0;
	}
	int32_t *q01 = q00 + n;
	const uint8_t *buf = pub;

	for (size_t u = 0; u < n; u ++, buf += 4) {
		q00[u] = dec_i32le(buf);
	}
	for (size_t u = 0; u < n; u ++, buf += 4) {
		q01[u] = dec_i32le(buf);
	}
	hash_public_key(logn, hpub, pub);

	/*
	 * The completion-norm verifier uses only q00 and q01.
	 */
	return e8_verify_uncompressed_ref(logn, sig, sig_len, sc_data,
		hpub, (size_t)1 << (logn - 4), q00, q01, NULL, NULL);
}

#endif
