#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "../src/hawk_e8_inner.h"
#include "hawk_shim.h"

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
 * Attempts are counted identically for both schemes from the signing RNG:
 * each attempt calls it exactly twice (salt, then sampler seed), so
 * attempts = rng_calls / 2.
 *
 * E8 runs with one sampler thread and with the selected per-logn thread
 * count (8, 8, 16); the thread count is reported on every row. Ordinary
 * HAWK is serial. Before each block of signatures, one untimed signature
 * per accounting mode warms caches (and the E8 sampler tables and worker
 * pool) for both schemes alike.
 *
 * stdout: one CSV row per signature, plus one row per key generation and
 * per key preparation (kind = keygen / prepare). stderr: one summary
 * table per accounting mode, then the separately timed setup costs.
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

typedef struct {
	unsigned trials;
	unsigned sigs_per_key;
	unsigned logn_min;
	unsigned logn_max;
	int no_header;
} sign_bench_options;

/*
 * One benchmark block: a scheme at one degree and sampler thread count.
 */
typedef struct {
	unsigned scheme;
	unsigned logn;
	unsigned threads;
	unsigned sig_bytes;
	uint64_t *cycles[ACCT_COUNT];
	uint64_t *wall_ns[ACCT_COUNT];
	uint64_t signatures[ACCT_COUNT];
	uint64_t attempts[ACCT_COUNT];
	uint64_t sign_failures[ACCT_COUNT];
	uint64_t verify_failures[ACCT_COUNT];
	uint64_t *keygen_cycles;
	uint64_t *prepare_cycles;
	uint64_t keys;
} bench_block;

#define MAX_BLOCKS   9

static bench_block blocks[MAX_BLOCKS];
static unsigned block_count;

typedef struct {
	uint64_t state;
	unsigned calls;
} bench_rng_state;

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

static uint64_t
bench_wall_ns(void)
{
	struct timespec ts;

#if defined(CLOCK_MONOTONIC_RAW)
	if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) != 0) {
		return 0;
	}
#else
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
		return 0;
	}
#endif
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
		+ (uint64_t)ts.tv_nsec;
}

static uint64_t
bench_cycles_start(void)
{
#if defined(__x86_64__)
	_mm_lfence();
	return __rdtsc();
#else
	return 0;
#endif
}

static uint64_t
bench_cycles_end(void)
{
#if defined(__x86_64__)
	unsigned int aux;
	uint64_t x;

	x = __rdtscp(&aux);
	_mm_lfence();
	return x;
#else
	return 0;
#endif
}

static uint64_t
bench_delta(uint64_t t0, uint64_t t1)
{
	if (t0 == 0 && t1 == 0) {
		return 0;
	}
	if (t1 <= t0) {
		return 1;
	}
	return t1 - t0;
}

/*
 * Benchmark RNG (not cryptographic; both schemes hash its output with
 * SHAKE256). It counts its calls for the attempt counter.
 */
static void
bench_rng(void *ctx, void *dst, size_t len)
{
	bench_rng_state *rng = ctx;
	uint8_t *buf = dst;

	rng->calls ++;
	while (len > 0) {
		uint64_t x;

		rng->state = rng->state * UINT64_C(6364136223846793005)
			+ UINT64_C(1442695040888963407);
		x = rng->state;
		for (unsigned u = 0; u < 8 && len > 0; u ++, len --) {
			*buf ++ = (uint8_t)(x >> (u << 3));
		}
	}
}

static void
bench_rng_init(bench_rng_state *rng, unsigned stream_id,
	unsigned logn, unsigned index)
{
	uint64_t seed = UINT64_C(0xE850A11B6E4C0000);

	seed ^= (uint64_t)stream_id * UINT64_C(0x9E3779B97F4A7C15);
	seed ^= (uint64_t)logn << 48;
	seed ^= (uint64_t)index * UINT64_C(0xD1B54A32D192ED03);
	rng->state = seed;
	rng->calls = 0;
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
	uint64_t *cycles, uint64_t *wall_ns)
{
	bench_rng_state rng;
	uint64_t c0, c1, w0, w1;
	int ok;

	bench_rng_init(&rng, 1 + scheme, logn, key_index);
	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	if (scheme == SCHEME_HAWK) {
		ok = hawk_keygen(logn, key_priv, key_pub, bench_rng, &rng,
			tmp_keygen, sizeof tmp_keygen);
	} else {
		ok = e8_keygen(logn, key_priv, key_pub, bench_rng, &rng,
			tmp_keygen, sizeof tmp_keygen);
	}
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();
	*cycles = bench_delta(c0, c1);
	*wall_ns = bench_delta(w0, w1);
	return ok;
}

static int
bench_prepare(unsigned scheme, unsigned logn,
	uint64_t *cycles, uint64_t *wall_ns)
{
	uint64_t c0, c1, w0, w1;
	int ok;

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	if (scheme == SCHEME_HAWK) {
		hawk_decode_private_key(logn, key_prepared, key_priv);
		ok = 1;
	} else {
		ok = e8_prepare_private_key(logn, key_prepared, key_priv);
	}
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();
	*cycles = bench_delta(c0, c1);
	*wall_ns = bench_delta(w0, w1);
	return ok;
}

#if HAWK_E8_PROFILE_SIGN
static void profile_add(unsigned acct, unsigned logn, int accepted,
	unsigned attempts, const e8_sign_trace_timing *timing);
#endif

/*
 * One timed sign_finish call. Returns 1 on success and sets *attempts
 * from the RNG call count.
 */
static int
bench_sign(unsigned scheme, unsigned acct, unsigned logn,
	const shake_context *sc_data, bench_rng_state *rng,
	unsigned *attempts, uint64_t *cycles, uint64_t *wall_ns,
	int profile)
{
	uint64_t c0, c1, w0, w1;
	int ok;
#if HAWK_E8_PROFILE_SIGN
	e8_sign_trace_timing timing;
	unsigned trace_attempts = 0;
#else
	(void)profile;
#endif

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	if (scheme == SCHEME_HAWK) {
		if (acct == ACCT_ALL_IN) {
			ok = hawk_sign_finish(logn, bench_rng, rng, sig_buf,
				sc_data, key_priv, tmp_sign, sizeof tmp_sign);
		} else {
			ok = hawk_shim_sign_finish_decoded(logn, bench_rng, rng,
				sig_buf, sc_data, key_prepared,
				tmp_sign, sizeof tmp_sign);
		}
	} else {
#if HAWK_E8_PROFILE_SIGN
		/*
		 * Profile builds go through the same implementation with
		 * the per-stage trace enabled.
		 */
		if (acct == ACCT_ALL_IN) {
			ok = e8_sign_finish_inner(logn, bench_rng, rng, sig_buf,
				sc_data, key_priv, E8_PRIVKEY_SIZE(logn),
				tmp_sign, sizeof tmp_sign,
				&trace_attempts, &timing);
		} else {
			ok = e8_sign_finish_inner(logn, bench_rng, rng, sig_buf,
				sc_data, key_prepared,
				E8_PRIVKEY_PREPARED_SIZE(logn), NULL, 0,
				&trace_attempts, &timing);
		}
#else
		if (acct == ACCT_ALL_IN) {
			ok = e8_sign_finish(logn, bench_rng, rng, sig_buf,
				sc_data, key_priv, tmp_sign, sizeof tmp_sign);
		} else {
			ok = e8_sign_finish_prepared(logn, bench_rng, rng,
				sig_buf, sc_data, key_prepared, NULL, 0);
		}
#endif
	}
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();
	*cycles = bench_delta(c0, c1);
	*wall_ns = bench_delta(w0, w1);
	*attempts = rng->calls / 2;
#if HAWK_E8_PROFILE_SIGN
	if (scheme == SCHEME_E8 && profile) {
		profile_add(acct, logn, ok, *attempts, &timing);
		if (trace_attempts != *attempts) {
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

static void
write_header(void)
{
	printf("kind,scheme,accounting,logn,trial,key_index,attempts,"
		"sig_bytes,sampler_threads,tsc_cycles,wall_ns,verify_ok\n");
}

static void
write_sign_row(const bench_block *b, unsigned acct, unsigned trial,
	unsigned key_index, int sign_ok, unsigned attempts,
	uint64_t cycles, uint64_t wall_ns, int verify_ok)
{
	printf("%s,%s,%s,%u,%u,%u,%u,%u,%u,%llu,%llu,%d\n",
		sign_ok ? "sign" : "sign_failed",
		SCHEME_NAME[b->scheme], ACCT_NAME[acct], b->logn,
		trial, key_index, attempts, b->sig_bytes, b->threads,
		(unsigned long long)cycles, (unsigned long long)wall_ns,
		verify_ok);
}

static void
write_setup_row(const bench_block *b, const char *kind,
	const char *acct, unsigned trial, unsigned key_index,
	uint64_t cycles, uint64_t wall_ns)
{
	printf("%s,%s,%s,%u,%u,%u,,,%u,%llu,%llu,\n",
		kind, SCHEME_NAME[b->scheme], acct, b->logn,
		trial, key_index, b->threads,
		(unsigned long long)cycles, (unsigned long long)wall_ns);
}

static int
block_alloc(bench_block *b, unsigned trials)
{
	size_t len = (size_t)trials * sizeof(uint64_t);

	for (unsigned a = 0; a < ACCT_COUNT; a ++) {
		b->cycles[a] = malloc(len);
		b->wall_ns[a] = malloc(len);
		if (b->cycles[a] == NULL || b->wall_ns[a] == NULL) {
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
	uint64_t cycles, wall_ns;
	shake_context sc_data;

	if (!bench_keygen(b->scheme, b->logn, 0xFFFFFFu, &cycles, &wall_ns)
		|| !bench_prepare(b->scheme, b->logn, &cycles, &wall_ns))
	{
		return 0;
	}
	bench_make_message(&sc_data, b->logn, 0xFFFFFFu);
	for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
		bench_rng_state rng;
		unsigned attempts;

		bench_rng_init(&rng, 9, b->logn, acct);
		if (!bench_sign(b->scheme, acct, b->logn, &sc_data, &rng,
			&attempts, &cycles, &wall_ns, 0))
		{
			return 0;
		}
	}
	return 1;
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
		b->sig_bytes = HAWK_SIG_SIZE(b->logn);
	}
	if (!bench_warmup(b)) {
		fprintf(stderr, "ERR: warm-up failed scheme=%s logn=%u\n",
			SCHEME_NAME[b->scheme], b->logn);
		return 0;
	}

	for (unsigned trial = 0; trial < opts->trials; trial ++) {
		shake_context sc_data;
		uint64_t cycles, wall_ns;

		if (trial % opts->sigs_per_key == 0) {
			key_index = trial / opts->sigs_per_key;
			if (!bench_keygen(b->scheme, b->logn, key_index,
				&cycles, &wall_ns))
			{
				fprintf(stderr, "ERR: keygen failed scheme=%s"
					" logn=%u key=%u\n",
					SCHEME_NAME[b->scheme], b->logn,
					key_index);
				return 0;
			}
			write_setup_row(b, "keygen", "", trial, key_index,
				cycles, wall_ns);
			b->keygen_cycles[b->keys] = cycles;
			if (!bench_prepare(b->scheme, b->logn,
				&cycles, &wall_ns))
			{
				fprintf(stderr, "ERR: key preparation failed"
					" scheme=%s logn=%u key=%u\n",
					SCHEME_NAME[b->scheme], b->logn,
					key_index);
				return 0;
			}
			write_setup_row(b, "prepare", ACCT_NAME[ACCT_PREPARED],
				trial, key_index, cycles, wall_ns);
			b->prepare_cycles[b->keys] = cycles;
			b->keys ++;
		}

		bench_make_message(&sc_data, b->logn, trial);
		for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
			bench_rng_state rng;
			unsigned attempts;
			int sign_ok, verify_ok = 0;

			bench_rng_init(&rng, 16 + acct, b->logn, trial);
			sign_ok = bench_sign(b->scheme, acct, b->logn,
				&sc_data, &rng, &attempts, &cycles, &wall_ns,
				profile);
			if (sign_ok) {
				verify_ok = bench_verify(b->scheme, b->logn,
					&sc_data);
				b->cycles[acct][b->signatures[acct]] = cycles;
				b->wall_ns[acct][b->signatures[acct]] = wall_ns;
				b->signatures[acct] ++;
				b->attempts[acct] += attempts;
				if (!verify_ok) {
					b->verify_failures[acct] ++;
				}
			} else {
				b->sign_failures[acct] ++;
			}
			write_sign_row(b, acct, trial, key_index, sign_ok,
				attempts, cycles, wall_ns, verify_ok);
		}
	}
	if (b->scheme == SCHEME_E8) {
		e8_sampler_set_thread_count(1);
	}
	return 1;
}

static int
cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a;
	uint64_t y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

static uint64_t
median_u64(uint64_t *v, uint64_t count)
{
	if (count == 0) {
		return 0;
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

static void
print_summary(const sign_bench_options *opts)
{
	for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
		fprintf(stderr,
			"\nHAWK_E8_SIGN_BENCH accounting=%s trials=%u"
			" sigs_per_key=%u\n"
			"scheme,logn,threads,signatures,median_cycles,"
			"mean_cycles,median_wall_ns,attempts_per_sign,"
			"sig_bytes,sign_failures,verify_failures\n",
			ACCT_NAME[acct], opts->trials, opts->sigs_per_key);
		for (unsigned u = 0; u < block_count; u ++) {
			bench_block *b = &blocks[u];
			uint64_t count = b->signatures[acct];
			double mean = mean_u64(b->cycles[acct], count);

			fprintf(stderr, "%s,%u,%u,%llu,%llu,%.0f,%llu,%.4f,"
				"%u,%llu,%llu\n",
				SCHEME_NAME[b->scheme], b->logn, b->threads,
				(unsigned long long)count,
				(unsigned long long)median_u64(
					b->cycles[acct], count),
				mean,
				(unsigned long long)median_u64(
					b->wall_ns[acct], count),
				count == 0 ? 0.0 : (double)b->attempts[acct]
					/ (double)count,
				b->sig_bytes,
				(unsigned long long)b->sign_failures[acct],
				(unsigned long long)b->verify_failures[acct]);
		}
	}
	fprintf(stderr,
		"\nHAWK_E8_SIGN_BENCH setup (timed separately, not included"
		" in any signing figure)\n"
		"scheme,logn,threads,keys,median_keygen_cycles,"
		"median_prepare_cycles\n");
	for (unsigned u = 0; u < block_count; u ++) {
		bench_block *b = &blocks[u];

		fprintf(stderr, "%s,%u,%u,%llu,%llu,%llu\n",
			SCHEME_NAME[b->scheme], b->logn, b->threads,
			(unsigned long long)b->keys,
			(unsigned long long)median_u64(b->keygen_cycles, b->keys),
			(unsigned long long)median_u64(b->prepare_cycles,
				b->keys));
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

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--trials N] [--sigs-per-key N] [--logn 8|9|10]"
		" [--no-header]\n"
		"  --trials N        signatures per scheme, accounting mode,"
		" degree and\n"
		"                    thread count (default %u, or"
		" E8_SIGN_BENCH_TRIALS)\n"
		"  --sigs-per-key N  signatures sharing one key (default %u);"
		" this does\n"
		"                    not change the accounting\n",
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
	if (!opts.no_header) {
		write_header();
	}
	for (unsigned logn = opts.logn_min; logn <= opts.logn_max; logn ++) {
		static const unsigned SCHEMES[] = {
			SCHEME_HAWK, SCHEME_E8, SCHEME_E8
		};

		for (unsigned u = 0; u < 3; u ++) {
			bench_block *b = &blocks[block_count ++];

			b->scheme = SCHEMES[u];
			b->logn = logn;
			b->threads = u == 2 ? bench_selected_e8_threads(logn) : 1;
			if (!run_block(b, &opts, u == 2)) {
				return 1;
			}
			for (unsigned acct = 0; acct < ACCT_COUNT; acct ++) {
				failures += b->sign_failures[acct]
					+ b->verify_failures[acct];
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
