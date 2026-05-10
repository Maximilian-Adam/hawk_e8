#ifndef HAWK_E8_INNER_H__
#define HAWK_E8_INNER_H__

#include "hawk_inner.h"

#ifndef HAWK_ENABLE_E8_EXPERIMENTAL
#define HAWK_ENABLE_E8_EXPERIMENTAL   0
#endif

#if HAWK_ENABLE_E8_EXPERIMENTAL

/*
 * Experimental E8 helper API.
 *
 * These functions operate on signed coefficient arrays in
 * R_n = Z[X]/(X^n + 1), for logn in { 8, 9, 10 }. Inputs and outputs must
 * not overlap. They are reference helpers only; no constant-time claim is
 * made for this experimental path.
 */
#define e8_apply_P     Zh(e8_apply_P)
#define e8_apply_S     Zh(e8_apply_S)
#define e8_make_delta  Zh(e8_make_delta)
#define e8_compute_qform  Zh(e8_compute_qform)
#define e8_salt_len  Zh(e8_salt_len)
#define e8_sig_uncompressed_size  Zh(e8_sig_uncompressed_size)
#define e8_encode_sig_uncompressed  Zh(e8_encode_sig_uncompressed)
#define e8_decode_sig_uncompressed  Zh(e8_decode_sig_uncompressed)
#define e8_qnorm_direct  Zh(e8_qnorm_direct)
#define e8_verify_uncompressed  Zh(e8_verify_uncompressed)
#define e8_sign_dummy_uncompressed  Zh(e8_sign_dummy_uncompressed)
#define e8_sign_dummy_offset_uncompressed  Zh(e8_sign_dummy_offset_uncompressed)
#define e8_sign_sampler_uncompressed  Zh(e8_sign_sampler_uncompressed)
#define e8_sign_sampler_trace_uncompressed  Zh(e8_sign_sampler_trace_uncompressed)
#define e8_extract_tau  Zh(e8_extract_tau)
#define e8_write_block  Zh(e8_write_block)
#define e8_read_block  Zh(e8_read_block)
#define e8_block_apply_P  Zh(e8_block_apply_P)
#define e8_block_norm2  Zh(e8_block_norm2)
#define e8_sample_block_float  Zh(e8_sample_block_float)
#define e8_sample_z_float  Zh(e8_sample_z_float)

void e8_apply_P(int32_t *out0, int32_t *out1,
	const int32_t *z0, const int32_t *z1, unsigned logn);

void e8_apply_S(int32_t *out0, int32_t *out1,
	const int32_t *z0, const int32_t *z1, unsigned logn);

void e8_make_delta(int32_t *delta, unsigned logn);

void e8_compute_qform(int32_t *q00, int32_t *q01,
	int32_t *q10, int32_t *q11, const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, unsigned logn);

size_t e8_salt_len(unsigned logn);

size_t e8_sig_uncompressed_size(unsigned logn);

int e8_encode_sig_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const uint8_t *salt,
	const int16_t *s0, const int16_t *s1);

int e8_decode_sig_uncompressed(unsigned logn,
	uint8_t *salt, int16_t *s0, int16_t *s1,
	const void *sig, size_t sig_len);

int e8_qnorm_direct(int64_t *norm,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const int32_t *w0, const int32_t *w1, unsigned logn);

int e8_verify_uncompressed(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11);

/*
 * Experimental dummy signing helpers.  These only exercise algebra and
 * encoding; they do not sample from the target E8 distribution.
 */
int e8_sign_dummy_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt);

int e8_sign_dummy_offset_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	const int32_t *r0, const int32_t *r1);

/*
 * Experimental sampler-backed uncompressed signing.  This uses the
 * floating-point bounded E8 sampler and is non-constant-time research code.
 */
int e8_sign_sampler_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify, int sampler_bound,
	unsigned max_attempts, hawk_rng rng, void *rng_context);

int e8_sign_sampler_trace_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify, int sampler_bound,
	unsigned max_attempts, hawk_rng rng, void *rng_context,
	int32_t *trace_z0, int32_t *trace_z1,
	int64_t *trace_pnorm, unsigned *trace_attempts);

/*
 * Experimental floating-point bounded E8 sampler helpers.  These are
 * truncated, non-constant-time research functions and are not production
 * signing code.
 */
uint8_t e8_extract_tau(const uint8_t *t0, const uint8_t *t1,
	size_t r, unsigned logn);

void e8_write_block(int32_t *z0, int32_t *z1,
	size_t r, const int32_t *zblk, unsigned logn);

void e8_read_block(int32_t *zblk,
	const int32_t *z0, const int32_t *z1, size_t r, unsigned logn);

void e8_block_apply_P(int32_t *xblk, const int32_t *zblk);

int64_t e8_block_norm2(const int32_t *zblk);

int e8_sample_block_float(int32_t *zblk, uint8_t tau,
	double sigma, int bound, hawk_rng rng, void *rng_context);

int e8_sample_z_float(int32_t *z0, int32_t *z1, int64_t *norm2,
	const uint8_t *t0, const uint8_t *t1, unsigned logn,
	double sigma, int bound, hawk_rng rng, void *rng_context);

#endif

#endif
