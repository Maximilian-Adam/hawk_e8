#ifndef HAWK_E8_H__
#define HAWK_E8_H__

/*
 * Public API of the experimental HAWK-E8-CM signature scheme.
 *
 * This header mirrors hawk.h: key pair generation, signature generation
 * through a sign_start / sign_finish pair, and verification, with the same
 * random source type, SHAKE256 message context, and temporary-buffer
 * conventions as ordinary HAWK. A prepared private key plays the role of
 * HAWK's decoded private key.
 *
 * The scheme is research code: it uses a floating-point, data-dependent,
 * non-constant-time sampler, and its key and signature formats are
 * experimental and uncompressed. It is available only when compiled with
 * HAWK_ENABLE_E8_EXPERIMENTAL=1.
 *
 * All sizes below are in bytes. Notation: n = 2^logn, with logn in
 * { 8, 9, 10 }; seed_len = 8 + n/32 (16, 24 or 40) is HAWK's key
 * generation seed length; hpub_len = n/16 (16, 32 or 64) is HAWK's public
 * key hash length; salt_len is 14, 24 or 40.
 *
 * Encoded private key (E8_PRIVKEY_SIZE(logn) bytes):
 *
 *   kgseed (seed_len) || F (n x int8) || G (n x int8) || hpub (hpub_len)
 *
 * The signer needs the full F and G (not only F mod 2 and G mod 2, as in
 * HAWK) to compute w = B^-1 z. As in HAWK, f and g are regenerated from
 * kgseed with Hawk_regen_fg(). All coefficients of F and G are in
 * [-127, +127].
 *
 * Public key (E8_PUBKEY_SIZE(logn) bytes):
 *
 *   q00 (n x int32, little-endian) || q01 (n x int32, little-endian)
 *
 * These are the entries of the expanded public form Q_E8 = B^* S_n B
 * used by the verifier (q10 = adj(q01) and q11 are not stored). The public
 * key hash is hpub = SHAKE256(pub) truncated to hpub_len bytes, the same
 * rule as HAWK.
 *
 * Prepared private key (E8_PRIVKEY_PREPARED_SIZE(logn) bytes): f, g, F, G,
 * hpub, and the per-key NTT and F2 basis tables used by the signer, in an
 * implementation-defined layout. It is produced by
 * e8_prepare_private_key() and is valid only at the address where it was
 * prepared (it is aligned internally, as HAWK aligns its tmp buffers).
 *
 * Signature (E8_SIG_SIZE(logn) bytes):
 *
 *   salt (salt_len) || s0 (n x int16, little-endian)
 *                   || s1 (n x int16, little-endian)
 *
 * Decoded public key (E8_PUBKEY_DECODED_SIZE(logn) bytes): q00, q01
 * (n x int32 each) and hpub; see e8_decode_public_key().
 *
 *   degree (n)   private key   prepared key   public key   decoded   signature
 *       256          544           40960          2048       2064       1038
 *       512         1080           40960          4096       4128       2072
 *      1024         2152           40960          8192       8256       4136
 */

/*
 * hawk_inner.h provides hawk.h (hawk_rng, shake_context) and the Zh()
 * name mangling shared with HAWK; public E8 names are hawk_e8_*.
 */
#include "hawk_inner.h"

#ifndef HAWK_ENABLE_E8_EXPERIMENTAL
#define HAWK_ENABLE_E8_EXPERIMENTAL   0
#endif

#if HAWK_ENABLE_E8_EXPERIMENTAL

/*
 * Private key length (in bytes).
 */
#define E8_PRIVKEY_SIZE(logn) \
	(8u + (1u << ((logn) - 5)) + (2u << (logn)) + (1u << ((logn) - 4)))

/*
 * Prepared private key length (in bytes). The prepared basis tables have
 * a fixed maximum size, so this does not currently depend on logn.
 */
#define E8_PRIVKEY_PREPARED_SIZE(logn)   40960u

/*
 * Public key length (in bytes).
 */
#define E8_PUBKEY_SIZE(logn)    (8u << (logn))

/*
 * Signature length (in bytes).
 */
#define E8_SIG_SIZE(logn)       ((4u << (logn)) + 14u \
	+ 10u * (2u >> (10 - (logn))) + 6u * (1u >> (10 - (logn))))

/*
 * Temporary buffer size for key pair generation.
 */
#define E8_TMPSIZE_KEYGEN(logn)    ((24u << (logn)) + 7)

/*
 * Temporary buffer size for signature generation with an encoded private
 * key (the key is prepared in the temporary buffer on every call).
 * Signing with a prepared key does not use the temporary buffer.
 */
#define E8_TMPSIZE_SIGN(logn)      E8_PRIVKEY_PREPARED_SIZE(logn)

/*
 * Temporary buffer size for signature verification.
 */
#define E8_TMPSIZE_VERIFY(logn)    ((8u << (logn)) + 7)

/*
 * Decoded public key length (in bytes): q00 (n x int32), q01
 * (n x int32), hpub (n/16 bytes); see e8_decode_public_key().
 */
#define E8_PUBKEY_DECODED_SIZE(logn) \
	((8u << (logn)) + (1u << ((logn) - 4)))

#define e8_keygen                Zh(e8_keygen)
#define e8_sign_start            Zh(e8_sign_start)
#define e8_sign_finish           Zh(e8_sign_finish)
#define e8_prepare_private_key   Zh(e8_prepare_private_key)
#define e8_sign_finish_prepared  Zh(e8_sign_finish_prepared)
#define e8_verify_finish         Zh(e8_verify_finish)
#define e8_decode_public_key     Zh(e8_decode_public_key)

/*
 * Generate a new public/private key pair.
 *
 * The secret basis (f, g, F, G) is produced by HAWK's key generator
 * (Hawk_keygen(), keeping its seed), so f and g follow HAWK's
 * distribution exactly. Candidates whose E8 form is outside the key
 * generation limits (a bound on every coefficient of q00, q01 and q11,
 * as HAWK's make_q001(); never hit in 10^5 keys per degree), or whose
 * q00 is not invertible modulo every verifier NTT prime, are discarded
 * and generation restarts.
 *
 * The private key is written into 'priv' (E8_PRIVKEY_SIZE(logn) bytes)
 * and the public key into 'pub' (E8_PUBKEY_SIZE(logn) bytes); both must
 * be non-NULL. The temporary buffer must have at least
 * E8_TMPSIZE_KEYGEN(logn) bytes.
 *
 * Returned value is 1 on success, 0 on error (unsupported logn, NULL
 * output, or temporary buffer too short).
 */
int e8_keygen(unsigned logn, void *priv, void *pub,
	hawk_rng rng, void *rng_context, void *tmp, size_t tmp_len);

/*
 * Start a signature process by initializing the provided SHAKE context.
 * This is equivalent to calling shake_init(sc_data, 256) (and to
 * hawk_sign_start()). The caller then injects the message with
 * shake_inject(). The same function works for verification.
 */
void e8_sign_start(shake_context *sc_data);
#define e8_verify_start   e8_sign_start

/*
 * Generate an E8 signature.
 *    logn          log of degree (8 to 10, for n = 256 to 1024)
 *    rng           random source to use for salts and sampling
 *    rng_context   context for the random source
 *    sig           signature output buffer (E8_SIG_SIZE(logn) bytes)
 *    sc_data       SHAKE256 context with message injected (_not_ flipped)
 *    priv          private key (encoded)
 *    tmp           temporary buffer
 *    tmp_len       temporary buffer length (in bytes)
 *
 * The signing process mirrors hawk_sign_finish():
 *
 *   hm = SHAKE256(message || hpub)                          (once)
 *   per attempt (attempt = 0, 2, 4, ...):
 *     salt = SHAKE256(hm || kgseed || enc32le(attempt)
 *                     || rng(salt_len))                     (salt_len)
 *     h = SHAKE256(hm || salt)                              (2*n bits)
 *     t = B*h mod 2; z <- E8 sampler on 2R^2 + t
 *     reject if ||P_n z||^2 is above the verification bound
 *     w = B^-1 z; sign symmetry break; s = (h - w)/2
 *     reject if s does not fit in int16; encode
 *
 * The RNG is invoked exactly twice per attempt: salt_len bytes for the
 * salt, then a 32-byte sampler seed (per-worker sampler streams are
 * derived from that seed). f and g are regenerated from kgseed and the
 * NTT and F2 basis tables are rebuilt on every call, in tmp.
 *
 * The signing parameters are the prototype values
 * (sigma_sign, sigma_verify) = (3.592, 2.03), (3.631, 1.99) and
 * (3.669, 1.95) for n = 256, 512 and 1024. The number of attempts is
 * capped internally at a value that is never reached in practice.
 *
 * Returned value is 1 on success, 0 on error (unsupported logn, invalid
 * key, temporary buffer too short, sampler failure, or attempt cap
 * reached). The sampler thread count is taken from the process-global
 * sampler configuration (see e8_sampler_set_thread_count()).
 *
 * Minimum temporary buffer size: E8_TMPSIZE_SIGN(logn).
 */
int e8_sign_finish(unsigned logn,
	hawk_rng rng, void *rng_context,
	void *sig, const shake_context *sc_data,
	const void *priv, void *tmp, size_t tmp_len);

/*
 * Prepare a private key for repeated signing. The input private key
 * (priv) has exactly E8_PRIVKEY_SIZE(logn) bytes; the prepared key is
 * written into priv_prepared (E8_PRIVKEY_PREPARED_SIZE(logn) bytes). This
 * regenerates f and g and builds the per-key NTT and F2 basis tables.
 *
 * Returned value is 1 on success, 0 on error.
 */
int e8_prepare_private_key(unsigned logn,
	void *priv_prepared, const void *priv);

/*
 * Generate an E8 signature with a prepared private key. This is
 * e8_sign_finish() without the per-call key preparation; as in HAWK's
 * decoded-key mode, the salt derivation absorbs f || g (2*n bytes) in
 * place of kgseed. The temporary buffer is not used (tmp may be NULL).
 */
int e8_sign_finish_prepared(unsigned logn,
	hawk_rng rng, void *rng_context,
	void *sig, const shake_context *sc_data,
	const void *priv_prepared, void *tmp, size_t tmp_len);

/*
 * Decode a public key. The encoded public key is provided in pub, with
 * size pub_len; if the length is not exactly E8_PUBKEY_SIZE(logn) bytes,
 * an error is reported.
 *
 * The decoded key, E8_PUBKEY_DECODED_SIZE(logn) bytes, is written into
 * pub_decoded, which must be 32-bit aligned: q00 (n x int32_t), q01
 * (n x int32_t), then hpub = SHAKE256(pub) truncated to n/16 bytes.
 * It holds no precomputed NTT (as HAWK's decoded public key).
 *
 * As hawk_decode_public_key(), this checks that every coefficient of q00
 * and q01 is within the key generation limits (and that q00 is
 * auto-adjoint), and rejects the key otherwise.
 *
 * Returned value is 1 on success, 0 on error.
 */
int e8_decode_public_key(unsigned logn,
	void *pub_decoded, const void *pub, size_t pub_len);

/*
 * Verify an E8 signature.
 *    logn          log of degree (8 to 10, for n = 256 to 1024)
 *    sig           signature (encoded)
 *    sig_len       signature length (must be E8_SIG_SIZE(logn))
 *    sc_data       SHAKE256 context with message injected (_not_ flipped)
 *    pub           public key (encoded or decoded)
 *    pub_len       public key length, or (size_t)-1 for a decoded key
 *    tmp           temporary buffer
 *    tmp_len       temporary buffer length (in bytes)
 *
 * If pub_len is equal to -1 (cast to size_t), then pub should point at
 * the decoded public key, as obtained from e8_decode_public_key().
 * Otherwise, pub should contain the encoded public key, of length exactly
 * E8_PUBKEY_SIZE(logn) bytes; it is decoded (with the same checks) and
 * hashed on every call.
 *
 * The signature is checked with the completion-of-squares norm against
 * the default verification bound, with HAWK's verifier techniques:
 * E8_VRFY_PRIMES (3) NTT primes with precomputed twiddles, the constant
 * NTT of Delta, one batch inversion of q00 per prime, an integer bound
 * and an early reject after the first prime.
 *
 * Returned value is 1 on success (signature matches the provided public key
 * and hashed message), 0 on failure.
 *
 * Minimum temporary buffer size: E8_TMPSIZE_VERIFY(logn).
 */
int e8_verify_finish(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data,
	const void *pub, size_t pub_len,
	void *tmp, size_t tmp_len);

#endif

#endif
