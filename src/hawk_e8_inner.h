#ifndef HAWK_E8_INNER_H__
#define HAWK_E8_INNER_H__

#include "hawk_inner.h"

#ifndef HAWK_ENABLE_E8_EXPERIMENTAL
#define HAWK_ENABLE_E8_EXPERIMENTAL   0
#endif

#if HAWK_ENABLE_E8_EXPERIMENTAL

#ifndef HAWK_E8_DEBUG_CHECKS
#define HAWK_E8_DEBUG_CHECKS   1
#endif

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
#define e8_sym_break  Zh(e8_sym_break)
#define e8_qnorm_direct  Zh(e8_qnorm_direct)
#define e8_qnorm_completion  Zh(e8_qnorm_completion)
#define e8_sigma_verify_default  Zh(e8_sigma_verify_default)
#define e8_verify_bound_from_sigma  Zh(e8_verify_bound_from_sigma)
#define e8_verify_uncompressed  Zh(e8_verify_uncompressed)
#define e8_verify_uncompressed_with_sigma  Zh(e8_verify_uncompressed_with_sigma)
#define e8_sign_dummy_uncompressed  Zh(e8_sign_dummy_uncompressed)
#define e8_sign_dummy_offset_uncompressed  Zh(e8_sign_dummy_offset_uncompressed)
#define e8_sign_sampler_uncompressed  Zh(e8_sign_sampler_uncompressed)
#define e8_sign_sampler_trace_uncompressed  Zh(e8_sign_sampler_trace_uncompressed)
#define e8_sign_sampler_trace_timed_uncompressed  Zh(e8_sign_sampler_trace_timed_uncompressed)
#define e8_extract_tau  Zh(e8_extract_tau)
#define e8_write_block  Zh(e8_write_block)
#define e8_read_block  Zh(e8_read_block)
#define e8_block_apply_P  Zh(e8_block_apply_P)
#define e8_block_norm2  Zh(e8_block_norm2)
#define e8_block_solve_P_checked  Zh(e8_block_solve_P_checked)
#define e8_rm13_codeword  Zh(e8_rm13_codeword)
#define e8_rm13_syndrome  Zh(e8_rm13_syndrome)
#define e8_sigma_to_rho_s  Zh(e8_sigma_to_rho_s)
#define e8_sample_block_bounded_float  Zh(e8_sample_block_bounded_float)
#define e8_sample_z_bounded_float  Zh(e8_sample_z_bounded_float)
#define e8_sample_block_float  Zh(e8_sample_block_float)
#define e8_sample_z_float  Zh(e8_sample_z_float)
#define e8_sample_block_construction_a_cm  Zh(e8_sample_block_construction_a_cm)
#define e8_sample_block_construction_a_cm_trace  Zh(e8_sample_block_construction_a_cm_trace)
#define e8_sample_z_construction_a_cm  Zh(e8_sample_z_construction_a_cm)
#define e8_sampler_warm_cache  Zh(e8_sampler_warm_cache)
#define e8_sampler_cache_compare_1d_mass  Zh(e8_sampler_cache_compare_1d_mass)
#define e8_sampler_cache_compare_component_mass  Zh(e8_sampler_cache_compare_component_mass)

typedef struct {
	uint64_t blocks;
	uint64_t one_dim_samples;
	uint64_t construction_a_cosets[16];
	uint64_t norm2_sum;
	uint64_t norm2_max;
} e8_sampler_stats;

typedef struct {
	uint8_t component;
	uint8_t component_codeword;
	int32_t product_coords[8];
	int32_t xblk[8];
	int32_t zblk[8];
} e8_ca_sample_trace;

typedef struct {
	uint64_t cycles_sample_total;
	uint64_t cycles_sample_last;
	uint64_t wall_ns_sample_total;
	uint64_t wall_ns_sample_last;
} e8_sign_trace_timing;

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

/*
 * HAWK-style sign-based symmetry break for the experimental E8
 * uncompressed path.  This only chooses between w and -w: inspect w1 from
 * coefficient 0 upward, then w0 as fallback, and accept iff the first
 * non-zero coefficient is positive.  The all-zero vector is accepted.
 */
int e8_sym_break(const int32_t *w0, const int32_t *w1, unsigned logn);

int e8_qnorm_direct(int64_t *norm,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11,
	const int32_t *w0, const int32_t *w1, unsigned logn);

int e8_qnorm_completion(int64_t *norm,
	const int32_t *q00, const int32_t *q01,
	const int32_t *w0, const int32_t *w1, unsigned logn);

int e8_sigma_verify_default(unsigned logn, double *sigma_verify);

int e8_verify_bound_from_sigma(unsigned logn,
	double sigma_verify, int64_t *bound);

int e8_verify_uncompressed(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11);

int e8_verify_uncompressed_with_sigma(unsigned logn,
	const void *sig, size_t sig_len,
	const shake_context *sc_data, const void *hpub, size_t hpub_len,
	const int32_t *q00, const int32_t *q01,
	const int32_t *q10, const int32_t *q11, double sigma_verify);

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
 * coset-matched Construction-A CM sampler and is non-constant-time research
 * code.
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

int e8_sign_sampler_trace_timed_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify, int sampler_bound,
	unsigned max_attempts, hawk_rng rng, void *rng_context,
	int32_t *trace_z0, int32_t *trace_z1,
	int64_t *trace_pnorm, unsigned *trace_attempts,
	e8_sign_trace_timing *trace_timing);

/*
 * Experimental E8 sampler helpers.  These are floating-point,
 * data-dependent, non-constant-time research functions and are not
 * production signing code.
 */
uint8_t e8_extract_tau(const uint8_t *t0, const uint8_t *t1,
	size_t r, unsigned logn);

void e8_write_block(int32_t *z0, int32_t *z1,
	size_t r, const int32_t *zblk, unsigned logn);

void e8_read_block(int32_t *zblk,
	const int32_t *z0, const int32_t *z1, size_t r, unsigned logn);

void e8_block_apply_P(int32_t *xblk, const int32_t *zblk);

int64_t e8_block_norm2(const int32_t *zblk);

int e8_block_solve_P_checked(int32_t *zblk,
	const int32_t *xblk, uint8_t tau);

uint8_t e8_rm13_codeword(unsigned u);

uint8_t e8_rm13_syndrome(uint8_t p);

double e8_sigma_to_rho_s(double sigma);

/*
 * Explicit bounded/truncated baseline retained for comparison.
 */
int e8_sample_block_bounded_float(int32_t *zblk, uint8_t tau,
	double sigma, int bound, hawk_rng rng, void *rng_context);

int e8_sample_z_bounded_float(int32_t *z0, int32_t *z1, int64_t *norm2,
	const uint8_t *t0, const uint8_t *t1, unsigned logn,
	double sigma, int bound, hawk_rng rng, void *rng_context);

/*
 * Backward-compatible names for the bounded baseline.
 */
int e8_sample_block_float(int32_t *zblk, uint8_t tau,
	double sigma, int bound, hawk_rng rng, void *rng_context);

int e8_sample_z_float(int32_t *z0, int32_t *z1, int64_t *norm2,
	const uint8_t *t0, const uint8_t *t1, unsigned logn,
	double sigma, int bound, hawk_rng rng, void *rng_context);

/*
 * Coset-matched Construction-A HAWK-E8-CM sampler.  The input tau is the
 * internal P-coordinate coset label; the returned zblk always satisfies
 * zblk[i] = bit_i(tau) mod 2, and norm2 is ||P zblk||^2.
 *
 * The default sampler uses a runtime cache of one-dimensional shifted
 * Gaussian tables and tau/component selection masses.  It remains an
 * experimental, floating-point, data-dependent research sampler; the cache
 * is an optimisation only and is not constant-time hardened.
 */
int e8_sampler_warm_cache(double sigma_sign);

int e8_sample_block_construction_a_cm(int32_t *zblk, uint8_t tau,
	double sigma_sign, hawk_rng rng, void *rng_context,
	uint64_t *norm2_out, e8_sampler_stats *stats);

int e8_sample_block_construction_a_cm_trace(int32_t *zblk, uint8_t tau,
	double sigma_sign, hawk_rng rng, void *rng_context,
	uint64_t *norm2_out, e8_sampler_stats *stats,
	e8_ca_sample_trace *trace);

int e8_sample_z_construction_a_cm(int32_t *z0, int32_t *z1,
	uint64_t *pnorm_out, const uint8_t *t0, const uint8_t *t1,
	unsigned logn, double sigma_sign, hawk_rng rng, void *rng_context,
	e8_sampler_stats *stats);

/*
 * Test hooks for cache equivalence against the direct reference mass
 * computation. Centers use the exact numerator over denominator 32.
 */
int e8_sampler_cache_compare_1d_mass(double sigma_sign,
	int64_t center_num32, double *reference_mass, double *cached_mass);

int e8_sampler_cache_compare_component_mass(uint8_t tau,
	uint8_t component, double sigma_sign,
	double *reference_mass, double *cached_mass);

#endif

#endif
