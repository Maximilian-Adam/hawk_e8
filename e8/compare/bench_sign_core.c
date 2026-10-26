#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bench_sign_core.h"
#include "bench_build_info.h"
#include "hawk_shim.h"

/* see bench_sign_core.h */
const char *const bsc_scheme_name[BSC_SCHEME_COUNT] = { "hawk", "e8" };
const char *const bsc_acct_name[BSC_ACCT_COUNT] = { "all_in", "prepared" };

const char *const bsc_build_flagset = BENCH_FLAGSET;
const char *const bsc_build_cflags = BENCH_CFLAGS;
#if defined(__VERSION__)
const char *const bsc_build_compiler = __VERSION__;
#else
const char *const bsc_build_compiler = "unknown";
#endif

/*
 * Scratch buffers, sized for the largest degree of either scheme.
 */
#if BSC_HAWK_ONLY
static uint8_t tmp_keygen[HAWK_TMPSIZE_KEYGEN(10)];
static uint8_t tmp_sign[HAWK_TMPSIZE_SIGN(10)];
static uint8_t tmp_verify[HAWK_TMPSIZE_VERIFY(10)];
#else
static uint8_t tmp_keygen[E8_TMPSIZE_KEYGEN(10)];
static uint8_t tmp_sign[E8_TMPSIZE_SIGN(10)];
static uint8_t tmp_verify[E8_TMPSIZE_VERIFY(10) + HAWK_TMPSIZE_VERIFY(10)];

typedef char bsc_hawk_buffers_check[
	(BSC_PRIV_SIZE >= HAWK_PRIVKEY_SIZE(10)
	&& BSC_PUB_SIZE >= HAWK_PUBKEY_SIZE(10)
	&& BSC_PREPARED_SIZE >= HAWK_PRIVKEY_DECODED_SIZE(10)
	&& sizeof tmp_keygen >= HAWK_TMPSIZE_KEYGEN(10)
	&& sizeof tmp_sign >= HAWK_TMPSIZE_SIGN(10)
	&& sizeof tmp_verify >= HAWK_TMPSIZE_VERIFY(10)
	&& BSC_SIG_SIZE >= HAWK_SIG_SIZE(10)) ? 1 : -1];
#endif

/* see bench_sign_core.h */
void
bsc_rng_init(cmn_sign_rng *rng, unsigned stream_id,
	unsigned logn, unsigned index)
{
	uint8_t seed[12];
	uint32_t v[3];

	v[0] = stream_id;
	v[1] = logn;
	v[2] = index;
	for (unsigned u = 0; u < 3; u ++) {
		for (unsigned w = 0; w < 4; w ++) {
			seed[4 * u + w] = (uint8_t)(v[u] >> (w << 3));
		}
	}
	cmn_sign_rng_init(rng, seed, sizeof seed);
}

/* see bench_sign_core.h */
void
bsc_make_message(shake_context *sc_data, unsigned logn, unsigned index)
{
	static const char prefix[] = "hawk e8 sign bench message";
	uint8_t buf[5];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)index;
	buf[2] = (uint8_t)(index >> 8);
	buf[3] = (uint8_t)(index >> 16);
	buf[4] = (uint8_t)(index >> 24);
	hawk_sign_start(sc_data);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

/* see bench_sign_core.h */
void
bsc_make_run_id(char *buf, size_t len)
{
	time_t now = time(NULL);
	struct tm tm;
	char stamp[24];
	uint32_t suffix = 0;
	FILE *fp = fopen("/dev/urandom", "rb");

	if (fp == NULL || fread(&suffix, sizeof suffix, 1, fp) != 1) {
		suffix = (uint32_t)(cmn_wall_ns() ^ ((uint64_t)getpid() << 16));
	}
	if (fp != NULL) {
		fclose(fp);
	}
	if (gmtime_r(&now, &tm) == NULL
		|| strftime(stamp, sizeof stamp, "%Y%m%dT%H%M%SZ", &tm) == 0)
	{
		snprintf(stamp, sizeof stamp, "%llu",
			(unsigned long long)now);
	}
	snprintf(buf, len, "%s-%08x", stamp, (unsigned)suffix);
}

/* see bench_sign_core.h */
size_t
bsc_variant_count(unsigned scheme)
{
#if BSC_HAWK_ONLY
	return scheme == BSC_SCHEME_HAWK ? 1 : 0;
#else
	return scheme == BSC_SCHEME_HAWK ? hawk_sampler_variant_count
		: e8_sampler_variant_count;
#endif
}

/* see bench_sign_core.h */
int
bsc_config_init(bsc_config *c, unsigned scheme, size_t vi,
	unsigned logn, unsigned threads)
{
	memset(c, 0, sizeof *c);
	if (vi >= bsc_variant_count(scheme) || logn < 8 || logn > 10
		|| threads == 0)
	{
		return 0;
	}
	c->scheme = scheme;
	c->logn = logn;
	c->threads = threads;
#if BSC_HAWK_ONLY
	c->variant = "ref-avx2";
	c->threshold_bits = 78;
	c->technique = "ct_full_scan";
	c->upstream = 1;
#else
	if (scheme == BSC_SCHEME_HAWK) {
		c->hawkv = &hawk_sampler_variants[vi];
		c->variant = c->hawkv->name;
		c->threshold_bits = c->hawkv->threshold_bits;
		c->technique = c->hawkv->technique;
		c->upstream = c->hawkv->range == NULL;
	} else {
		c->e8v = &e8_sampler_variants[vi];
		c->variant = c->e8v->name;
		c->threshold_bits = c->e8v->threshold_bits;
		c->technique = c->e8v->technique;
	}
#endif
	return !(c->upstream && threads != 1);
}

/* see bench_sign_core.h */
int
bsc_select(bsc_config *c)
{
#if BSC_HAWK_ONLY
	c->sig_bytes = HAWK_SIG_SIZE(c->logn);
	return c->threads == 1;
#else
	if (c->scheme == BSC_SCHEME_E8) {
		e8_sampler_set_thread_count(c->threads);
		e8_sampler_set_rng_mode(E8_SAMPLER_RNG_PER_WORKER);
		c->threads = e8_sampler_get_thread_count(c->logn);
		c->sig_bytes = E8_SIG_SIZE(c->logn);
		return 1;
	}
	c->sig_bytes = HAWK_SIG_SIZE(c->logn);
	return hawkv_select(c->hawkv, c->threads);
#endif
}

/* see bench_sign_core.h */
void
bsc_deselect(void)
{
#if !BSC_HAWK_ONLY
	e8_sampler_set_thread_count(1);
	(void)hawkv_select(&hawk_sampler_variants[0], 1);
#endif
}

/* see bench_sign_core.h */
int
bsc_keygen(unsigned scheme, unsigned logn, unsigned key_index,
	bsc_key *key, cmn_measure *m)
{
	cmn_sign_rng rng;
	cmn_pool_stamp start;
	int ok;

	bsc_rng_init(&rng, BSC_STREAM_KEYGEN + scheme, logn, key_index);
	cmn_pool_measure_start(&start);
	if (scheme == BSC_SCHEME_HAWK) {
		ok = hawk_keygen(logn, key->priv, key->pub,
			cmn_sign_rng_hawk, &rng,
			tmp_keygen, sizeof tmp_keygen);
	} else {
#if BSC_HAWK_ONLY
		ok = 0;
#else
		ok = e8_keygen(logn, key->priv, key->pub,
			cmn_sign_rng_hawk, &rng,
			tmp_keygen, sizeof tmp_keygen);
#endif
	}
	cmn_pool_measure_stop(m, &start);
	return ok;
}

/* see bench_sign_core.h */
int
bsc_prepare(unsigned scheme, unsigned logn, bsc_key *key, cmn_measure *m)
{
	cmn_pool_stamp start;
	int ok;

	cmn_pool_measure_start(&start);
	if (scheme == BSC_SCHEME_HAWK) {
		hawk_decode_private_key(logn, key->prepared, key->priv);
		ok = 1;
	} else {
#if BSC_HAWK_ONLY
		ok = 0;
#else
		ok = e8_prepare_private_key(logn, key->prepared, key->priv);
#endif
	}
	cmn_pool_measure_stop(m, &start);
	return ok;
}

/* see bench_sign_core.h */
int
bsc_sign(const bsc_config *c, unsigned acct, const bsc_key *key,
	const shake_context *sc_data, cmn_sign_rng *rng, uint8_t *sig,
	unsigned *attempts, uint64_t *rng_bytes, cmn_measure *m,
	unsigned *trace_attempts, void *trace_timing)
{
	unsigned logn = c->logn;
	cmn_pool_stamp start;
	int ok = 0;
#if !BSC_HAWK_ONLY
	cmn_rng_counters sampler0, sampler1;

	if (c->scheme == BSC_SCHEME_HAWK) {
		hawkv_rng_counters_get(&sampler0);
	} else {
		e8_sampler_rng_counters_get(&sampler0);
	}
#else
	(void)trace_attempts;
	(void)trace_timing;
#endif
	cmn_pool_measure_start(&start);
	if (c->upstream) {
		if (acct == BSC_ACCT_ALL_IN) {
			ok = hawk_sign_finish(logn, cmn_sign_rng_hawk, rng,
				sig, sc_data, key->priv,
				tmp_sign, sizeof tmp_sign);
		} else {
			ok = hawk_shim_sign_finish_decoded(logn,
				cmn_sign_rng_hawk, rng,
				sig, sc_data, key->prepared,
				tmp_sign, sizeof tmp_sign);
		}
#if !BSC_HAWK_ONLY
	} else if (c->scheme == BSC_SCHEME_HAWK) {
		if (acct == BSC_ACCT_ALL_IN) {
			ok = hawkv_sign_finish(logn, cmn_sign_rng_hawk, rng,
				sig, sc_data, key->priv,
				tmp_sign, sizeof tmp_sign);
		} else {
			ok = hawkv_sign_finish_decoded(logn,
				cmn_sign_rng_hawk, rng,
				sig, sc_data, key->prepared,
				tmp_sign, sizeof tmp_sign);
		}
	} else {
		if (acct == BSC_ACCT_ALL_IN) {
			ok = e8_sign_finish_inner(logn, cmn_sign_rng_hawk, rng,
				sig, sc_data, key->priv,
				E8_PRIVKEY_SIZE(logn), tmp_sign, sizeof tmp_sign,
				c->e8v, trace_attempts, trace_timing);
		} else {
			ok = e8_sign_finish_inner(logn, cmn_sign_rng_hawk, rng,
				sig, sc_data, key->prepared,
				E8_PRIVKEY_PREPARED_SIZE(logn), NULL, 0,
				c->e8v, trace_attempts, trace_timing);
		}
#endif
	}
	cmn_pool_measure_stop(m, &start);
	*attempts = (unsigned)(rng->calls / 2);
	if (c->upstream) {
		*rng_bytes = rng->bytes
			+ (uint64_t)*attempts * (20u << logn);
	} else {
#if !BSC_HAWK_ONLY
		if (c->scheme == BSC_SCHEME_HAWK) {
			hawkv_rng_counters_get(&sampler1);
		} else {
			e8_sampler_rng_counters_get(&sampler1);
		}
		*rng_bytes = rng->bytes + (sampler1.bytes_delivered
			- sampler0.bytes_delivered);
#endif
	}
	return ok;
}

/* see bench_sign_core.h */
int
bsc_verify(unsigned scheme, unsigned logn, const bsc_key *key,
	const uint8_t *sig, const shake_context *sc_data)
{
	if (scheme == BSC_SCHEME_HAWK) {
		return hawk_verify_finish(logn, sig, HAWK_SIG_SIZE(logn),
			sc_data, key->pub, HAWK_PUBKEY_SIZE(logn),
			tmp_verify, sizeof tmp_verify);
	}
#if BSC_HAWK_ONLY
	return 0;
#else
	return e8_verify_finish(logn, sig, E8_SIG_SIZE(logn),
		sc_data, key->pub, E8_PUBKEY_SIZE(logn),
		tmp_verify, sizeof tmp_verify);
#endif
}

/* see bench_sign_core.h */
void
bsc_row_init(cmn_sign_row *row, const char *run_id, const char *kind,
	const bsc_config *c, unsigned scheme, unsigned logn,
	unsigned key_index, const cmn_measure *m)
{
	cmn_sign_row_clear(row);
	row->kind = kind;
	row->run_id = run_id;
	if (c != NULL) {
		row->scheme = bsc_scheme_name[c->scheme];
		row->variant = c->variant;
		row->threshold_bits = c->threshold_bits;
		row->technique = c->technique;
		row->threads = c->threads;
		row->logn = c->logn;
	} else {
		row->scheme = bsc_scheme_name[scheme];
		row->logn = logn;
	}
	row->key_index = key_index;
	row->flags = bsc_build_flagset;
	if (m != NULL) {
		row->tsc_cycles = m->tsc_cycles;
		row->core_cycles = m->core_cycles;
		row->wall_ns = m->wall_ns;
		row->cpu_ns_all_threads = m->cpu_ns_all_threads;
	}
}
