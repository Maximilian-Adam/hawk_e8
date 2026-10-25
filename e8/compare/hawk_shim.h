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

#endif
