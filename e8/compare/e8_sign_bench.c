#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../src/hawk_e8_inner.h"
#include "cmn_csv.h"
#include "cmn_pool.h"
#include "cmn_rng.h"
#include "cmn_timing.h"
#include "hawk_shim.h"
#include "hawk_variants.h"

/*
 * Full signature benchmark: ordinary HAWK against HAWK-E8-CM, both timed
 * through their own sign_finish call, one call per signature, under two
 * accounting modes applied identically to both schemes:
 *
 *   all_in    the encoded private key is passed to sign_finish, which
 *             expands it on every call:
 *               HAWK  hawk_sign_finish()
 *               E8    e8_sign_finish()
 *   prepared  the private key is expanded once per key, outside the timed
 *             call; the expansion is timed and reported separately:
 *               HAWK  hawk_decode_private_key(), then
 *                     hawk_shim_sign_finish_decoded()
 *               E8    e8_prepare_private_key(), then
 *                     e8_sign_finish_prepared()
 *
 * Both timed calls include message/public-key hashing, per-attempt salt
 * derivation, challenge hashing, sampling, rejection, reconstruction and
 * encoding. Nothing measured separately is added to or subtracted from a
 * timed call. Keys come from hawk_keygen() / e8_keygen(), both schemes sign
 * the same messages, and every signature is verified with
 * hawk_verify_finish() / e8_verify_finish() outside the timed region.
 *
 * The signing RNG is the shared module's SHAKE256 signing RNG
 * (cmn_sign_rng, cmn_rng.h), seeded per trial and given to both schemes
 * through their rng callback; for the same trial both schemes receive the
 * same stream. Attempts are counted identically for both schemes from it:
 * each attempt calls it exactly twice (salt, then sampler seed), so
 * attempts = rng_calls / 2.
 *
 * Sampler variants (Step 4): every block is one (scheme, variant, thread
 * count, degree). HAWK variants: ref (upstream hawk_sign_finish() and the
 * decoded-key shim, unmodified), and ref-T, hot64, hot32 through the
 * variant signer (compare/hawk_variants/: upstream sign_finish_inner()
 * with the sampler hook). E8 variants: ref, hot64, hot32 through
 * e8_sign_finish_inner() with the variant (the public API's code path;
 * hot64 is its default). threshold_bits and technique come from the
 * variant registries. Upstream HAWK ref is serial; every other variant
 * runs with T workers on the shared pool, T = 1 being its serial form.
 * --variant and --threads select the blocks.
 *
 * rng_bytes is the number of random bytes consumed by the signature: the
 * bytes requested from the signing RNG (salt_len + 40 per attempt for every
 * variant of both schemes) plus the bytes the sampler streams deliver to
 * the sampler. For every variant except upstream HAWK ref the latter is
 * read from the sampler's stream counters (e8_sampler_rng_counters_get(),
 * hawkv_rng_counters_get()); for upstream HAWK ref, whose sampler stays
 * unmodified, it is the exact count 20n bytes per attempt (four SHAKE256
 * instances, 40 bytes per four coordinates, 2n coordinates).
 *
 * Every timed call is one cmn_pool_measure_start() / _stop() pair
 * (cmn_pool.h): TSC cycles, core cycles (perf_event_open; empty if
 * unavailable), wall-clock time and CPU time over all threads (the calling
 * thread plus the pool workers' jobs, with pool accounting on). Worker
 * spinning between dispatches is excluded from each row and reported per
 * block in the summary (pool idle CPU).
 *
 * By default every variant of both schemes runs with one thread and with
 * the selected per-logn thread count (8, 8, 16); the thread count is
 * reported on every row. Before each block of signatures, one untimed
 * signature per accounting mode warms caches (and the sampler tables and
 * worker pool) for both schemes alike. A threaded block sizes the pool with
 * cmn_pool_ensure() before its warm-up and joins the pool threads after
 * the block, so that no timed call starts or joins a thread and no block
 * runs beside spinning workers.
 *
 * stdout: one CSV row per signature, plus one row per key generation and
 * per key preparation (kind = keygen / prepare), in the shared schema
 * (cmn_csv.h). stderr: one summary table per accounting mode, then the
 * separately timed setup costs and the pool activity per block.
 */

#define DEFAULT_SIGN_BENCH_TRIALS    16
#define DEFAULT_SIGS_PER_KEY         1
#define MAX_BENCH_TRIALS             1000000

#define SCHEME_HAWK    0
#define SCHEME_E8      1

#define ACCT_ALL_IN     0
#define ACCT_PREPARED   1
#define ACCT_COUNT      2

static const char *const SCHEME_NAME[] = { "hawk", "e8" };
static const char *const ACCT_NAME[] = { "all_in", "prepared" };

#define MAX_THREAD_CASES   8
#define MAX_VARIANT_NAMES  8

/*
 * Thread counts: 0 stands for the selected per-logn count (8, 8, 16).
 * Variant names select variants of either scheme by name; none selects
 * every variant.
 */
typedef struct {
	unsigned trials;
	unsigned sigs_per_key;
	unsigned logn_min;
	unsigned logn_max;
	int no_header;
	int pool_accounting;
	unsigned thread_cases[MAX_THREAD_CASES];
	unsigned thread_case_count;
	const char *variant_names[MAX_VARIANT_NAMES];
	unsigned variant_name_count;
} sign_bench_options;

/*
 * One benchmark block: a scheme's sampler variant at one degree and
 * thread count.
 */
typedef struct {
	unsigned scheme;
	const char *variant;
	unsigned threshold_bits;
	const char *technique;
	const e8_sampler_variant *e8v;
	const hawk_sampler_variant *hawkv;
	unsigned logn;
	unsigned threads;
	unsigned sig_bytes;
	uint64_t *cycles[ACCT_COUNT];
	uint64_t *core_cycles[ACCT_COUNT];
	uint64_t *wall_ns[ACCT_COUNT];
	uint64_t *cpu_ns[ACCT_COUNT];
	uint64_t signatures[ACCT_COUNT];
	uint64_t attempts[ACCT_COUNT];
	uint64_t rng_bytes[ACCT_COUNT];
	uint64_t sign_failures[ACCT_COUNT];
	uint64_t verify_failures[ACCT_COUNT];
	uint64_t *keygen_cycles;
	uint64_t *prepare_cycles;
	uint64_t keys;
	cmn_pool_stats pool;
} bench_block;

#define MAX_BLOCKS   256

static bench_block blocks[MAX_BLOCKS];
static unsigned block_count;

static char run_id[48];
static cmn_csv csv;

#if HAWK_E8_PROFILE_SIGN
typedef struct {
	uint64_t trials;
	uint64_t accepted;
	uint64_t failed;
	uint64_t attempts;
	uint64_t rejections;
	uint64_t attempt_mismatches;
	uint64_t cycles_total;
	uint64_t cycles_prepare;
	uint64_t cycles_salt;
	uint64_t cycles_hash;
	uint64_t cycles_target;
	uint64_t cycles_sample;
	uint64_t cycles_reconstruct;
	uint64_t cycles_norm_check;
	uint64_t cycles_encode;
	uint64_t wall_ns_total;
	uint64_t wall_ns_prepare;
	uint64_t wall_ns_salt;
	uint64_t wall_ns_hash;
	uint64_t wall_ns_target;
	uint64_t wall_ns_sample;
	uint64_t wall_ns_reconstruct;
	uint64_t wall_ns_norm_check;
	uint64_t wall_ns_encode;
} bench_sign_profile_totals;

static bench_sign_profile_totals sign_profile_totals[ACCT_COUNT][11];
#endif

static unsigned
bench_selected_e8_threads(unsigned logn)
{
	switch (logn) {
	case 8: return 8;
	case 9: return 8;
	default: return 16;
	}
}

/*
 * Signing RNG for one purpose (stream_id), degree and index: the shared
 * module's signing RNG seeded with
 * u32le(stream_id) || u32le(logn) || u32le(index).
 */
static void
bench_rng_init(cmn_sign_rng *rng, unsigned stream_id,
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

/*
 * run_id: UTC start time plus a random 32-bit suffix.
 */
static void
make_run_id(void)
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
	snprintf(run_id, sizeof run_id, "%s-%08x", stamp, (unsigned)suffix);
}

/*
 * The message for a trial; identical for both schemes.
 */
static void
bench_make_message(shake_context *sc_data, unsigned logn, unsigned trial)
{
	static const char prefix[] = "hawk e8 sign bench message";
	uint8_t buf[5];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)trial;
	buf[2] = (uint8_t)(trial >> 8);
	buf[3] = (uint8_t)(trial >> 16);
	buf[4] = (uint8_t)(trial >> 24);
	hawk_sign_start(sc_data);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

/*
 * Key material for one scheme, in both forms.
 */
static uint8_t key_priv[E8_PRIVKEY_SIZE(10)];
static uint8_t key_pub[E8_PUBKEY_SIZE(10)];
static uint8_t key_prepared[E8_PRIVKEY_PREPARED_SIZE(10)];
static uint8_t tmp_keygen[E8_TMPSIZE_KEYGEN(10)];
static uint8_t tmp_sign[E8_TMPSIZE_SIGN(10)];
static uint8_t tmp_verify[E8_TMPSIZE_VERIFY(10) + HAWK_TMPSIZE_VERIFY(10)];
static uint8_t sig_buf[E8_SIG_SIZE(10)];

typedef char bench_hawk_buffers_check[
	(sizeof key_priv >= HAWK_PRIVKEY_SIZE(10)
	&& sizeof key_pub >= HAWK_PUBKEY_SIZE(10)
	&& sizeof key_prepared >= HAWK_PRIVKEY_DECODED_SIZE(10)
	&& sizeof tmp_keygen >= HAWK_TMPSIZE_KEYGEN(10)
	&& sizeof tmp_sign >= HAWK_TMPSIZE_SIGN(10)
	&& sizeof tmp_verify >= HAWK_TMPSIZE_VERIFY(10)
	&& sizeof sig_buf >= HAWK_SIG_SIZE(10)) ? 1 : -1];

static int
bench_keygen(unsigned scheme, unsigned logn, unsigned key_index,
	cmn_measure *m)
{
	cmn_sign_rng rng;
	cmn_pool_stamp start;
	int ok;

	bench_rng_init(&rng, 1 + scheme, logn, key_index);
	cmn_pool_measure_start(&start);
	if (scheme == SCHEME_HAWK) {
		ok = hawk_keygen(logn, key_priv, key_pub,
			cmn_sign_rng_hawk, &rng,
			tmp_keygen, sizeof tmp_keygen);
	} else {
		ok = e8_keygen(logn, key_priv, key_pub,
			cmn_sign_rng_hawk, &rng,
			tmp_keygen, sizeof tmp_keygen);
	}
	cmn_pool_measure_stop(m, &start);
	return ok;
}

static int
bench_prepare(unsigned scheme, unsigned logn, cmn_measure *m)
{
	cmn_pool_stamp start;
	int ok;

	cmn_pool_measure_start(&start);
	if (scheme == SCHEME_HAWK) {
		hawk_decode_private_key(logn, key_prepared, key_priv);
		ok = 1;
	} else {
		ok = e8_prepare_private_key(logn, key_prepared, key_priv);
	}
	cmn_pool_measure_stop(m, &start);
	return ok;
}

#if HAWK_E8_PROFILE_SIGN
static void profile_add(unsigned acct, unsigned logn, int accepted,
	unsigned attempts, const e8_sign_trace_timing *timing);
#endif

/*
 * One timed sign_finish call of block b's scheme and variant. Returns 1 on
 * success and sets *attempts from the RNG call count and *rng_bytes as
 * described above.
 */
static int
bench_sign(const bench_block *b, unsigned acct,
	const shake_context *sc_data, cmn_sign_rng *rng,
	unsigned *attempts, uint64_t *rng_bytes, cmn_measure *m,
	int profile)
{
	unsigned logn = b->logn;
	cmn_pool_stamp start;
	cmn_rng_counters sampler0, sampler1;
	unsigned *trace_attempts = NULL;
	e8_sign_trace_timing *trace_timing = NULL;
	int ok;
#if HAWK_E8_PROFILE_SIGN
	e8_sign_trace_timing timing;
	unsigned trace_attempts_value = 0;

	/*
	 * Profile builds go through the same implementation with the
	 * per-stage trace enabled.
	 */
	trace_attempts = &trace_attempts_value;
	trace_timing = &timing;
#else
	(void)profile;
#endif

	if (b->scheme == SCHEME_HAWK) {
		hawkv_rng_counters_get(&sampler0);
	} else {
		e8_sampler_rng_counters_get(&sampler0);
	}
	cmn_pool_measure_start(&start);
	if (b->scheme == SCHEME_HAWK && b->hawkv->range == NULL) {
		if (acct == ACCT_ALL_IN) {
			ok = hawk_sign_finish(logn, cmn_sign_rng_hawk, rng,
				sig_buf, sc_data, key_priv,
				tmp_sign, sizeof tmp_sign);
		} else {
			ok = hawk_shim_sign_finish_decoded(logn,
				cmn_sign_rng_hawk, rng,
				sig_buf, sc_data, key_prepared,
				tmp_sign, sizeof tmp_sign);
		}
	} else if (b->scheme == SCHEME_HAWK) {
		if (acct == ACCT_ALL_IN) {
			ok = hawkv_sign_finish(logn, cmn_sign_rng_hawk, rng,
				sig_buf, sc_data, key_priv,
				tmp_sign, sizeof tmp_sign);
		} else {
			ok = hawkv_sign_finish_decoded(logn,
				cmn_sign_rng_hawk, rng,
				sig_buf, sc_data, key_prepared,
				tmp_sign, sizeof tmp_sign);
		}
	} else {
		if (acct == ACCT_ALL_IN) {
			ok = e8_sign_finish_inner(logn, cmn_sign_rng_hawk, rng,
				sig_buf, sc_data, key_priv,
				E8_PRIVKEY_SIZE(logn), tmp_sign, sizeof tmp_sign,
				b->e8v, trace_attempts, trace_timing);
		} else {
			ok = e8_sign_finish_inner(logn, cmn_sign_rng_hawk, rng,
				sig_buf, sc_data, key_prepared,
				E8_PRIVKEY_PREPARED_SIZE(logn), NULL, 0,
				b->e8v, trace_attempts, trace_timing);
		}
	}
	cmn_pool_measure_stop(m, &start);
	*attempts = (unsigned)(rng->calls / 2);
	if (b->scheme == SCHEME_HAWK && b->hawkv->range == NULL) {
		*rng_bytes = rng->bytes
			+ (uint64_t)*attempts * (20u << logn);
	} else {
		if (b->scheme == SCHEME_HAWK) {
			hawkv_rng_counters_get(&sampler1);
		} else {
			e8_sampler_rng_counters_get(&sampler1);
		}
		*rng_bytes = rng->bytes + (sampler1.bytes_delivered
			- sampler0.bytes_delivered);
	}
#if HAWK_E8_PROFILE_SIGN
	if (b->scheme == SCHEME_E8 && profile) {
		profile_add(acct, logn, ok, *attempts, &timing);
		if (trace_attempts_value != *attempts) {
			sign_profile_totals[acct][logn].attempt_mismatches ++;
		}
	}
#endif
	return ok;
}

static int
bench_verify(unsigned scheme, unsigned logn, const shake_context *sc_data)
{
	if (scheme == SCHEME_HAWK) {
		return hawk_verify_finish(logn, sig_buf, HAWK_SIG_SIZE(logn),
			sc_data, key_pub, HAWK_PUBKEY_SIZE(logn),
			tmp_verify, sizeof tmp_verify);
	}
	return e8_verify_finish(logn, sig_buf, E8_SIG_SIZE(logn),
		sc_data, key_pub, E8_PUBKEY_SIZE(logn),
		tmp_verify, sizeof tmp_verify);
}

/*
 * Fields shared by every row of a block.
 */
static void
row_init(cmn_sign_row *row, const bench_block *b, const char *kind,
	unsigned key_index, const cmn_measure *m)
{
	cmn_sign_row_clear(row);
	row->kind = kind;
	row->run_id = run_id;
	row->scheme = SCHEME_NAME[b->scheme];
	row->variant = b->variant;
	row->threshold_bits = b->threshold_bits;
	row->technique = b->technique;
	row->threads = b->threads;
	row->logn = b->logn;
	row->key_index = key_index;
	row->tsc_cycles = m->tsc_cycles;
	row->core_cycles = m->core_cycles;
	row->wall_ns = m->wall_ns;
	row->cpu_ns_all_threads = m->cpu_ns_all_threads;
}

static void
write_sign_row(const bench_block *b, unsigned acct, unsigned trial,
	unsigned key_index, int sign_ok, unsigned attempts,
	uint64_t rng_bytes, const cmn_measure *m, int verify_ok)
{
	cmn_sign_row row;

	row_init(&row, b, sign_ok ? "sign" : "sign_failed", key_index, m);
	row.msg_index = trial;
	row.attempts = attempts;
	row.sig_bytes = b->sig_bytes;
	row.rng_bytes = rng_bytes;
	row.accounting = ACCT_NAME[acct];
	row.encoding_included = 1;
	row.verify_ok = (uint64_t)verify_ok;
	cmn_csv_sign_row(&csv, &row);
}

static void
write_setup_row(const bench_block *b, const char *kind,
	const char *acct, unsigned key_index, const cmn_measure *m)
{
	cmn_sign_row row;

	row_init(&row, b, kind, key_index, m);
	row.accounting = acct;
	cmn_csv_sign_row(&csv, &row);
}

static int
block_alloc(bench_block *b, unsigned trials)
{
	size_t len = (size_t)trials * sizeof(uint64_t);

	for (unsigned a = 0; a < ACCT_COUNT; a ++) {
		b->cycles[a] = malloc(len);
		b->core_cycles[a] = malloc(len);
		b->wall_ns[a] = malloc(len);
		b->cpu_ns[a] = malloc(len);
		if (b->cycles[a] == NULL || b->core_cycles[a] == NULL
			|| b->wall_ns[a] == NULL || b->cpu_ns[a] == NULL)
		{
			return 0;
		}
	}
	b->keygen_cycles = malloc(len);
	b->prepare_cycles = malloc(len);
	return b->keygen_cycles != NULL && b->prepare_cycles != NULL;
}

/*
 * One untimed signature per accounting mode with a throwaway key, so
 * that every block starts from the same warm state for both schemes.
 */
static int
bench_warmup(const bench_block *b)
{
	cmn_measure m;
	shake_context sc_data;

	if (!bench_keygen(b->scheme, b->logn, 0xFFFFFFu, &m)
		|| !bench_prepare(b->scheme, b->logn, &m))
	{
		return 0;
	}
	bench_make_message(&sc_data, b->logn, 0xFFFFFFu);
	for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
		cmn_sign_rng rng;
		unsigned attempts;
		uint64_t rng_bytes;

		bench_rng_init(&rng, 9, b->logn, acct);
		if (!bench_sign(b, acct, &sc_data, &rng,
			&attempts, &rng_bytes, &m, 0))
		{
			return 0;
		}
	}
	return 1;
}

static int
variant_selected(const sign_bench_options *opts, const char *name)
{
	if (opts->variant_name_count == 0) {
		return 1;
	}
	for (unsigned u = 0; u < opts->variant_name_count; u ++) {
		if (strcmp(opts->variant_names[u], name) == 0) {
			return 1;
		}
	}
	return 0;
}

/*
 * Fill block b for variant vi of scheme at logn with thread case c;
 * returns 0 if the block is not run (variant not selected, upstream
 * HAWK ref with more than one thread, or a thread count already run).
 */
static int
block_setup(bench_block *b, const sign_bench_options *opts,
	unsigned scheme, size_t vi, unsigned logn, unsigned c)
{
	unsigned threads = opts->thread_cases[c] == 0
		? bench_selected_e8_threads(logn) : opts->thread_cases[c];

	for (unsigned d = 0; d < c; d ++) {
		unsigned prev = opts->thread_cases[d] == 0
			? bench_selected_e8_threads(logn) : opts->thread_cases[d];

		if (prev == threads) {
			return 0;
		}
	}
	memset(b, 0, sizeof *b);
	b->scheme = scheme;
	b->logn = logn;
	b->threads = threads;
	if (scheme == SCHEME_HAWK) {
		b->hawkv = &hawk_sampler_variants[vi];
		b->variant = b->hawkv->name;
		b->threshold_bits = b->hawkv->threshold_bits;
		b->technique = b->hawkv->technique;
		if (b->hawkv->range == NULL && threads != 1) {
			return 0;
		}
	} else {
		b->e8v = &e8_sampler_variants[vi];
		b->variant = b->e8v->name;
		b->threshold_bits = b->e8v->threshold_bits;
		b->technique = b->e8v->technique;
	}
	return variant_selected(opts, b->variant);
}

static int
run_block(bench_block *b, const sign_bench_options *opts, int profile)
{
	unsigned key_index = 0;

	if (!block_alloc(b, opts->trials)) {
		fprintf(stderr, "ERR: out of memory\n");
		return 0;
	}
	if (b->scheme == SCHEME_E8) {
		e8_sampler_set_thread_count(b->threads);
		e8_sampler_set_rng_mode(E8_SAMPLER_RNG_PER_WORKER);
		b->threads = e8_sampler_get_thread_count(b->logn);
		b->sig_bytes = E8_SIG_SIZE(b->logn);
	} else {
		if (!hawkv_select(b->hawkv, b->threads)) {
			fprintf(stderr, "ERR: cannot select HAWK variant %s"
				" with %u threads\n", b->variant, b->threads);
			return 0;
		}
		b->sig_bytes = HAWK_SIG_SIZE(b->logn);
	}

	/*
	 * Size the pool before the warm-up, outside any timed region:
	 * dispatches only grow the pool, so no timed call then starts or
	 * joins a thread.
	 */
	if (b->threads > 1 && !cmn_pool_ensure(b->threads - 1)) {
		fprintf(stderr, "ERR: cannot start %u pool threads\n",
			b->threads - 1);
		return 0;
	}
	if (!bench_warmup(b)) {
		fprintf(stderr, "ERR: warm-up failed scheme=%s variant=%s"
			" threads=%u logn=%u\n", SCHEME_NAME[b->scheme],
			b->variant, b->threads, b->logn);
		return 0;
	}
	cmn_pool_stats_reset();

	for (unsigned trial = 0; trial < opts->trials; trial ++) {
		shake_context sc_data;
		cmn_measure m;

		if (trial % opts->sigs_per_key == 0) {
			key_index = trial / opts->sigs_per_key;
			if (!bench_keygen(b->scheme, b->logn, key_index, &m))
			{
				fprintf(stderr, "ERR: keygen failed scheme=%s"
					" logn=%u key=%u\n",
					SCHEME_NAME[b->scheme], b->logn,
					key_index);
				return 0;
			}
			write_setup_row(b, "keygen", NULL, key_index, &m);
			b->keygen_cycles[b->keys] = m.tsc_cycles;
			if (!bench_prepare(b->scheme, b->logn, &m)) {
				fprintf(stderr, "ERR: key preparation failed"
					" scheme=%s logn=%u key=%u\n",
					SCHEME_NAME[b->scheme], b->logn,
					key_index);
				return 0;
			}
			write_setup_row(b, "prepare", ACCT_NAME[ACCT_PREPARED],
				key_index, &m);
			b->prepare_cycles[b->keys] = m.tsc_cycles;
			b->keys ++;
		}

		bench_make_message(&sc_data, b->logn, trial);
		for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
			cmn_sign_rng rng;
			unsigned attempts;
			uint64_t rng_bytes;
			int sign_ok, verify_ok = 0;

			bench_rng_init(&rng, 16 + acct, b->logn, trial);
			sign_ok = bench_sign(b, acct, &sc_data, &rng,
				&attempts, &rng_bytes, &m, profile);
			if (sign_ok) {
				uint64_t k = b->signatures[acct];

				verify_ok = bench_verify(b->scheme, b->logn,
					&sc_data);
				b->cycles[acct][k] = m.tsc_cycles;
				b->core_cycles[acct][k] = m.core_cycles;
				b->wall_ns[acct][k] = m.wall_ns;
				b->cpu_ns[acct][k] = m.cpu_ns_all_threads;
				b->signatures[acct] ++;
				b->attempts[acct] += attempts;
				b->rng_bytes[acct] += rng_bytes;
				if (!verify_ok) {
					b->verify_failures[acct] ++;
				}
			} else {
				b->sign_failures[acct] ++;
			}
			write_sign_row(b, acct, trial, key_index, sign_ok,
				attempts, rng_bytes, &m, verify_ok);
		}
	}
	cmn_pool_stats_get(&b->pool);
	e8_sampler_set_thread_count(1);
	(void)hawkv_select(&hawk_sampler_variants[0], 1);

	/*
	 * Join the pool threads outside any timed region, so that the next
	 * block does not run beside spinning workers.
	 */
	cmn_pool_shrink(0);
	return 1;
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

/*
 * Median, or CMN_UNAVAILABLE if there are no values or any value is
 * unavailable.
 */
static uint64_t
median_u64(uint64_t *v, uint64_t count)
{
	if (count == 0) {
		return CMN_UNAVAILABLE;
	}
	for (uint64_t u = 0; u < count; u ++) {
		if (v[u] == CMN_UNAVAILABLE) {
			return CMN_UNAVAILABLE;
		}
	}
	qsort(v, (size_t)count, sizeof *v, cmp_u64);
	if ((count & 1u) != 0) {
		return v[count >> 1];
	}
	return (v[(count >> 1) - 1] + v[count >> 1]) / 2;
}

static double
mean_u64(const uint64_t *v, uint64_t count)
{
	double s = 0.0;

	for (uint64_t u = 0; u < count; u ++) {
		s += (double)v[u];
	}
	return count == 0 ? 0.0 : s / (double)count;
}

/*
 * A summary value: empty if unavailable.
 */
static const char *
fmt_u64(char *buf, size_t len, uint64_t x)
{
	if (x == CMN_UNAVAILABLE) {
		buf[0] = 0;
	} else {
		snprintf(buf, len, "%llu", (unsigned long long)x);
	}
	return buf;
}

static void
print_summary(const sign_bench_options *opts)
{
	fprintf(stderr,
		"\nHAWK_E8_SIGN_BENCH run_id=%s core_cycles=%s"
		" pool_accounting=%s\n",
		run_id, cmn_core_cycles_status(),
		opts->pool_accounting ? "on" : "off");
	for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
		fprintf(stderr,
			"\nHAWK_E8_SIGN_BENCH accounting=%s trials=%u"
			" sigs_per_key=%u\n"
			"scheme,variant,logn,threads,signatures,"
			"median_cycles,mean_cycles,median_core_cycles,"
			"median_wall_ns,median_cpu_ns_all_threads,"
			"attempts_per_sign,rng_bytes_per_sign,"
			"rng_bytes_per_attempt,sig_bytes,sign_failures,"
			"verify_failures\n",
			ACCT_NAME[acct], opts->trials, opts->sigs_per_key);
		for (unsigned u = 0; u < block_count; u ++) {
			bench_block *b = &blocks[u];
			uint64_t count = b->signatures[acct];
			double mean = mean_u64(b->cycles[acct], count);
			char f0[24], f1[24], f2[24], f3[24];

			fprintf(stderr, "%s,%s,%u,%u,%llu,%s,%.0f,%s,%s,%s,"
				"%.4f,%.1f,%.1f,%u,%llu,%llu\n",
				SCHEME_NAME[b->scheme], b->variant,
				b->logn, b->threads,
				(unsigned long long)count,
				fmt_u64(f0, sizeof f0,
					median_u64(b->cycles[acct], count)),
				mean,
				fmt_u64(f1, sizeof f1, median_u64(
					b->core_cycles[acct], count)),
				fmt_u64(f2, sizeof f2,
					median_u64(b->wall_ns[acct], count)),
				fmt_u64(f3, sizeof f3,
					median_u64(b->cpu_ns[acct], count)),
				count == 0 ? 0.0 : (double)b->attempts[acct]
					/ (double)count,
				count == 0 ? 0.0 : (double)b->rng_bytes[acct]
					/ (double)count,
				b->attempts[acct] == 0 ? 0.0
					: (double)b->rng_bytes[acct]
					/ (double)b->attempts[acct],
				b->sig_bytes,
				(unsigned long long)b->sign_failures[acct],
				(unsigned long long)b->verify_failures[acct]);
		}
	}
	fprintf(stderr,
		"\nHAWK_E8_SIGN_BENCH setup (timed separately, not included"
		" in any signing figure)\n"
		"scheme,variant,logn,threads,keys,median_keygen_cycles,"
		"median_prepare_cycles\n");
	for (unsigned u = 0; u < block_count; u ++) {
		bench_block *b = &blocks[u];
		char f0[24], f1[24];

		fprintf(stderr, "%s,%s,%u,%u,%llu,%s,%s\n",
			SCHEME_NAME[b->scheme], b->variant,
			b->logn, b->threads,
			(unsigned long long)b->keys,
			fmt_u64(f0, sizeof f0,
				median_u64(b->keygen_cycles, b->keys)),
			fmt_u64(f1, sizeof f1,
				median_u64(b->prepare_cycles, b->keys)));
	}
	fprintf(stderr,
		"\nHAWK_E8_SIGN_BENCH pool per block, after warm-up (job CPU"
		" is in cpu_ns_all_threads; idle CPU is pool spinning between"
		" dispatches, excluded from every row)\n"
		"scheme,variant,logn,threads,dispatches,threads_started,"
		"threads_joined,job_cpu_ns,idle_cpu_ns,"
		"idle_cpu_ns_per_sign\n");
	for (unsigned u = 0; u < block_count; u ++) {
		bench_block *b = &blocks[u];
		uint64_t sigs = b->signatures[ACCT_ALL_IN]
			+ b->signatures[ACCT_PREPARED];

		fprintf(stderr, "%s,%s,%u,%u,%llu,%llu,%llu,%llu,%llu,%.0f\n",
			SCHEME_NAME[b->scheme], b->variant,
			b->logn, b->threads,
			(unsigned long long)b->pool.dispatches,
			(unsigned long long)b->pool.threads_started,
			(unsigned long long)b->pool.threads_joined,
			(unsigned long long)b->pool.job_cpu_ns,
			(unsigned long long)b->pool.idle_cpu_ns,
			sigs == 0 ? 0.0
				: (double)b->pool.idle_cpu_ns / (double)sigs);
	}
}

#if HAWK_E8_PROFILE_SIGN
static uint64_t
profile_avg(uint64_t total, uint64_t count)
{
	if (total == 0 || count == 0) {
		return 0;
	}
	return total / count;
}

static double
profile_percent(uint64_t part, uint64_t total)
{
	if (part == 0 || total == 0) {
		return 0.0;
	}
	return 100.0 * (double)part / (double)total;
}

static void
profile_add(unsigned acct, unsigned logn, int accepted, unsigned attempts,
	const e8_sign_trace_timing *timing)
{
	bench_sign_profile_totals *total;

	if (acct >= ACCT_COUNT || logn > 10 || timing == NULL) {
		return;
	}
	total = &sign_profile_totals[acct][logn];
	total->trials ++;
	if (accepted) {
		total->accepted ++;
	} else {
		total->failed ++;
	}
	total->attempts += timing->attempts_total != 0
		? timing->attempts_total : attempts;
	total->rejections += timing->rejections_total;
	total->cycles_total += timing->cycles_sign_total;
	total->cycles_prepare += timing->cycles_prepare_total;
	total->cycles_salt += timing->cycles_salt_total;
	total->cycles_hash += timing->cycles_hash_total;
	total->cycles_target += timing->cycles_target_total;
	total->cycles_sample += timing->cycles_sample_total;
	total->cycles_reconstruct += timing->cycles_reconstruct_total;
	total->cycles_norm_check += timing->cycles_norm_check_total;
	total->cycles_encode += timing->cycles_encode_total;
	total->wall_ns_total += timing->wall_ns_sign_total;
	total->wall_ns_prepare += timing->wall_ns_prepare_total;
	total->wall_ns_salt += timing->wall_ns_salt_total;
	total->wall_ns_hash += timing->wall_ns_hash_total;
	total->wall_ns_target += timing->wall_ns_target_total;
	total->wall_ns_sample += timing->wall_ns_sample_total;
	total->wall_ns_reconstruct += timing->wall_ns_reconstruct_total;
	total->wall_ns_norm_check += timing->wall_ns_norm_check_total;
	total->wall_ns_encode += timing->wall_ns_encode_total;
}

static void
profile_print_stage(const char *name, uint64_t cycles, uint64_t cycles_total,
	uint64_t wall_ns, uint64_t wall_ns_total)
{
	fprintf(stderr,
		"  %-18s cycles=%12llu %6.2f%%  wall_ns=%12llu %6.2f%%\n",
		name,
		(unsigned long long)cycles,
		profile_percent(cycles, cycles_total),
		(unsigned long long)wall_ns,
		profile_percent(wall_ns, wall_ns_total));
}

static void
profile_print_summary(void)
{
	for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
		fprintf(stderr,
			"\nHAWK_E8_PROFILE_SIGN summary "
			"(threaded E8, accounting=%s)\n", ACCT_NAME[acct]);
		for (unsigned logn = 8; logn <= 10; logn ++) {
			const bench_sign_profile_totals *total =
				&sign_profile_totals[acct][logn];
			uint64_t stage_cycles, stage_wall_ns;
			uint64_t other_cycles = 0, other_wall_ns = 0;

			if (total->trials == 0) {
				continue;
			}
			stage_cycles = total->cycles_prepare
				+ total->cycles_salt
				+ total->cycles_hash
				+ total->cycles_target
				+ total->cycles_sample
				+ total->cycles_reconstruct
				+ total->cycles_norm_check
				+ total->cycles_encode;
			stage_wall_ns = total->wall_ns_prepare
				+ total->wall_ns_salt
				+ total->wall_ns_hash
				+ total->wall_ns_target
				+ total->wall_ns_sample
				+ total->wall_ns_reconstruct
				+ total->wall_ns_norm_check
				+ total->wall_ns_encode;
			if (total->cycles_total > stage_cycles) {
				other_cycles = total->cycles_total
					- stage_cycles;
			}
			if (total->wall_ns_total > stage_wall_ns) {
				other_wall_ns = total->wall_ns_total
					- stage_wall_ns;
			}

			fprintf(stderr,
				"logn=%u n=%u trials=%llu accepted=%llu"
				" failed=%llu attempts=%llu rejections=%llu"
				" attempt_counter_mismatches=%llu\n",
				logn, 1u << logn,
				(unsigned long long)total->trials,
				(unsigned long long)total->accepted,
				(unsigned long long)total->failed,
				(unsigned long long)total->attempts,
				(unsigned long long)total->rejections,
				(unsigned long long)total->attempt_mismatches);
			fprintf(stderr,
				"  total cycles=%llu wall_ns=%llu "
				"avg_cycles/sign=%llu avg_wall_ns/sign=%llu\n",
				(unsigned long long)total->cycles_total,
				(unsigned long long)total->wall_ns_total,
				(unsigned long long)profile_avg(
					total->cycles_total, total->trials),
				(unsigned long long)profile_avg(
					total->wall_ns_total, total->trials));
			profile_print_stage("key_prepare",
				total->cycles_prepare, total->cycles_total,
				total->wall_ns_prepare, total->wall_ns_total);
			profile_print_stage("salt",
				total->cycles_salt, total->cycles_total,
				total->wall_ns_salt, total->wall_ns_total);
			profile_print_stage("hash_challenge",
				total->cycles_hash, total->cycles_total,
				total->wall_ns_hash, total->wall_ns_total);
			profile_print_stage("target_coset",
				total->cycles_target, total->cycles_total,
				total->wall_ns_target, total->wall_ns_total);
			profile_print_stage("e8_sampler",
				total->cycles_sample, total->cycles_total,
				total->wall_ns_sample, total->wall_ns_total);
			profile_print_stage("reconstruct_s",
				total->cycles_reconstruct, total->cycles_total,
				total->wall_ns_reconstruct,
				total->wall_ns_total);
			profile_print_stage("norm_rejection",
				total->cycles_norm_check, total->cycles_total,
				total->wall_ns_norm_check,
				total->wall_ns_total);
			profile_print_stage("encode_sig",
				total->cycles_encode, total->cycles_total,
				total->wall_ns_encode, total->wall_ns_total);
			profile_print_stage("other",
				other_cycles, total->cycles_total,
				other_wall_ns, total->wall_ns_total);
		}
	}
}
#endif

static int
parse_unsigned_arg(const char *text, unsigned min_value,
	unsigned max_value, unsigned *out)
{
	char *end = NULL;
	unsigned long x;

	if (text == NULL || out == NULL) {
		return 0;
	}
	x = strtoul(text, &end, 10);
	if (end == text || *end != 0
		|| x < min_value || x > max_value)
	{
		return 0;
	}
	*out = (unsigned)x;
	return 1;
}

/*
 * Comma-separated thread counts (1..24) or "sel".
 */
static int
parse_thread_list(const char *text, sign_bench_options *opts)
{
	char buf[128];
	char *tok, *save = NULL;

	if (strlen(text) >= sizeof buf) {
		return 0;
	}
	strcpy(buf, text);
	opts->thread_case_count = 0;
	for (tok = strtok_r(buf, ",", &save); tok != NULL;
		tok = strtok_r(NULL, ",", &save))
	{
		unsigned t;

		if (opts->thread_case_count == MAX_THREAD_CASES) {
			return 0;
		}
		if (strcmp(tok, "sel") == 0) {
			t = 0;
		} else if (!parse_unsigned_arg(tok, 1, 24, &t)) {
			return 0;
		}
		opts->thread_cases[opts->thread_case_count ++] = t;
	}
	return opts->thread_case_count > 0;
}

static void
list_variants(void)
{
	printf("scheme,variant,threshold_bits,technique,threads\n");
	for (size_t u = 0; u < hawk_sampler_variant_count; u ++) {
		const hawk_sampler_variant *v = &hawk_sampler_variants[u];

		printf("hawk,%s,%u,%s,%s\n", v->name, v->threshold_bits,
			v->technique, v->range == NULL ? "1" : "1..24");
	}
	for (size_t u = 0; u < e8_sampler_variant_count; u ++) {
		const e8_sampler_variant *v = &e8_sampler_variants[u];

		printf("e8,%s,%u,%s,1..24\n", v->name, v->threshold_bits,
			v->technique);
	}
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--trials N] [--sigs-per-key N] [--logn 8|9|10]"
		" [--variant NAME]... [--threads LIST]\n"
		"       [--no-header] [--no-pool-accounting] [--list]\n"
		"  --trials N        signatures per scheme, accounting mode,"
		" degree and\n"
		"                    thread count (default %u, or"
		" E8_SIGN_BENCH_TRIALS)\n"
		"  --sigs-per-key N  signatures sharing one key (default %u);"
		" this does\n"
		"                    not change the accounting\n"
		"  --variant NAME    run this sampler variant of every scheme"
		" that has it\n"
		"                    (repeatable; default: all; see --list)\n"
		"  --threads LIST    comma-separated thread counts, 1..24, or"
		" sel for the\n"
		"                    per-logn selection 8, 8, 16 (default"
		" 1,sel); upstream\n"
		"                    HAWK ref runs with 1 only\n"
		"  --list            list the variants of both schemes and"
		" exit\n"
		"  --no-pool-accounting\n"
		"                    do not time pool jobs per worker; threaded"
		" rows then\n"
		"                    have empty cpu_ns_all_threads and"
		" core_cycles\n",
		prog, DEFAULT_SIGN_BENCH_TRIALS, DEFAULT_SIGS_PER_KEY);
}

static int
parse_options(int argc, char **argv, sign_bench_options *opts)
{
	const char *env = getenv("E8_SIGN_BENCH_TRIALS");

	memset(opts, 0, sizeof *opts);
	opts->trials = DEFAULT_SIGN_BENCH_TRIALS;
	opts->sigs_per_key = DEFAULT_SIGS_PER_KEY;
	opts->logn_min = 8;
	opts->logn_max = 10;
	opts->pool_accounting = 1;
	opts->thread_cases[0] = 1;
	opts->thread_cases[1] = 0;
	opts->thread_case_count = 2;
	if (env != NULL && env[0] != 0 && !parse_unsigned_arg(env,
		1, MAX_BENCH_TRIALS, &opts->trials))
	{
		fprintf(stderr, "ERR: E8_SIGN_BENCH_TRIALS must be in [1,%u]\n",
			MAX_BENCH_TRIALS);
		return 0;
	}

	for (int i = 1; i < argc; i ++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--help") == 0) {
			usage(argv[0]);
			exit(0);
		} else if (strcmp(arg, "--no-header") == 0) {
			opts->no_header = 1;
		} else if (strcmp(arg, "--no-pool-accounting") == 0) {
			opts->pool_accounting = 0;
		} else if (strcmp(arg, "--list") == 0) {
			list_variants();
			exit(0);
		} else if (strcmp(arg, "--variant") == 0) {
			if (++ i >= argc || opts->variant_name_count
				== MAX_VARIANT_NAMES
				|| (hawk_sampler_variant_find(argv[i]) == NULL
				&& e8_sampler_variant_find(argv[i]) == NULL))
			{
				fprintf(stderr, "ERR: invalid --variant\n");
				return 0;
			}
			opts->variant_names[opts->variant_name_count ++] =
				argv[i];
		} else if (strcmp(arg, "--threads") == 0) {
			if (++ i >= argc || !parse_thread_list(argv[i], opts)) {
				fprintf(stderr, "ERR: invalid --threads\n");
				return 0;
			}
		} else if (strcmp(arg, "--trials") == 0) {
			if (++ i >= argc || !parse_unsigned_arg(argv[i],
				1, MAX_BENCH_TRIALS, &opts->trials))
			{
				fprintf(stderr, "ERR: invalid --trials\n");
				return 0;
			}
		} else if (strcmp(arg, "--sigs-per-key") == 0) {
			if (++ i >= argc || !parse_unsigned_arg(argv[i],
				1, MAX_BENCH_TRIALS, &opts->sigs_per_key))
			{
				fprintf(stderr, "ERR: invalid --sigs-per-key\n");
				return 0;
			}
		} else if (strcmp(arg, "--logn") == 0) {
			if (++ i >= argc || !parse_unsigned_arg(argv[i],
				8, 10, &opts->logn_min))
			{
				fprintf(stderr, "ERR: invalid --logn\n");
				return 0;
			}
			opts->logn_max = opts->logn_min;
		} else {
			fprintf(stderr, "ERR: unknown argument: %s\n", arg);
			return 0;
		}
	}
	return 1;
}

int
main(int argc, char **argv)
{
	sign_bench_options opts;
	uint64_t failures = 0;

	if (!parse_options(argc, argv, &opts)) {
		usage(argv[0]);
		return 1;
	}
	make_run_id();
	cmn_csv_sign_init(&csv, stdout);
	if (opts.no_header) {
		cmn_csv_skip_header(&csv);
	} else {
		cmn_csv_header(&csv);
	}
	(void)cmn_core_cycles_open();
	if (!cmn_pool_set_accounting(opts.pool_accounting)) {
		fprintf(stderr, "ERR: cannot set pool accounting\n");
		return 1;
	}
	for (unsigned logn = opts.logn_min; logn <= opts.logn_max; logn ++) {
		for (unsigned scheme = 0; scheme < 2; scheme ++) {
			size_t vcount = scheme == SCHEME_HAWK
				? hawk_sampler_variant_count
				: e8_sampler_variant_count;

			for (size_t vi = 0; vi < vcount; vi ++) {
				for (unsigned c = 0; c < opts.thread_case_count;
					c ++)
				{
					bench_block *b;

					if (block_count == MAX_BLOCKS) {
						fprintf(stderr, "ERR: too many"
							" blocks\n");
						return 1;
					}
					b = &blocks[block_count];
					if (!block_setup(b, &opts, scheme, vi,
						logn, c))
					{
						continue;
					}
					block_count ++;
					if (!run_block(b, &opts,
						scheme == SCHEME_E8
						&& b->e8v == e8_sampler_variant_default()
						&& b->threads > 1))
					{
						return 1;
					}
					for (unsigned acct = 0;
						acct < ACCT_COUNT; acct ++)
					{
						failures += b->sign_failures[acct]
							+ b->verify_failures[acct];
					}
				}
			}
		}
	}
	fflush(stdout);
#if HAWK_E8_PROFILE_SIGN
	profile_print_summary();
#endif
	print_summary(&opts);
	if (failures != 0) {
		fprintf(stderr, "\nERR: %llu signing or verification"
			" failures\n", (unsigned long long)failures);
		return 1;
	}
	return 0;
}
