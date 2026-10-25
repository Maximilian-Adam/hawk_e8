#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

/*
 * Caller-supplied-salt signers with a double sigma_sign and sigma_verify
 * (tests, tools and sigma sweeps). They build the hot64 tables at runtime
 * for the given sigma_sign (e8_sampler_runtime_hot64()) and the norm
 * bound from sigma_verify (e8_verify_bound_from_sigma()), then call the
 * integer-only core e8_sign_sampler_core(). This translation unit uses
 * floating point; the public API never calls it.
 */

/* see hawk_e8_inner.h */
int
e8_sign_sampler_trace_timed_uncompressed_prepared(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G,
	const e8_inverse_w_ntt_basis *basis_ntt,
	const e8_coset_f2_basis *basis_f2, const uint8_t *salt,
	double sigma_sign, double sigma_verify,
	unsigned max_attempts, hawk_rng rng, void *rng_context,
	int32_t *trace_z0, int32_t *trace_z1,
	int64_t *trace_pnorm, unsigned *trace_attempts,
	e8_sign_trace_timing *trace_timing)
{
	const void *tables;
	int64_t norm_bound;

	if (salt == NULL || !(sigma_sign > 0.0) || !(sigma_verify > 0.0)
		|| logn < 8 || logn > 10
		|| (tables = e8_sampler_runtime_hot64(sigma_sign)) == NULL
		|| !e8_verify_bound_from_sigma(logn, sigma_verify, &norm_bound))
	{
		if (trace_timing != NULL) {
			memset(trace_timing, 0, sizeof *trace_timing);
		}
		return 0;
	}
	return e8_sign_sampler_core(logn,
		sig, sig_len, sc_data, hpub, hpub_len,
		f, g, F, G, basis_ntt, basis_f2, salt, NULL, 0,
		e8_sample_z_hot64, tables, norm_bound,
		max_attempts, rng, rng_context,
		trace_z0, trace_z1, trace_pnorm, trace_attempts,
		trace_timing);
}

/* see hawk_e8_inner.h */
int
e8_sign_sampler_trace_timed_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify,
	unsigned max_attempts, hawk_rng rng, void *rng_context,
	int32_t *trace_z0, int32_t *trace_z1,
	int64_t *trace_pnorm, unsigned *trace_attempts,
	e8_sign_trace_timing *trace_timing)
{
	e8_inverse_w_ntt_basis basis_ntt;
	e8_coset_f2_basis basis_f2;

	if (trace_timing != NULL) {
		memset(trace_timing, 0, sizeof *trace_timing);
	}
	if (logn < 8 || logn > 10
		|| f == NULL || g == NULL || F == NULL || G == NULL)
	{
		return 0;
	}
	e8_inverse_w_ntt_prepare(&basis_ntt, f, g, F, G, logn);
	e8_coset_f2_prepare(&basis_f2, f, g, F, G, (size_t)1 << logn);
	return e8_sign_sampler_trace_timed_uncompressed_prepared(logn,
		sig, sig_len, sc_data, hpub, hpub_len,
		f, g, F, G, &basis_ntt, &basis_f2, salt,
		sigma_sign, sigma_verify, max_attempts, rng, rng_context,
		trace_z0, trace_z1, trace_pnorm, trace_attempts,
		trace_timing);
}

/* see hawk_e8_inner.h */
int
e8_sign_sampler_trace_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify,
	unsigned max_attempts, hawk_rng rng, void *rng_context,
	int32_t *trace_z0, int32_t *trace_z1,
	int64_t *trace_pnorm, unsigned *trace_attempts)
{
	return e8_sign_sampler_trace_timed_uncompressed(logn,
		sig, sig_len, sc_data, hpub, hpub_len,
		f, g, F, G, salt, sigma_sign, sigma_verify,
		max_attempts, rng, rng_context, trace_z0, trace_z1,
		trace_pnorm, trace_attempts, NULL);
}

/* see hawk_e8_inner.h */
int
e8_sign_sampler_uncompressed(unsigned logn,
	void *sig, size_t sig_len, const shake_context *sc_data,
	const void *hpub, size_t hpub_len,
	const int8_t *f, const int8_t *g,
	const int8_t *F, const int8_t *G, const uint8_t *salt,
	double sigma_sign, double sigma_verify,
	unsigned max_attempts, hawk_rng rng, void *rng_context)
{
	return e8_sign_sampler_trace_uncompressed(logn,
		sig, sig_len, sc_data, hpub, hpub_len, f, g, F, G, salt,
		sigma_sign, sigma_verify, max_attempts,
		rng, rng_context, NULL, NULL, NULL, NULL);
}

#endif
