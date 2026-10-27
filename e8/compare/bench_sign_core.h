#ifndef BENCH_SIGN_CORE_H__
#define BENCH_SIGN_CORE_H__

#include <stddef.h>
#include <stdint.h>

#include "cmn_csv.h"
#include "cmn_pool.h"
#include "cmn_rng.h"
#include "cmn_timing.h"

/*
 * Signing core shared by the HAWK vs E8 signing benchmarks (e8_sign_bench,
 * bench_compare): key generation, key preparation, one timed sign_finish
 * call and verification for every sampler variant of both schemes, the
 * per-index seeds and messages, and the common fields of a CSV row.
 * e8_sign_bench.c and docs/perf_comparison_final.md describe the accounting:
 *
 *   all_in    the encoded private key is passed to sign_finish:
 *               HAWK ref     hawk_sign_finish()
 *               HAWK other   hawkv_sign_finish()
 *               E8           e8_sign_finish_inner(encoded key, variant)
 *   prepared  the key is expanded once per key outside the timed call
 *             (bsc_prepare(), timed separately):
 *               HAWK ref     hawk_shim_sign_finish_decoded()
 *               HAWK other   hawkv_sign_finish_decoded()
 *               E8           e8_sign_finish_inner(prepared key, variant)
 *
 * Seeds: the signing RNG of purpose stream_id, degree logn and index i is
 * the shared SHAKE256 signing RNG (cmn_sign_rng) seeded with
 * u32le(stream_id) || u32le(logn) || u32le(i). Key k of a scheme uses
 * stream 1 + scheme; signature i uses stream 16 + accounting; warm-up
 * signatures use stream 9. Messages depend on (logn, i) only. Both
 * schemes therefore sign the same messages with the same signing RNG
 * streams.
 *
 * attempts = rng_calls / 2 for every variant of both schemes; rng_bytes
 * is the signing RNG's bytes plus the sampler streams' delivered bytes
 * (upstream HAWK ref: the exact 20n bytes per attempt).
 *
 * With BSC_HAWK_ONLY = 1 (the HAWK AVX2 context build) only upstream HAWK
 * ref exists, under the variant name ref-avx2; the binary then links the
 * AVX2 objects and no E8 or HAWK-variant code.
 *
 * Key generation and verification timing (bench_compare --mode
 * keyverify):
 *
 *   keygen       each scheme's public keygen call (hawk_keygen(),
 *                e8_keygen()), RNG to encoded private and public key,
 *                retries included; both schemes from the paired stream
 *                BSC_STREAM_KEYGEN_PAIRED, index = key index, so they run
 *                HAWK's NTRU core on the same draws unless one restarts.
 *   keygen_core  Hawk_keygen() alone (the shared NTRU core, as
 *                e8_keygen() calls it) on the same seed.
 *   pub_decode   hawk_decode_public_key() / e8_decode_public_key().
 *   verify       all_in: verify_finish with the encoded signature and
 *                public key (HAWK with HAWK_TMPSIZE_VERIFY_FAST, E8 with
 *                E8_TMPSIZE_VERIFY); prepared: the decoded public key
 *                (pub_len = (size_t)-1), the signature still encoded.
 *                Verifiers: upstream (hawk_verify_finish()), fast
 *                (e8_verify_finish()) and ref (e8_verify_finish_ref()).
 *
 * attempts of a keygen row = RNG calls (one per (f, g) draw), rng_bytes =
 * bytes drawn.
 */

#ifndef BSC_HAWK_ONLY
#define BSC_HAWK_ONLY   0
#endif

#if BSC_HAWK_ONLY
#include "hawk.h"
#else
#include "../src/hawk_e8_inner.h"
#include "hawk_variants.h"
#endif

#define BSC_SCHEME_HAWK   0
#define BSC_SCHEME_E8     1
#define BSC_SCHEME_COUNT  2

#define BSC_ACCT_ALL_IN    0
#define BSC_ACCT_PREPARED  1
#define BSC_ACCT_COUNT     2

#define BSC_STREAM_KEYGEN          1     /* + scheme */
#define BSC_STREAM_WARMUP          9
#define BSC_STREAM_SIGN            16    /* + accounting */
#define BSC_STREAM_KEYGEN_PAIRED   24    /* keyverify: both schemes */
#define BSC_STREAM_KEYGEN_WARMUP   25    /* keyverify warm-up keys */

#define BSC_VERIFIER_UPSTREAM   0     /* HAWK hawk_verify_finish() */
#define BSC_VERIFIER_FAST       1     /* E8 e8_verify_finish() */
#define BSC_VERIFIER_REF        2     /* E8 e8_verify_finish_ref() */
#define BSC_VERIFIER_COUNT      3

extern const char *const bsc_scheme_name[BSC_SCHEME_COUNT];
extern const char *const bsc_acct_name[BSC_ACCT_COUNT];
extern const char *const bsc_verifier_name[BSC_VERIFIER_COUNT];

#if BSC_HAWK_ONLY
#define BSC_PRIV_SIZE       HAWK_PRIVKEY_SIZE(10)
#define BSC_PUB_SIZE        HAWK_PUBKEY_SIZE(10)
#define BSC_PREPARED_SIZE   HAWK_PRIVKEY_DECODED_SIZE(10)
#define BSC_SIG_SIZE        HAWK_SIG_SIZE(10)
#define BSC_PUB_DECODED_SIZE   (2 * HAWK_DECODED_PUB_LENGTH(10))
#else
#define BSC_PRIV_SIZE       E8_PRIVKEY_SIZE(10)
#define BSC_PUB_SIZE        E8_PUBKEY_SIZE(10)
#define BSC_PREPARED_SIZE   E8_PRIVKEY_PREPARED_SIZE(10)
#define BSC_SIG_SIZE        E8_SIG_SIZE(10)
#define BSC_PUB_DECODED_SIZE   E8_PUBKEY_DECODED_SIZE(10)
#endif

/*
 * One key of either scheme in both forms (the decoded public key is
 * filled by bsc_decode_pub() only).
 */
typedef struct {
	uint8_t priv[BSC_PRIV_SIZE];
	uint8_t pub[BSC_PUB_SIZE];
	uint8_t prepared[BSC_PREPARED_SIZE];
	int32_t pub_decoded[(BSC_PUB_DECODED_SIZE + 3) / 4];
} bsc_key;

/*
 * One configuration: a scheme's sampler variant at one degree and thread
 * count. upstream is 1 for HAWK ref (upstream entry points, serial).
 */
typedef struct {
	unsigned scheme;
	const char *variant;
	unsigned threshold_bits;
	const char *technique;
	int upstream;
#if !BSC_HAWK_ONLY
	const e8_sampler_variant *e8v;
	const hawk_sampler_variant *hawkv;
#endif
	unsigned logn;
	unsigned threads;
	unsigned sig_bytes;
} bsc_config;

/*
 * Build information: the flag-set name and the full flags of the build
 * (bench_build_info.h, generated by the Makefile), and the compiler.
 */
extern const char *const bsc_build_flagset;
extern const char *const bsc_build_cflags;
extern const char *const bsc_build_compiler;

void bsc_rng_init(cmn_sign_rng *rng, unsigned stream_id,
	unsigned logn, unsigned index);

/*
 * The message of index i at degree logn (identical for both schemes),
 * absorbed into sc_data after hawk_sign_start().
 */
void bsc_make_message(shake_context *sc_data, unsigned logn, unsigned index);

/*
 * run_id: UTC start time plus a random 32-bit suffix.
 */
void bsc_make_run_id(char *buf, size_t len);

/*
 * Variants of a scheme, by index (bsc_variant_count()), and the
 * configuration of variant vi at logn with threads; returns 0 if the
 * configuration does not exist (upstream HAWK ref with threads != 1).
 */
size_t bsc_variant_count(unsigned scheme);

int bsc_config_init(bsc_config *c, unsigned scheme, size_t vi,
	unsigned logn, unsigned threads);

/*
 * Select c's variant and thread count for the following calls (the
 * samplers' process-global selection; this never starts or joins a
 * thread), set c->sig_bytes, and for E8 set c->threads to the resolved
 * count. bsc_deselect() restores the serial defaults. Outside timed
 * regions.
 */
int bsc_select(bsc_config *c);

void bsc_deselect(void);

/*
 * Key k of scheme at logn (seeded as above), timed into *m.
 */
int bsc_keygen(unsigned scheme, unsigned logn, unsigned key_index,
	bsc_key *key, cmn_measure *m);

/*
 * Key index of scheme at logn from signing-RNG stream stream_id, timed
 * into *m; *rng_calls and *rng_bytes receive the RNG's calls and bytes.
 * bsc_keygen() is stream BSC_STREAM_KEYGEN + scheme.
 */
int bsc_keygen_stream(unsigned scheme, unsigned logn, unsigned stream_id,
	unsigned index, bsc_key *key, cmn_measure *m,
	uint64_t *rng_calls, uint64_t *rng_bytes);

/*
 * HAWK's NTRU core Hawk_keygen() alone, as e8_keygen() calls it, from
 * stream stream_id and index, timed into *m (keygen_core rows).
 */
int bsc_keygen_core(unsigned logn, unsigned stream_id, unsigned index,
	cmn_measure *m, uint64_t *rng_calls, uint64_t *rng_bytes);

/*
 * Decode key->pub into key->pub_decoded (hawk_decode_public_key() /
 * e8_decode_public_key()), timed into *m.
 */
int bsc_decode_pub(unsigned scheme, unsigned logn, bsc_key *key,
	cmn_measure *m);

/*
 * One timed verification of sig (encoded) against sc_data with verifier
 * (BSC_VERIFIER_*) under acct (all_in: encoded public key; prepared:
 * key->pub_decoded); returns the verdict (1 = accepted). m may be NULL.
 */
int bsc_verify_timed(unsigned scheme, unsigned verifier, unsigned acct,
	unsigned logn, const bsc_key *key, const uint8_t *sig,
	const shake_context *sc_data, cmn_measure *m);

/*
 * Sizes (bytes) of every key, signature and tmp object of the schemes at
 * logn, from the size macros (bench_compare --sizes): fills up to max
 * entries of scheme name, object name and size; returns the count.
 */
typedef struct {
	const char *scheme;
	const char *object;
	unsigned long bytes;
} bsc_size;

size_t bsc_sizes(unsigned logn, bsc_size *out, size_t max);

/*
 * The prepared form of key (HAWK hawk_decode_private_key(), E8
 * e8_prepare_private_key()), timed into *m.
 */
int bsc_prepare(unsigned scheme, unsigned logn, bsc_key *key,
	cmn_measure *m);

/*
 * One timed sign_finish call of configuration c (selected) under
 * accounting acct into sig; returns 1 on success and sets *attempts and
 * *rng_bytes. trace_attempts and trace_timing are passed to the E8
 * signer (NULL unless a profile build wants the per-stage trace).
 */
int bsc_sign(const bsc_config *c, unsigned acct, const bsc_key *key,
	const shake_context *sc_data, cmn_sign_rng *rng, uint8_t *sig,
	unsigned *attempts, uint64_t *rng_bytes, cmn_measure *m,
	unsigned *trace_attempts, void *trace_timing);

/*
 * Verify sig with the scheme's verifier (outside timed regions).
 */
int bsc_verify(unsigned scheme, unsigned logn, const bsc_key *key,
	const uint8_t *sig, const shake_context *sc_data);

/*
 * Clear row and set the fields shared by every row of configuration c
 * (c may be NULL for rows of a key shared by several configurations;
 * scheme and logn are then taken from the arguments), plus the
 * measurement m (NULL: none).
 */
void bsc_row_init(cmn_sign_row *row, const char *run_id, const char *kind,
	const bsc_config *c, unsigned scheme, unsigned logn,
	unsigned key_index, const cmn_measure *m);

#endif
