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

#include "../hawk_e8_inner.h"

/*
 * Pull in the HAWK signing implementation and the E8
 * signer/verifier for this standalone full signature benchmark.
 */
#include "../hawk_sign.c"
#define get_bit e8_vrfy_bench_get_bit
#define hash_to_h e8_vrfy_bench_hash_to_h
#include "../e8_vrfy.c"
#undef get_bit
#undef hash_to_h
#include "../e8_sign.c"

#define MAXN                         1024
#define DEFAULT_SIGN_BENCH_TRIALS    16
#define MAX_BENCH_TRIALS             1000000

#if HAWK_E8_PROFILE_SIGN
typedef struct {
	uint64_t trials;
	uint64_t accepted;
	uint64_t failed;
	uint64_t attempts;
	uint64_t rejections;
	uint64_t cycles_total;
	uint64_t cycles_hash;
	uint64_t cycles_target;
	uint64_t cycles_sample;
	uint64_t cycles_reconstruct;
	uint64_t cycles_norm_check;
	uint64_t cycles_encode;
	uint64_t wall_ns_total;
	uint64_t wall_ns_hash;
	uint64_t wall_ns_target;
	uint64_t wall_ns_sample;
	uint64_t wall_ns_reconstruct;
	uint64_t wall_ns_norm_check;
	uint64_t wall_ns_encode;
} bench_sign_profile_totals;

static bench_sign_profile_totals sign_profile_totals[11];
#endif

typedef struct {
	uint64_t state;
} bench_rng_state;

typedef struct {
	unsigned trials;
	int no_header;
} sign_bench_options;

typedef struct {
	const char *label;
	unsigned threads;
} bench_thread_case;

typedef struct {
	const char *label;
	unsigned mode;
} bench_mode_case;

static const bench_thread_case E8_THREAD_CASES[] = {
	{ "1", 1 },
	{ "2", 2 },
	{ "4", 4 },
	{ "8", 8 },
	{ "12", 12 },
	{ "16", 16 },
	{ "24", 24 }
};

static const bench_mode_case E8_RNG_MODE_CASES[] = {
	{ "per_block", E8_SAMPLER_RNG_PER_BLOCK },
	{ "per_worker", E8_SAMPLER_RNG_PER_WORKER }
};

static int
bench_selected_e8_sampler_config(unsigned logn,
	unsigned *threads, unsigned *rng_mode)
{
	if (threads == NULL || rng_mode == NULL) {
		return 0;
	}
	switch (logn) {
	case 8:
		*threads = 8;
		*rng_mode = E8_SAMPLER_RNG_PER_WORKER;
		return 1;
	case 9:
		*threads = 8;
		*rng_mode = E8_SAMPLER_RNG_PER_WORKER;
		return 1;
	case 10:
		*threads = 16;
		*rng_mode = E8_SAMPLER_RNG_PER_WORKER;
		return 1;
	default:
		return 0;
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
bench_cycles_delta(uint64_t t0, uint64_t t1)
{
	if (t0 == 0 && t1 == 0) {
		return 0;
	}
	if (t1 <= t0) {
		return 1;
	}
	return t1 - t0;
}

static uint64_t
bench_wall_delta(uint64_t t0, uint64_t t1)
{
	if (t0 == 0 && t1 == 0) {
		return 0;
	}
	if (t1 <= t0) {
		return 1;
	}
	return t1 - t0;
}

static uint64_t
bench_rng_next(bench_rng_state *rng)
{
	rng->state = rng->state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return rng->state;
}

static void
bench_rng(void *ctx, void *dst, size_t len)
{
	bench_rng_state *rng = ctx;
	uint8_t *buf = dst;

	while (len > 0) {
		uint64_t x = bench_rng_next(rng);

		for (unsigned u = 0; u < 8 && len > 0; u ++, len --) {
			*buf ++ = (uint8_t)(x >> (u << 3));
		}
	}
}

static void
bench_rng_init(bench_rng_state *rng,
	unsigned sampler_id, unsigned logn, unsigned trial_index)
{
	uint64_t seed = UINT64_C(0xE850A11B6E4C0000);

	seed ^= (uint64_t)sampler_id * UINT64_C(0x9E3779B97F4A7C15);
	seed ^= (uint64_t)logn << 48;
	seed ^= (uint64_t)trial_index * UINT64_C(0xD1B54A32D192ED03);
	rng->state = seed;
	(void)bench_rng_next(rng);
}

static double
hawk_sigma_sign(unsigned logn)
{
	switch (logn) {
	case 8: return 1.010;
	case 9: return 1.278;
	default: return 1.299;
	}
}

static double
e8_sigma_sign(unsigned logn)
{
	switch (logn) {
	case 8: return 3.592;
	case 9: return 3.631;
	default: return 3.669;
	}
}

static double
e8_sigma_verify_sign(unsigned logn)
{
	switch (logn) {
	case 8: return 2.03;
	case 9: return 1.99;
	default: return 1.95;
	}
}

static unsigned
parse_bench_count_env(const char *name, unsigned fallback,
	unsigned min_value, unsigned max_value)
{
	const char *env = getenv(name);
	char *end = NULL;
	unsigned long x;

	if (env == NULL || env[0] == 0) {
		return fallback;
	}
	x = strtoul(env, &end, 10);
	if (end == env || *end != 0 || x < min_value || x > max_value) {
		fprintf(stderr,
			"ERR: %s must be in [%u,%u]\n",
			name, min_value, max_value);
		return 0;
	}
	return (unsigned)x;
}

static unsigned
get_trials(void)
{
	return parse_bench_count_env("E8_SIGN_BENCH_TRIALS",
		DEFAULT_SIGN_BENCH_TRIALS, 1, MAX_BENCH_TRIALS);
}

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

static const char *
thread_label(unsigned threads, char *buf, size_t buf_len)
{
	for (size_t u = 0;
		u < sizeof E8_THREAD_CASES / sizeof E8_THREAD_CASES[0];
		u ++)
	{
		if (E8_THREAD_CASES[u].threads == threads) {
			return E8_THREAD_CASES[u].label;
		}
	}
	if (buf_len != 0) {
		snprintf(buf, buf_len, "%u", threads);
	}
	return buf;
}

static const char *
rng_mode_label(unsigned mode)
{
	for (size_t u = 0;
		u < sizeof E8_RNG_MODE_CASES / sizeof E8_RNG_MODE_CASES[0];
		u ++)
	{
		if (E8_RNG_MODE_CASES[u].mode == mode) {
			return E8_RNG_MODE_CASES[u].label;
		}
	}
	return "unknown";
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--trials N] [--no-header]\n",
		prog);
}

static int
parse_options(int argc, char **argv, sign_bench_options *opts)
{
	memset(opts, 0, sizeof *opts);
	opts->trials = get_trials();
	if (opts->trials == 0) {
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
		} else {
			fprintf(stderr, "ERR: unknown argument: %s\n", arg);
			return 0;
		}
	}
	return opts->trials != 0;
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
profile_add(unsigned logn, int accepted, unsigned attempts,
	const e8_sign_trace_timing *timing)
{
	bench_sign_profile_totals *total;

	if (logn > 10 || timing == NULL) {
		return;
	}
	total = &sign_profile_totals[logn];
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
	total->cycles_hash += timing->cycles_hash_total;
	total->cycles_target += timing->cycles_target_total;
	total->cycles_sample += timing->cycles_sample_total;
	total->cycles_reconstruct += timing->cycles_reconstruct_total;
	total->cycles_norm_check += timing->cycles_norm_check_total;
	total->cycles_encode += timing->cycles_encode_total;
	total->wall_ns_total += timing->wall_ns_sign_total;
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
	fprintf(stderr,
		"\nHAWK_E8_PROFILE_SIGN summary "
		"(e8_sign_sampler_trace_timed_uncompressed)\n");
	for (unsigned logn = 8; logn <= 10; logn ++) {
		const bench_sign_profile_totals *total =
			&sign_profile_totals[logn];
		uint64_t stage_cycles, stage_wall_ns;
		uint64_t other_cycles = 0, other_wall_ns = 0;

		if (total->trials == 0) {
			continue;
		}
		stage_cycles = total->cycles_hash
			+ total->cycles_target
			+ total->cycles_sample
			+ total->cycles_reconstruct
			+ total->cycles_norm_check
			+ total->cycles_encode;
		stage_wall_ns = total->wall_ns_hash
			+ total->wall_ns_target
			+ total->wall_ns_sample
			+ total->wall_ns_reconstruct
			+ total->wall_ns_norm_check
			+ total->wall_ns_encode;
		if (total->cycles_total > stage_cycles) {
			other_cycles = total->cycles_total - stage_cycles;
		}
		if (total->wall_ns_total > stage_wall_ns) {
			other_wall_ns = total->wall_ns_total - stage_wall_ns;
		}

		fprintf(stderr,
			"logn=%u n=%u trials=%llu accepted=%llu failed=%llu "
			"attempts=%llu rejections=%llu\n",
			logn, 1u << logn,
			(unsigned long long)total->trials,
			(unsigned long long)total->accepted,
			(unsigned long long)total->failed,
			(unsigned long long)total->attempts,
			(unsigned long long)total->rejections);
		fprintf(stderr,
			"  total cycles=%llu wall_ns=%llu "
			"avg_cycles/sign=%llu avg_wall_ns/sign=%llu\n",
			(unsigned long long)total->cycles_total,
			(unsigned long long)total->wall_ns_total,
			(unsigned long long)profile_avg(total->cycles_total,
				total->trials),
			(unsigned long long)profile_avg(total->wall_ns_total,
				total->trials));
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
			total->wall_ns_reconstruct, total->wall_ns_total);
		profile_print_stage("norm_rejection",
			total->cycles_norm_check, total->cycles_total,
			total->wall_ns_norm_check, total->wall_ns_total);
		profile_print_stage("encode_sig",
			total->cycles_encode, total->cycles_total,
			total->wall_ns_encode, total->wall_ns_total);
		profile_print_stage("other",
			other_cycles, total->cycles_total,
			other_wall_ns, total->wall_ns_total);
	}
}
#endif

static void
write_header(void)
{
	printf("sampler_type,scope,logn,n,trial_index,sigma,"
		"accepted,attempts,cycles_total,"
		"cycles_per_unit,wall_ns_total,"
		"wall_ns_per_unit,threads_requested,threads_used,"
		"worker_mode,rng_mode,speedup_vs_threads_1,"
		"profile_master_seed_cycles,profile_block_rng_init_cycles,"
		"profile_block_sample_cycles,profile_worker_dispatch_cycles,"
		"profile_worker_wait_cycles,profile_reduction_cycles,"
		"profile_master_seed_wall_ns,"
		"profile_block_rng_init_wall_ns,profile_block_sample_wall_ns,"
		"profile_worker_dispatch_wall_ns,profile_worker_wait_wall_ns,"
		"profile_reduction_wall_ns,notes\n");
}

static void
write_row_threads(const char *sampler_type, const char *scope,
	unsigned logn, unsigned trial_index,
	double sigma, int accepted, unsigned attempts,
	uint64_t cycles_total,
	uint64_t cycles_per_signature, uint64_t wall_ns_total,
	uint64_t wall_ns_per_signature,
	const char *threads_requested, unsigned threads_used,
	const char *worker_mode, const char *rng_mode,
	const char *notes)
{
	size_t n = (size_t)1 << logn;

	printf("%s,%s,%u,%u,%u,%.17g,%d,%u,"
		"%llu,%llu,%llu,%llu,%s,%u,%s,%s,0.000000,"
		"0,0,0,0,0,0,0,0,0,0,0,0,%s\n",
		sampler_type, scope, logn, (unsigned)n,
		trial_index, sigma, accepted, attempts,
		(unsigned long long)cycles_total,
		(unsigned long long)cycles_per_signature,
		(unsigned long long)wall_ns_total,
		(unsigned long long)wall_ns_per_signature,
		threads_requested != NULL ? threads_requested : "",
		threads_used,
		worker_mode != NULL ? worker_mode : "",
		rng_mode != NULL ? rng_mode : "",
		notes);
}

static void
write_row(const char *sampler_type, const char *scope,
	unsigned logn, unsigned trial_index,
	double sigma, int accepted, unsigned attempts,
	uint64_t cycles_total,
	uint64_t cycles_per_signature, uint64_t wall_ns_total,
	uint64_t wall_ns_per_signature,
	const char *notes)
{
	write_row_threads(sampler_type, scope, logn,
		trial_index, sigma, accepted, attempts,
		cycles_total, cycles_per_signature, wall_ns_total,
		wall_ns_per_signature, "", 0, "", "", notes);
}

static void
bench_make_hawk_message_context(shake_context *sc_data,
	unsigned logn, unsigned trial_index)
{
	static const char prefix[] = "ordinary hawk signing bench";
	uint8_t buf[2];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)trial_index;
	hawk_sign_start(sc_data);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

static void
bench_hawk_sign(unsigned logn, unsigned trial_index)
{
	uint8_t priv[HAWK_PRIVKEY_SIZE(10)];
	uint8_t pub[HAWK_PUBKEY_SIZE(10)];
	uint8_t sig[HAWK_SIG_SIZE(10)];
	uint8_t tmp_keygen[HAWK_TMPSIZE_KEYGEN(10)];
	uint8_t tmp_sign[HAWK_TMPSIZE_SIGN(10)];
	shake_context sc_data;
	bench_rng_state key_rng, sign_rng;
	uint64_t c0, c1, w0, w1, cycles_total, wall_ns_total;
	int accepted;

	bench_rng_init(&key_rng, 5, logn, trial_index);
	bench_rng_init(&sign_rng, 6, logn, trial_index);
	bench_make_hawk_message_context(&sc_data, logn, trial_index);
	memset(sig, 0, sizeof sig);

	if (!hawk_keygen(logn, priv, pub, bench_rng, &key_rng,
		tmp_keygen, HAWK_TMPSIZE_KEYGEN(logn)))
	{
		write_row("hawk_sign", "signature", logn, trial_index,
			hawk_sigma_sign(logn), 0, 0, 0, 0, 0, 0,
			"keygen_setup_failed");
		return;
	}

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	accepted = hawk_sign_finish(logn, bench_rng, &sign_rng,
		sig, &sc_data, priv, tmp_sign, HAWK_TMPSIZE_SIGN(logn));
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();

	cycles_total = bench_cycles_delta(c0, c1);
	wall_ns_total = bench_wall_delta(w0, w1);
	write_row("hawk_sign", "signature", logn, trial_index,
		hawk_sigma_sign(logn), accepted, 1, cycles_total,
		accepted ? cycles_total : 0, wall_ns_total,
		accepted ? wall_ns_total : 0,
		"ordinary_hawk_sign_finish_encoded_private_key");
}

static void
bench_make_message_context(shake_context *sc_data,
	unsigned logn, unsigned trial_index)
{
	static const char prefix[] = "experimental e8 sign bench";
	uint8_t buf[2];

	buf[0] = (uint8_t)logn;
	buf[1] = (uint8_t)trial_index;
	shake_init(sc_data, 256);
	shake_inject(sc_data, prefix, sizeof prefix - 1);
	shake_inject(sc_data, buf, sizeof buf);
}

static void
bench_make_hpub(uint8_t *hpub, size_t hpub_len,
	unsigned logn, unsigned trial_index)
{
	for (size_t u = 0; u < hpub_len; u ++) {
		hpub[u] = (uint8_t)(0x63u + 17u * u
			+ 11u * trial_index + 5u * logn);
	}
}

static void
bench_make_salt(uint8_t *salt, size_t salt_len,
	unsigned logn, unsigned trial_index)
{
	for (size_t u = 0; u < salt_len; u ++) {
		salt[u] = (uint8_t)(0xB5u + 29u * u
			+ trial_index + 3u * logn);
	}
}

static int
bench_make_basis(unsigned logn,
	int8_t *f, int8_t *g, int8_t *F, int8_t *G,
	bench_rng_state *rng)
{
	uint8_t tmp[HAWK_TMPSIZE_KEYGEN(10)];
	uint8_t seed[40];

	return Hawk_keygen(logn, f, g, F, G, NULL, NULL, NULL, seed,
		bench_rng, rng, tmp, sizeof tmp) == 0;
}

static int
bench_e8_sampler_warmup(unsigned logn, unsigned trial_index,
	unsigned requested_threads, unsigned rng_mode)
{
	size_t n = (size_t)1 << logn;
	uint8_t t0[MAXN], t1[MAXN];
	int32_t z0[MAXN], z1[MAXN];
	uint64_t norm = 0;
	e8_sampler_stats stats;
	bench_rng_state rng;

	memset(&stats, 0, sizeof stats);
	memset(z0, 0, sizeof z0);
	memset(z1, 0, sizeof z1);
	for (size_t r = 0, k = n >> 2; r < k; r ++) {
		uint8_t tau = (uint8_t)(0x5Bu + 37u * trial_index
			+ 19u * logn + 11u * r);
		for (unsigned u = 0; u < 4; u ++) {
			t0[r + u * k] = (uint8_t)((tau >> u) & 1u);
			t1[r + u * k] = (uint8_t)((tau >> (u + 4)) & 1u);
		}
	}
	bench_rng_init(&rng, 7, logn, trial_index);
	e8_sampler_set_thread_count(requested_threads);
	e8_sampler_set_rng_mode(rng_mode);
	return e8_sample_z_construction_a_cm(z0, z1, &norm,
		t0, t1, logn, e8_sigma_sign(logn), bench_rng, &rng,
		&stats) && n != 0;
}

static void
bench_e8_sign_sampler(unsigned logn, unsigned trial_index)
{
	size_t sig_len = e8_sig_uncompressed_size(logn);
	size_t salt_len = e8_salt_len(logn);
	size_t hpub_len = (size_t)1 << (logn - 4);
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	uint8_t hpub[64], salt[40], sig[40 + 4 * MAXN];
	shake_context sc_data;
	bench_rng_state key_rng, sign_rng;
	e8_sign_trace_timing timing;
	uint64_t c0, c1, w0, w1, cycles_total, wall_ns_total;
	unsigned attempts = 0;
	unsigned sampler_threads;
	unsigned sampler_rng_mode;
	unsigned threads_used = 0;
	char threads_buf[16];
	const char *threads_text;
	const char *rng_text;
	int accepted;

	bench_rng_init(&key_rng, 3, logn, trial_index);
	bench_rng_init(&sign_rng, 4, logn, trial_index);
	memset(&timing, 0, sizeof timing);
	memset(sig, 0, sizeof sig);
	bench_make_message_context(&sc_data, logn, trial_index);
	bench_make_hpub(hpub, hpub_len, logn, trial_index);
	bench_make_salt(salt, salt_len, logn, trial_index);
	if (!bench_selected_e8_sampler_config(logn,
			&sampler_threads, &sampler_rng_mode))
	{
		write_row("e8_sign_sampler_cached", "signature", logn,
			trial_index, e8_sigma_sign(logn), 0, 0, 0, 0, 0, 0,
			"unsupported_logn");
		return;
	}
	threads_text = thread_label(sampler_threads,
		threads_buf, sizeof threads_buf);
	rng_text = rng_mode_label(sampler_rng_mode);
	e8_sampler_set_thread_count(sampler_threads);
	e8_sampler_set_rng_mode(sampler_rng_mode);
	threads_used = e8_sampler_get_thread_count(logn);

	if (!bench_make_basis(logn, f, g, F, G, &key_rng)
		|| !e8_sampler_warm_cache(e8_sigma_sign(logn))
		|| !bench_e8_sampler_warmup(logn,
			0xB0000000u + trial_index,
			sampler_threads, sampler_rng_mode))
	{
		write_row_threads("e8_sign_sampler_cached", "signature", logn,
			trial_index, e8_sigma_sign(logn), 0, 0, 0, 0, 0, 0,
			threads_text, threads_used,
			threads_used == 1 ? "serial" : "spin", rng_text,
			"setup_failed");
		return;
	}

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	accepted = e8_sign_sampler_trace_timed_uncompressed(logn,
		sig, sig_len, &sc_data, hpub, hpub_len, f, g, F, G, salt,
		e8_sigma_sign(logn), e8_sigma_verify_sign(logn), 1000,
		bench_rng, &sign_rng, NULL, NULL, NULL, &attempts,
		&timing);
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();

	cycles_total = bench_cycles_delta(c0, c1);
	wall_ns_total = bench_wall_delta(w0, w1);
#if HAWK_E8_PROFILE_SIGN
	profile_add(logn, accepted, attempts, &timing);
#endif
	write_row_threads("e8_sign_sampler_cached", "signature", logn,
		trial_index, e8_sigma_sign(logn), accepted, attempts,
		cycles_total, accepted ? cycles_total : 0, wall_ns_total,
		accepted ? wall_ns_total : 0,
		threads_text, threads_used,
		threads_used == 1 ? "serial" : "spin", rng_text,
		"end_to_end_signing_warm_cache");
}

static int
run_sign_bench(const sign_bench_options *opts)
{
	if (!opts->no_header) {
		write_header();
	}
	for (unsigned logn = 8; logn <= 10; logn ++) {
		for (unsigned trial_index = 0;
			trial_index < opts->trials; trial_index ++)
		{
			bench_hawk_sign(logn, trial_index);
			bench_e8_sign_sampler(logn, trial_index);
		}
	}
#if HAWK_E8_PROFILE_SIGN
	profile_print_summary();
#endif
	return 0;
}

int
main(int argc, char **argv)
{
	sign_bench_options opts;

	if (!parse_options(argc, argv, &opts)) {
		usage(argv[0]);
		return 1;
	}
	return run_sign_bench(&opts);
}
