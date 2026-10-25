#ifndef HAWK_SHIM_H__
#define HAWK_SHIM_H__

/*
 * The single bridge from E8 benchmarks and tools to ordinary HAWK internals
 * that are static in upstream hawk_sign.c.
 *
 * hawk_shim.c compiles the unmodified, generated hawk_sign.c as one
 * translation unit and exports thin wrappers around the static helpers that
 * comparisons need.  Programs that link hawk_shim.o must not also link
 * hawk_sign.o; the shim object already carries the public HAWK signing API.
 */

#include "hawk_inner.h"

/*
 * Ordinary HAWK signing sampler (sig_gauss in hawk_sign.c): writes 2*n
 * samples to x for parity bits t and returns their squared norm.
 */
uint32_t hawk_shim_sig_gauss(unsigned logn, hawk_rng rng, void *rng_context,
	const shake_context *sc_extra, int8_t *x, const uint8_t *t);

/*
 * Ordinary HAWK signing with a decoded private key and the normal SHAKE
 * sampler: sign_finish_inner(logn, 1, ..., HAWK_PRIVKEY_DECODED_SIZE(logn),
 * ...) in hawk_sign.c. priv_decoded comes from hawk_decode_private_key().
 * This is HAWK's prepared-key mode; it produces the same signature format
 * as hawk_sign_finish(), but its salt derivation absorbs f || g (2*n
 * bytes) instead of the key generation seed. (hawk_sign_finish_alt()
 * also accepts a decoded key but switches to a different sampler and salt
 * rule.) Returned value is 1 on success, 0 on error.
 */
int hawk_shim_sign_finish_decoded(unsigned logn, hawk_rng rng,
	void *rng_context, void *sig, const shake_context *sc_data,
	const void *priv_decoded, void *tmp, size_t tmp_len);

#endif
