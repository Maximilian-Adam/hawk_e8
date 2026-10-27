#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bench_sign_core.h"

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
 * Sampler variants: every block is one (scheme, variant, thread
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
 * (cmn_csv.h; mode = latency, proc and batch empty, flags = the build's
 * flag set). stderr: one summary table per accounting mode, then the
 * separately timed setup costs and the pool activity per block.
 *
 * Key generation, preparation, signing, verification, seeds and messages
 * are bench_sign_core.c's, shared with bench_compare (the interleaved
 * comparison harness). Profile builds
 * (HAWK_E8_PROFILE_SIGN = 1, bin/e8_sign_profile_bench) record E8's
 * per-stage trace for every E8 block; --stage-csv FILE writes it as CSV.
 */

#define DEFAULT_SIGN_BENCH_TRIALS    16
#define DEFAULT_SIGS_PER_KEY         1
#define MAX_BENCH_TRIALS             1000000

#define SCHEME_HAWK    BSC_SCHEME_HAWK
#define SCHEME_E8      BSC_SCHEME_E8

#define ACCT_ALL_IN     BSC_ACCT_ALL_IN
#define ACCT_PREPARED   BSC_ACCT_PREPARED
#define ACCT_COUNT      BSC_ACCT_COUNT

#define SCHEME_NAME   bsc_scheme_name
#define ACCT_NAME     bsc_acct_name

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
	const char *stage_csv;
} sign_bench_options;

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
#endif

/*
 * One benchmark block: a scheme's sampler variant at one degree and
 * thread count.
 */
typedef struct {
	bsc_config cfg;
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
#if HAWK_E8_PROFILE_SIGN
	bench_sign_profile_totals profile[ACCT_COUNT];
#endif
} bench_block;

#define MAX_BLOCKS   256

static bench_block blocks[MAX_BLOCKS];
static unsigned block_count;

static char run_id[48];
static cmn_csv csv;

static bsc_key key;
static uint8_t sig_buf[BSC_SIG_SIZE];

static unsigned
bench_selected_e8_threads(unsigned logn)
{
	switch (logn) {
	case 8: return 8;
	case 9: return 8;
	default: return 16;
	}
}

#if HAWK_E8_PROFILE_SIGN
static void profile_add(bench_sign_profile_totals *total, int accepted,
	unsigned attempts, const e8_sign_trace_timing *timing);
#endif

/*
 * One timed sign_finish call of block b's scheme and variant
 * (bsc_sign()); profile builds record E8's per-stage trace into the
 * block when profile is set.
 */
static int
bench_sign(bench_block *b, unsigned acct,
	const shake_context *sc_data, cmn_sign_rng *rng,
	unsigned *attempts, uint64_t *rng_bytes, cmn_measure *m,
	int profile)
{
	int ok;
#if HAWK_E8_PROFILE_SIGN
	e8_sign_trace_timing timing;
	unsigned trace_attempts = 0;

	/*
	 * Profile builds go through the same implementation with the
	 * per-stage trace enabled.
	 */
	ok = bsc_sign(&b->cfg, acct, &key, sc_data, rng, sig_buf,
		attempts, rng_bytes, m, &trace_attempts, &timing);
	if (b->cfg.scheme == SCHEME_E8 && profile) {
		profile_add(&b->profile[acct], ok, *attempts, &timing);
		if (trace_attempts != *attempts) {
			b->profile[acct].attempt_mismatches ++;
		}
	}
#else
	(void)profile;
	ok = bsc_sign(&b->cfg, acct, &key, sc_data, rng, sig_buf,
		attempts, rng_bytes, m, NULL, NULL);
#endif
	return ok;
}

static void
write_sign_row(const bench_block *b, unsigned acct, unsigned trial,
	unsigned key_index, int sign_ok, unsigned attempts,
	uint64_t rng_bytes, const cmn_measure *m, int verify_ok)
{
	cmn_sign_row row;

	bsc_row_init(&row, run_id, sign_ok ? "sign" : "sign_failed",
		&b->cfg, 0, 0, key_index, m);
	row.msg_index = trial;
	row.attempts = attempts;
	row.sig_bytes = b->cfg.sig_bytes;
	row.rng_bytes = rng_bytes;
	row.accounting = ACCT_NAME[acct];
	row.encoding_included = 1;
	row.verify_ok = (uint64_t)verify_ok;
	row.mode = "latency";
	cmn_csv_sign_row(&csv, &row);
}

static void
write_setup_row(const bench_block *b, const char *kind,
	const char *acct, unsigned key_index, const cmn_measure *m)
{
	cmn_sign_row row;

	bsc_row_init(&row, run_id, kind, &b->cfg, 0, 0, key_index, m);
	row.accounting = acct;
	row.mode = "latency";
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
bench_warmup(bench_block *b)
{
	cmn_measure m;
	shake_context sc_data;

	if (!bsc_keygen(b->cfg.scheme, b->cfg.logn, 0xFFFFFFu, &key, &m)
		|| !bsc_prepare(b->cfg.scheme, b->cfg.logn, &key, &m))
	{
		return 0;
	}
	bsc_make_message(&sc_data, b->cfg.logn, 0xFFFFFFu);
	for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
		cmn_sign_rng rng;
		unsigned attempts;
		uint64_t rng_bytes;

		bsc_rng_init(&rng, BSC_STREAM_WARMUP, b->cfg.logn, acct);
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
	if (!bsc_config_init(&b->cfg, scheme, vi, logn, threads)) {
		return 0;
	}
	return variant_selected(opts, b->cfg.variant);
}

static int
run_block(bench_block *b, const sign_bench_options *opts, int profile)
{
	unsigned key_index = 0;

	if (!block_alloc(b, opts->trials)) {
		fprintf(stderr, "ERR: out of memory\n");
		return 0;
	}
	if (!bsc_select(&b->cfg)) {
		fprintf(stderr, "ERR: cannot select %s variant %s"
			" with %u threads\n", SCHEME_NAME[b->cfg.scheme],
			b->cfg.variant, b->cfg.threads);
		return 0;
	}

	/*
	 * Size the pool before the warm-up, outside any timed region:
	 * dispatches only grow the pool, so no timed call then starts or
	 * joins a thread.
	 */
	if (b->cfg.threads > 1 && !cmn_pool_ensure(b->cfg.threads - 1)) {
		fprintf(stderr, "ERR: cannot start %u pool threads\n",
			b->cfg.threads - 1);
		return 0;
	}
	if (!bench_warmup(b)) {
		fprintf(stderr, "ERR: warm-up failed scheme=%s variant=%s"
			" threads=%u logn=%u\n", SCHEME_NAME[b->cfg.scheme],
			b->cfg.variant, b->cfg.threads, b->cfg.logn);
		return 0;
	}
	cmn_pool_stats_reset();

	for (unsigned trial = 0; trial < opts->trials; trial ++) {
		shake_context sc_data;
		cmn_measure m;

		if (trial % opts->sigs_per_key == 0) {
			key_index = trial / opts->sigs_per_key;
			if (!bsc_keygen(b->cfg.scheme, b->cfg.logn, key_index,
				&key, &m))
			{
				fprintf(stderr, "ERR: keygen failed scheme=%s"
					" logn=%u key=%u\n",
					SCHEME_NAME[b->cfg.scheme], b->cfg.logn,
					key_index);
				return 0;
			}
			write_setup_row(b, "keygen", NULL, key_index, &m);
			b->keygen_cycles[b->keys] = m.tsc_cycles;
			if (!bsc_prepare(b->cfg.scheme, b->cfg.logn, &key, &m)) {
				fprintf(stderr, "ERR: key preparation failed"
					" scheme=%s logn=%u key=%u\n",
					SCHEME_NAME[b->cfg.scheme], b->cfg.logn,
					key_index);
				return 0;
			}
			write_setup_row(b, "prepare", ACCT_NAME[ACCT_PREPARED],
				key_index, &m);
			b->prepare_cycles[b->keys] = m.tsc_cycles;
			b->keys ++;
		}

		bsc_make_message(&sc_data, b->cfg.logn, trial);
		for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
			cmn_sign_rng rng;
			unsigned attempts;
			uint64_t rng_bytes;
			int sign_ok, verify_ok = 0;

			bsc_rng_init(&rng, BSC_STREAM_SIGN + acct, b->cfg.logn,
				trial);
			sign_ok = bench_sign(b, acct, &sc_data, &rng,
				&attempts, &rng_bytes, &m, profile);
			if (sign_ok) {
				uint64_t k = b->signatures[acct];

				verify_ok = bsc_verify(b->cfg.scheme,
					b->cfg.logn, &key, sig_buf, &sc_data);
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
	bsc_deselect();

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
		" pool_accounting=%s flags=%s\n",
		run_id, cmn_core_cycles_status(),
		opts->pool_accounting ? "on" : "off", bsc_build_flagset);
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
				SCHEME_NAME[b->cfg.scheme], b->cfg.variant,
				b->cfg.logn, b->cfg.threads,
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
				b->cfg.sig_bytes,
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
			SCHEME_NAME[b->cfg.scheme], b->cfg.variant,
			b->cfg.logn, b->cfg.threads,
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
			SCHEME_NAME[b->cfg.scheme], b->cfg.variant,
			b->cfg.logn, b->cfg.threads,
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
profile_add(bench_sign_profile_totals *total, int accepted,
	unsigned attempts, const e8_sign_trace_timing *timing)
{
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

/*
 * The stages of one block and accounting mode, with the remainder
 * ("other") and the total last.
 */
#define PROFILE_STAGES   10

static void
profile_stages(const bench_sign_profile_totals *total,
	const char **name, uint64_t *cycles, uint64_t *wall_ns)
{
	static const char *const names[PROFILE_STAGES] = {
		"key_prepare", "salt", "hash_challenge", "target_coset",
		"e8_sampler", "reconstruct_s", "norm_rejection", "encode_sig",
		"other", "total"
	};
	uint64_t sc = 0, sw = 0;

	cycles[0] = total->cycles_prepare;
	cycles[1] = total->cycles_salt;
	cycles[2] = total->cycles_hash;
	cycles[3] = total->cycles_target;
	cycles[4] = total->cycles_sample;
	cycles[5] = total->cycles_reconstruct;
	cycles[6] = total->cycles_norm_check;
	cycles[7] = total->cycles_encode;
	wall_ns[0] = total->wall_ns_prepare;
	wall_ns[1] = total->wall_ns_salt;
	wall_ns[2] = total->wall_ns_hash;
	wall_ns[3] = total->wall_ns_target;
	wall_ns[4] = total->wall_ns_sample;
	wall_ns[5] = total->wall_ns_reconstruct;
	wall_ns[6] = total->wall_ns_norm_check;
	wall_ns[7] = total->wall_ns_encode;
	for (unsigned u = 0; u < 8; u ++) {
		sc += cycles[u];
		sw += wall_ns[u];
	}
	cycles[8] = total->cycles_total > sc ? total->cycles_total - sc : 0;
	wall_ns[8] = total->wall_ns_total > sw
		? total->wall_ns_total - sw : 0;
	cycles[9] = total->cycles_total;
	wall_ns[9] = total->wall_ns_total;
	for (unsigned u = 0; u < PROFILE_STAGES; u ++) {
		name[u] = names[u];
	}
}

static void
profile_print_summary(void)
{
	for (unsigned u = 0; u < block_count; u ++) {
		const bench_block *b = &blocks[u];

		if (b->cfg.scheme != SCHEME_E8) {
			continue;
		}
		for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
			const bench_sign_profile_totals *total =
				&b->profile[acct];
			const char *name[PROFILE_STAGES];
			uint64_t cycles[PROFILE_STAGES];
			uint64_t wall_ns[PROFILE_STAGES];

			if (total->trials == 0) {
				continue;
			}
			profile_stages(total, name, cycles, wall_ns);
			fprintf(stderr,
				"\nHAWK_E8_PROFILE_SIGN summary (E8 variant=%s"
				" threads=%u, accounting=%s)\n",
				b->cfg.variant, b->cfg.threads, ACCT_NAME[acct]);
			fprintf(stderr,
				"logn=%u n=%u trials=%llu accepted=%llu"
				" failed=%llu attempts=%llu rejections=%llu"
				" attempt_counter_mismatches=%llu\n",
				b->cfg.logn, 1u << b->cfg.logn,
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
			for (unsigned s = 0; s + 1 < PROFILE_STAGES; s ++) {
				fprintf(stderr, "  %-18s cycles=%12llu %6.2f%%"
					"  wall_ns=%12llu %6.2f%%\n", name[s],
					(unsigned long long)cycles[s],
					profile_percent(cycles[s],
						total->cycles_total),
					(unsigned long long)wall_ns[s],
					profile_percent(wall_ns[s],
						total->wall_ns_total));
			}
		}
	}
}

/*
 * The per-stage totals as CSV (the stage table of the comparison
 * analysis): one row per E8 block, accounting mode and stage.
 */
static int
profile_write_csv(const char *path)
{
	FILE *fp = fopen(path, "w");

	if (fp == NULL) {
		return 0;
	}
	fprintf(fp, "scheme,variant,threads,logn,accounting,flags,"
		"signatures,attempts,stage,cycles_total,wall_ns_total\n");
	for (unsigned u = 0; u < block_count; u ++) {
		const bench_block *b = &blocks[u];

		if (b->cfg.scheme != SCHEME_E8) {
			continue;
		}
		for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
			const bench_sign_profile_totals *total =
				&b->profile[acct];
			const char *name[PROFILE_STAGES];
			uint64_t cycles[PROFILE_STAGES];
			uint64_t wall_ns[PROFILE_STAGES];

			if (total->trials == 0) {
				continue;
			}
			profile_stages(total, name, cycles, wall_ns);
			for (unsigned s = 0; s < PROFILE_STAGES; s ++) {
				fprintf(fp, "e8,%s,%u,%u,%s,%s,%llu,%llu,%s,"
					"%llu,%llu\n", b->cfg.variant,
					b->cfg.threads, b->cfg.logn,
					ACCT_NAME[acct], bsc_build_flagset,
					(unsigned long long)total->accepted,
					(unsigned long long)total->attempts,
					name[s],
					(unsigned long long)cycles[s],
					(unsigned long long)wall_ns[s]);
			}
		}
	}
	return fclose(fp) == 0;
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

#if HAWK_E8_PROFILE_SIGN
#define STAGE_CSV_SYNOPSIS   " [--stage-csv FILE]"
#define STAGE_CSV_HELP \
	"  --stage-csv FILE  write E8's per-stage totals per block to FILE\n"
#else
#define STAGE_CSV_SYNOPSIS   ""
#define STAGE_CSV_HELP       ""
#endif

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--trials N] [--sigs-per-key N] [--logn 8|9|10]"
		" [--variant NAME]... [--threads LIST]\n"
		"       [--no-header] [--no-pool-accounting] [--list]"
		" [--build-info]" STAGE_CSV_SYNOPSIS "\n"
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
		" core_cycles\n"
		"  --build-info      print the build's flag set, flags and"
		" compiler and exit\n"
		STAGE_CSV_HELP,
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
		} else if (strcmp(arg, "--build-info") == 0) {
			printf("flags: %s\ncflags: %s\ncompiler: %s\n",
				bsc_build_flagset, bsc_build_cflags,
				bsc_build_compiler);
			exit(0);
#if HAWK_E8_PROFILE_SIGN
		} else if (strcmp(arg, "--stage-csv") == 0) {
			if (++ i >= argc) {
				fprintf(stderr, "ERR: invalid --stage-csv\n");
				return 0;
			}
			opts->stage_csv = argv[i];
#endif
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
	bsc_make_run_id(run_id, sizeof run_id);
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
			size_t vcount = bsc_variant_count(scheme);

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
						scheme == SCHEME_E8))
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
	if (opts.stage_csv != NULL && !profile_write_csv(opts.stage_csv)) {
		fprintf(stderr, "ERR: cannot write %s\n", opts.stage_csv);
		return 1;
	}
#endif
	print_summary(&opts);
	if (failures != 0) {
		fprintf(stderr, "\nERR: %llu signing or verification"
			" failures\n", (unsigned long long)failures);
		return 1;
	}
	return 0;
}
