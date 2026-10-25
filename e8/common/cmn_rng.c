#include <string.h>

#include "cmn_rng.h"

/*
 * Extract len bytes from the stream's source.
 */
static void
cmn_rng_squeeze(cmn_rng *r, void *dst, size_t len)
{
	if (r->source != NULL) {
		r->source(r->source_context, dst, len);
	} else {
		/*
		 * shake_extract() permutes before each rate-sized block of
		 * output, starting with the first byte after shake_flip()
		 * (which leaves dptr == rate).
		 */
		size_t rate = r->sc.rate;
		size_t avail = rate - r->sc.dptr;

		if (len > avail) {
			size_t rem = len - avail;

			do {
				r->permutations ++;
				rem -= rem < rate ? rem : rate;
			} while (rem > 0);
		}
		shake_extract(&r->sc, dst, len);
	}
	r->bytes_squeezed += len;
	r->refills ++;
}

/* see cmn_rng.h */
uint64_t
cmn_rng_absorb(shake_context *sc, const void *data, size_t len)
{
	/*
	 * shake_inject() permutes each time the rate is filled, counting
	 * from the bytes already absorbed (dptr); shake_flip() does not
	 * permute.
	 */
	uint64_t perms = (uint64_t)((sc->dptr + len) / sc->rate);

	if (len != 0) {
		shake_inject(sc, data, len);
	}
	return perms;
}

/*
 * Absorb domain || seed || label into r->sc (initialised or copied by
 * the caller), flip, and reset the buffer and counters.
 */
static void
cmn_rng_start(cmn_rng *r, const void *domain, size_t domain_len,
	const void *seed, size_t seed_len, const void *label, size_t label_len)
{
	r->permutations = cmn_rng_absorb(&r->sc, domain, domain_len);
	r->permutations += cmn_rng_absorb(&r->sc, seed, seed_len);
	r->permutations += cmn_rng_absorb(&r->sc, label, label_len);
	shake_flip(&r->sc);
	r->pos = 0;
	r->len = 0;
	r->source = NULL;
	r->source_context = NULL;
	r->bytes_squeezed = 0;
	r->refills = 0;
}

/* see cmn_rng.h */
void
cmn_rng_init(cmn_rng *r, const void *domain, size_t domain_len,
	const void *seed, size_t seed_len, const void *label, size_t label_len)
{
	shake_init(&r->sc, 256);
	cmn_rng_start(r, domain, domain_len, seed, seed_len,
		label, label_len);
}

/* see cmn_rng.h */
void
cmn_rng_init_prefixed(cmn_rng *r, const shake_context *prefix,
	const void *domain, size_t domain_len,
	const void *seed, size_t seed_len, const void *label, size_t label_len)
{
	r->sc = *prefix;
	cmn_rng_start(r, domain, domain_len, seed, seed_len,
		label, label_len);
}

/* see cmn_rng.h */
void
cmn_rng_init_source(cmn_rng *r, hawk_rng source, void *source_context)
{
	memset(&r->sc, 0, sizeof r->sc);
	r->pos = 0;
	r->len = 0;
	r->source = source;
	r->source_context = source_context;
	r->bytes_squeezed = 0;
	r->refills = 0;
	r->permutations = 0;
}

/* see cmn_rng.h */
void
cmn_rng_refill(cmn_rng *r)
{
	cmn_rng_squeeze(r, r->buf, sizeof r->buf);
	r->pos = 0;
	r->len = sizeof r->buf;
}

/* see cmn_rng.h */
void
cmn_rng_read_bytes(cmn_rng *r, void *dst, size_t len)
{
	uint8_t *buf = dst;

	while (len > 0) {
		size_t clen;

		if (r->pos == r->len) {
			cmn_rng_refill(r);
		}
		clen = r->len - r->pos;
		if (clen > len) {
			clen = len;
		}
		memcpy(buf, r->buf + r->pos, clen);
		r->pos += clen;
		buf += clen;
		len -= clen;
	}
}

/* see cmn_rng.h */
void
cmn_rng_read_bulk(cmn_rng *r, void *dst, size_t len)
{
	uint8_t *buf = dst;
	size_t clen = r->len - r->pos;

	if (clen > len) {
		clen = len;
	}
	memcpy(buf, r->buf + r->pos, clen);
	r->pos += clen;
	buf += clen;
	len -= clen;
	if (len > 0) {
		/*
		 * The buffer is empty here, so extracting directly keeps the
		 * delivered bytes in source order.
		 */
		cmn_rng_squeeze(r, buf, len);
	}
}

/*
 * E8's sampler reads its streams only with cmn_rng_read_u64le(). The
 * buffer length is a multiple of 8, so every refill happens with an empty
 * buffer, exactly as in the sampler's earlier private stream, which
 * discarded any remainder under 8 bytes; that rule never fired, so its
 * output is unchanged by the no-discard rule here.
 */

/* see cmn_rng.h */
void
cmn_rng_counters_get(const cmn_rng *r, cmn_rng_counters *c)
{
	c->bytes_squeezed = r->bytes_squeezed;
	c->bytes_delivered = r->bytes_squeezed - (uint64_t)(r->len - r->pos);
	c->refills = r->refills;
	c->permutations = r->permutations;
}

/* see cmn_rng.h */
void
cmn_rng_counters_add(cmn_rng_counters *dst, const cmn_rng_counters *src)
{
	dst->bytes_delivered += src->bytes_delivered;
	dst->bytes_squeezed += src->bytes_squeezed;
	dst->refills += src->refills;
	dst->permutations += src->permutations;
}

/* see cmn_rng.h */
void
cmn_sign_rng_init(cmn_sign_rng *s, const void *seed, size_t seed_len)
{
	static const char domain[] = "HAWK-E8 bench signing RNG v1";

	cmn_rng_init(&s->rng, domain, sizeof domain - 1, seed, seed_len,
		NULL, 0);
	s->calls = 0;
	s->bytes = 0;
}

/* see cmn_rng.h */
void
cmn_sign_rng_hawk(void *ctx, void *dst, size_t len)
{
	cmn_sign_rng *s = ctx;

	s->calls ++;
	s->bytes += len;
	cmn_rng_read_bytes(&s->rng, dst, len);
}
