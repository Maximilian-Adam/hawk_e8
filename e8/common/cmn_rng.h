#ifndef CMN_RNG_H__
#define CMN_RNG_H__

#include <stddef.h>
#include <stdint.h>

#include "hawk.h"

/*
 * Shared randomness module for the HAWK and HAWK-E8-CM measurements.
 *
 * A stream is SHAKE256 initialised by absorbing a domain string, then the
 * seed bytes, then the label bytes, in that order, then flipped; its
 * output is read through a fixed CMN_RNG_BUF_BYTES-byte buffer. A stream
 * may start from an already-absorbed prefix (cmn_rng_init_prefixed()): the
 * prefix state is copied, then domain, seed and label are absorbed after
 * it, so one prefix serves any number of streams. A stream
 * may instead be sourced from a hawk_rng callback (cmn_rng_init_source());
 * buffering and counters are then the same, with the callback in place of
 * SHAKE256.
 *
 * Delivered bytes are exactly the source output, in order, whatever the
 * mix of read sizes: no byte is ever discarded. A read that finds fewer
 * bytes buffered than it needs first takes the buffered bytes, then
 * refills. cmn_rng_read_bulk() takes the buffered bytes, then extracts the
 * rest directly from the source, so that large requests are not copied
 * through the buffer; the sequence is the same.
 *
 * Counters (cmn_rng_counters_get()):
 *
 *   bytes_delivered   bytes returned to readers
 *   bytes_squeezed    bytes extracted from the source (SHAKE256 output or
 *                     callback requests); at most CMN_RNG_BUF_BYTES - 1
 *                     more than bytes_delivered
 *   refills           source extractions (buffer refills plus direct bulk
 *                     extractions)
 *   permutations      Keccak-f[1600] permutations, absorbing and squeezing
 *                     (0 for a callback source); for a prefixed stream,
 *                     only those after the prefix (the prefix's own are
 *                     counted once by whoever absorbed it, see
 *                     cmn_rng_absorb())
 *
 * Counters of several streams are summed with cmn_rng_counters_add().
 */

#define CMN_RNG_BUF_BYTES   128

typedef struct {
	uint64_t bytes_delivered;
	uint64_t bytes_squeezed;
	uint64_t refills;
	uint64_t permutations;
} cmn_rng_counters;

typedef struct {
	uint8_t buf[CMN_RNG_BUF_BYTES];
	size_t pos;
	size_t len;
	shake_context sc;
	hawk_rng source;
	void *source_context;
	uint64_t bytes_squeezed;
	uint64_t refills;
	uint64_t permutations;
} cmn_rng;

/*
 * Initialise a SHAKE256 stream from domain || seed || label. Any of the
 * three may be empty (NULL with length 0).
 */
void cmn_rng_init(cmn_rng *r, const void *domain, size_t domain_len,
	const void *seed, size_t seed_len, const void *label, size_t label_len);

/*
 * Initialise a SHAKE256 stream from prefix || domain || seed || label,
 * where prefix is an absorbing (not flipped) SHAKE256 context, for
 * example one built with cmn_rng_absorb(). The prefix is copied and not
 * modified. The stream's permutation counter counts the permutations
 * from the first byte after the prefix, so a prefix shared by several
 * streams is counted once, by its builder.
 */
void cmn_rng_init_prefixed(cmn_rng *r, const shake_context *prefix,
	const void *domain, size_t domain_len,
	const void *seed, size_t seed_len, const void *label, size_t label_len);

/*
 * Absorb len bytes into an absorbing SHAKE context (shake_inject()) and
 * return the number of Keccak-f[1600] permutations this triggered.
 */
uint64_t cmn_rng_absorb(shake_context *sc, const void *data, size_t len);

/*
 * Initialise a stream whose bytes come from a hawk_rng callback.
 */
void cmn_rng_init_source(cmn_rng *r, hawk_rng source, void *source_context);

/*
 * Refill the buffer from the source (the slow path of the reads below).
 * The buffer must be empty.
 */
void cmn_rng_refill(cmn_rng *r);

/*
 * Read len bytes through the buffer.
 */
void cmn_rng_read_bytes(cmn_rng *r, void *dst, size_t len);

/*
 * Read len bytes, extracting all but the buffered bytes directly from the
 * source. Same output as cmn_rng_read_bytes().
 */
void cmn_rng_read_bulk(cmn_rng *r, void *dst, size_t len);

/*
 * Read the next 8 bytes as a little-endian 64-bit integer. The fast path is
 * inline; it is the read of the E8 sampler's inner loop.
 */
static inline uint64_t
cmn_rng_read_u64le(cmn_rng *r)
{
	uint64_t x = 0;

	if (r->len - r->pos >= 8) {
		for (unsigned u = 0; u < 8; u ++) {
			x |= (uint64_t)r->buf[r->pos ++] << (u << 3);
		}
		return x;
	} else {
		uint8_t tmp[8];

		cmn_rng_read_bytes(r, tmp, sizeof tmp);
		for (unsigned u = 0; u < 8; u ++) {
			x |= (uint64_t)tmp[u] << (u << 3);
		}
		return x;
	}
}

/*
 * Read the next 4 bytes as a little-endian 32-bit integer (the read of
 * the 32-bit hot-table samplers). Reads of different widths may be mixed:
 * the stream never discards bytes.
 */
static inline uint32_t
cmn_rng_read_u32le(cmn_rng *r)
{
	uint32_t x = 0;

	if (r->len - r->pos >= 4) {
		for (unsigned u = 0; u < 4; u ++) {
			x |= (uint32_t)r->buf[r->pos ++] << (u << 3);
		}
		return x;
	} else {
		uint8_t tmp[4];

		cmn_rng_read_bytes(r, tmp, sizeof tmp);
		for (unsigned u = 0; u < 4; u ++) {
			x |= (uint32_t)tmp[u] << (u << 3);
		}
		return x;
	}
}

void cmn_rng_counters_get(const cmn_rng *r, cmn_rng_counters *c);

void cmn_rng_counters_add(cmn_rng_counters *dst, const cmn_rng_counters *src);

/*
 * Top-level signing RNG for the benchmarks, with the hawk_rng signature
 * (cmn_sign_rng_hawk(), context: a cmn_sign_rng). It is a SHAKE256 stream
 * with the domain "HAWK-E8 bench signing RNG v1", seeded per trial, read
 * with cmn_rng_read_bytes(); it counts calls and requested bytes. Both
 * schemes receive it through their rng callback, applied identically.
 */
typedef struct {
	cmn_rng rng;
	uint64_t calls;
	uint64_t bytes;
} cmn_sign_rng;

void cmn_sign_rng_init(cmn_sign_rng *s, const void *seed, size_t seed_len);

void cmn_sign_rng_hawk(void *ctx, void *dst, size_t len);

#endif
