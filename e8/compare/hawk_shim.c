/*
 * See hawk_shim.h.  This file is compiled with the same flags as upstream
 * hawk_sign.o, so the HAWK code under test is identical to the upstream
 * object apart from the exported wrappers below.
 */

#include "hawk_sign.c"
#include "hawk_shim.h"

/* see hawk_shim.h */
uint32_t
hawk_shim_sig_gauss(unsigned logn, hawk_rng rng, void *rng_context,
	const shake_context *sc_extra, int8_t *x, const uint8_t *t)
{
	return sig_gauss(logn, rng, rng_context, sc_extra, x, t);
}

/* see hawk_shim.h */
int
hawk_shim_sign_finish_decoded(unsigned logn, hawk_rng rng,
	void *rng_context, void *sig, const shake_context *sc_data,
	const void *priv_decoded, void *tmp, size_t tmp_len)
{
	if (logn < 8 || logn > 10) {
		return 0;
	}
	return sign_finish_inner(logn, 1, rng, rng_context, sig, sc_data,
		priv_decoded, HAWK_PRIVKEY_DECODED_SIZE(logn), tmp, tmp_len);
}
