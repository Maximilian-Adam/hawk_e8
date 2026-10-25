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

/* see hawk_shim.h */
int
hawk_shim_sig_gauss_tables(unsigned logn, const uint16_t **hi,
	size_t *hi_len, const uint64_t **lo, size_t *lo_len)
{
	switch (logn) {
	case 8:
		*hi = sig_gauss_hi_Hawk_256;
		*hi_len = SG_MAX_HI_Hawk_256;
		*lo = sig_gauss_lo_Hawk_256;
		*lo_len = SG_MAX_LO_Hawk_256;
		return 1;
	case 9:
		*hi = sig_gauss_hi_Hawk_512;
		*hi_len = SG_MAX_HI_Hawk_512;
		*lo = sig_gauss_lo_Hawk_512;
		*lo_len = SG_MAX_LO_Hawk_512;
		return 1;
	case 10:
		*hi = sig_gauss_hi_Hawk_1024;
		*hi_len = SG_MAX_HI_Hawk_1024;
		*lo = sig_gauss_lo_Hawk_1024;
		*lo_len = SG_MAX_LO_Hawk_1024;
		return 1;
	default:
		return 0;
	}
}
