#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <limits.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <x86intrin.h>
#endif

#include "../hawk_e8_inner.h"

/*
 * Pull in the ordinary HAWK signing sampler for this standalone benchmark.
 * The sampler helper is static in hawk_sign.c; this mirrors test_sampler.c
 * and keeps the benchmark as a thin wrapper around the real implementation
 * instead of copying sampler tables or logic.
 */
#include "../hawk_sign.c"

/*
 * The sampler benchmark target intentionally links only the ordinary Hawk
 * objects plus e8_math/e8_sampler.  Include the experimental E8 verifier and
 * signer here so this standalone benchmark can also time the end-to-end
 * HAWK-E8-CM signing path without changing the reference Makefile.
 */
#define get_bit e8_vrfy_bench_get_bit
#define hash_to_h e8_vrfy_bench_hash_to_h
#include "../e8_vrfy.c"
#undef get_bit
#undef hash_to_h
#include "../e8_sign.c"

#define MAXN                         1024
#define DEFAULT_BENCH_TRIALS         16
#define DEFAULT_BENCH_WARMUPS        1
#define HAWK_BASELINE_BENCH_TRIALS   10000
#define MAX_BENCH_TRIALS             1000000
#define MAX_BENCH_LINE               4096

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

typedef enum {
	BENCH_MODE_ISOLATED_MATRIX = 0,
	BENCH_MODE_SINGLE_CONFIG = 1,
	BENCH_MODE_SINGLE_HAWK_SAMPLER = 2,
	BENCH_MODE_SIGN = 3,
	BENCH_MODE_SELECTED_CONFIGS = 4
} bench_run_mode;

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
		*threads = 12;
		*rng_mode = E8_SAMPLER_RNG_PER_WORKER;
		return 1;
	case 9:
		*threads = 16;
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

typedef struct {
	bench_run_mode mode;
	unsigned logn;
	unsigned threads;
	unsigned rng_mode;
	unsigned trials;
	unsigned warmups;
	int no_header;
	int sign_only;
	int worker_mode_set;
	int worker_mode_spin;
} bench_options;

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
bench_per_unit(uint64_t total, unsigned units)
{
	uint64_t q;

	if (units == 0 || total == 0) {
		return 0;
	}
	q = total / units;
	return q == 0 ? 1 : q;
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

static void
fill_hawk_parities(uint8_t *t, size_t t_len,
	unsigned logn, unsigned trial_index)
{
	for (size_t u = 0; u < t_len; u ++) {
		t[u] = (uint8_t)(0xA7u + 29u * u
			+ 17u * logn + 43u * trial_index);
	}
}

static uint8_t
make_tau(unsigned logn, unsigned trial_index, unsigned block_index)
{
	return (uint8_t)(0x5Bu + 37u * trial_index
		+ 19u * logn + 11u * block_index);
}

static void
fill_e8_parities(uint8_t *t0, uint8_t *t1,
	unsigned logn, unsigned trial_index)
{
	size_t n = (size_t)1 << logn;
	size_t k = n >> 2;

	for (size_t r = 0; r < k; r ++) {
		uint8_t tau = make_tau(logn, trial_index, (unsigned)r);

		for (unsigned u = 0; u < 4; u ++) {
			t0[r + u * k] = (uint8_t)((tau >> u) & 1u);
			t1[r + u * k] = (uint8_t)((tau >> (u + 4)) & 1u);
		}
	}
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
	case 8: return 1.26;
	case 9: return 1.278;
	default: return 1.299;
	}
}

static double
e8_sigma_verify_sign(unsigned logn)
{
	switch (logn) {
	case 8: return 0.73;
	case 9: return 0.72;
	default: return 0.71;
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
	return parse_bench_count_env("E8_SAMPLER_BENCH_TRIALS",
		DEFAULT_BENCH_TRIALS, 1, MAX_BENCH_TRIALS);
}

static unsigned
get_warmups(void)
{
	return parse_bench_count_env("E8_SAMPLER_BENCH_WARMUPS",
		DEFAULT_BENCH_WARMUPS, 0, MAX_BENCH_TRIALS);
}

static int
get_sign_only(void)
{
	const char *env = getenv("E8_SAMPLER_BENCH_SIGN_ONLY");

	return env != NULL && env[0] != 0 && strcmp(env, "0") != 0;
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

static int
parse_rng_mode(const char *text, unsigned *mode)
{
	if (text == NULL || mode == NULL) {
		return 0;
	}
	for (size_t u = 0;
		u < sizeof E8_RNG_MODE_CASES / sizeof E8_RNG_MODE_CASES[0];
		u ++)
	{
		if (strcmp(text, E8_RNG_MODE_CASES[u].label) == 0) {
			*mode = E8_RNG_MODE_CASES[u].mode;
			return 1;
		}
	}
	return 0;
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--selected-configs|--isolated-matrix]"
		" [--trials N] [--warmups N]\n"
		"       %s --single-config --logn N --threads N "
		"--worker-mode serial|spin --rng-mode per_block|per_worker "
		"[--trials N] [--warmups N]\n"
		"       %s --single-hawk-sampler --logn N [--trials N]\n",
		prog, prog, prog);
}

static int
parse_options(int argc, char **argv, bench_options *opts)
{
	memset(opts, 0, sizeof *opts);
	opts->mode = BENCH_MODE_SELECTED_CONFIGS;
	opts->logn = 8;
	opts->threads = 1;
	opts->rng_mode = E8_SAMPLER_RNG_PER_BLOCK;
	opts->trials = get_trials();
	opts->warmups = get_warmups();
	opts->sign_only = get_sign_only();
	if (opts->sign_only) {
		opts->mode = BENCH_MODE_SIGN;
	}
	if (opts->trials == 0) {
		return 0;
	}

	for (int i = 1; i < argc; i ++) {
		const char *arg = argv[i];

		if (strcmp(arg, "--help") == 0) {
			usage(argv[0]);
			exit(0);
		} else if (strcmp(arg, "--single-config") == 0) {
			opts->mode = BENCH_MODE_SINGLE_CONFIG;
		} else if (strcmp(arg, "--single-hawk-sampler") == 0) {
			opts->mode = BENCH_MODE_SINGLE_HAWK_SAMPLER;
		} else if (strcmp(arg, "--isolated-matrix") == 0) {
			opts->mode = BENCH_MODE_ISOLATED_MATRIX;
		} else if (strcmp(arg, "--selected-configs") == 0) {
			opts->mode = BENCH_MODE_SELECTED_CONFIGS;
		} else if (strcmp(arg, "--no-header") == 0) {
			opts->no_header = 1;
		} else if (strcmp(arg, "--logn") == 0) {
			if (++ i >= argc || !parse_unsigned_arg(argv[i],
				8, 10, &opts->logn))
			{
				fprintf(stderr, "ERR: invalid --logn\n");
				return 0;
			}
		} else if (strcmp(arg, "--threads") == 0) {
			if (++ i >= argc || !parse_unsigned_arg(argv[i],
				1, 24, &opts->threads))
			{
				fprintf(stderr, "ERR: invalid --threads\n");
				return 0;
			}
		} else if (strcmp(arg, "--rng-mode") == 0) {
			if (++ i >= argc || !parse_rng_mode(argv[i],
				&opts->rng_mode))
			{
				fprintf(stderr, "ERR: invalid --rng-mode\n");
				return 0;
			}
		} else if (strcmp(arg, "--worker-mode") == 0) {
			if (++ i >= argc) {
				fprintf(stderr, "ERR: missing --worker-mode\n");
				return 0;
			}
			opts->worker_mode_set = 1;
			if (strcmp(argv[i], "serial") == 0) {
				opts->worker_mode_spin = 0;
			} else if (strcmp(argv[i], "spin") == 0) {
				opts->worker_mode_spin = 1;
			} else {
				fprintf(stderr, "ERR: invalid --worker-mode\n");
				return 0;
			}
		} else if (strcmp(arg, "--trials") == 0) {
			if (++ i >= argc || !parse_unsigned_arg(argv[i],
				1, MAX_BENCH_TRIALS, &opts->trials))
			{
				fprintf(stderr, "ERR: invalid --trials\n");
				return 0;
			}
		} else if (strcmp(arg, "--warmups") == 0) {
			if (++ i >= argc || !parse_unsigned_arg(argv[i],
				0, MAX_BENCH_TRIALS, &opts->warmups))
			{
				fprintf(stderr, "ERR: invalid --warmups\n");
				return 0;
			}
		} else {
			fprintf(stderr, "ERR: unknown argument: %s\n", arg);
			return 0;
		}
	}

	if (opts->mode == BENCH_MODE_SINGLE_CONFIG) {
		int spin = opts->threads > 1;

		if (opts->worker_mode_set && opts->worker_mode_spin != spin) {
			fprintf(stderr,
				"ERR: --worker-mode must be serial for threads=1"
				" and spin for threads>1\n");
			return 0;
		}
	}
	if (opts->mode != BENCH_MODE_SIGN && opts->sign_only) {
		fprintf(stderr,
			"ERR: sign-only mode is only supported by the"
			" sign benchmark\n");
		return 0;
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
	uint64_t cycles_per_accepted_block, uint64_t wall_ns_total,
	uint64_t wall_ns_per_accepted_block,
	const char *threads_requested, unsigned threads_used,
	const char *worker_mode, const char *rng_mode,
	double speedup_vs_threads_1, const e8_sampler_profile *profile,
	const char *notes)
{
	size_t n = (size_t)1 << logn;
	e8_sampler_profile empty_profile;

	if (profile == NULL) {
		memset(&empty_profile, 0, sizeof empty_profile);
		profile = &empty_profile;
	}

	printf("%s,%s,%u,%u,%u,%.17g,%d,%u,"
		"%llu,%llu,%llu,%llu,%s,%u,%s,%s,%.6f,"
		"%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%s\n",
		sampler_type, scope, logn, (unsigned)n,
		trial_index, sigma, accepted, attempts,
		(unsigned long long)cycles_total,
		(unsigned long long)cycles_per_accepted_block,
		(unsigned long long)wall_ns_total,
		(unsigned long long)wall_ns_per_accepted_block,
		threads_requested != NULL ? threads_requested : "",
		threads_used,
		worker_mode != NULL ? worker_mode : "",
		rng_mode != NULL ? rng_mode : "",
		speedup_vs_threads_1,
		(unsigned long long)profile->cycles_master_seed,
		(unsigned long long)profile->cycles_block_rng_init,
		(unsigned long long)profile->cycles_block_sample,
		(unsigned long long)profile->cycles_worker_dispatch,
		(unsigned long long)profile->cycles_worker_wait,
		(unsigned long long)profile->cycles_reduction,
		(unsigned long long)profile->wall_ns_master_seed,
		(unsigned long long)profile->wall_ns_block_rng_init,
		(unsigned long long)profile->wall_ns_block_sample,
		(unsigned long long)profile->wall_ns_worker_dispatch,
		(unsigned long long)profile->wall_ns_worker_wait,
		(unsigned long long)profile->wall_ns_reduction,
		notes);
}

static void
write_row(const char *sampler_type, const char *scope,
	unsigned logn, unsigned trial_index,
	double sigma, int accepted, unsigned attempts,
	uint64_t cycles_total,
	uint64_t cycles_per_accepted_block, uint64_t wall_ns_total,
	uint64_t wall_ns_per_accepted_block,
	const char *notes)
{
	write_row_threads(sampler_type, scope, logn,
		trial_index, sigma, accepted, attempts,
		cycles_total, cycles_per_accepted_block, wall_ns_total,
		wall_ns_per_accepted_block, "", 0, "", "", 0.0, NULL,
		notes);
}

static void
bench_hawk_sampler(unsigned logn, unsigned trial_index)
{
	size_t n = (size_t)1 << logn;
	size_t t_len = n >> 2;
	unsigned block_units = (unsigned)(n >> 2);
	uint8_t t[MAXN >> 2];
	int8_t x[2 * MAXN];
	bench_rng_state rng;
	uint64_t c0, c1, w0, w1, cycles_total, wall_ns_total;

	memset(x, 0, sizeof x);
	fill_hawk_parities(t, t_len, logn, trial_index);
	bench_rng_init(&rng, 0, logn, trial_index);

	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	(void)sig_gauss(logn, bench_rng, &rng, NULL, x, t);
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();

	cycles_total = bench_cycles_delta(c0, c1);
	wall_ns_total = bench_wall_delta(w0, w1);

	write_row("hawk_sampler", "amortized_block", logn, trial_index,
		hawk_sigma_sign(logn), 1, 1, cycles_total,
		bench_per_unit(cycles_total, block_units), wall_ns_total,
		bench_per_unit(wall_ns_total, block_units),
		"sig_gauss_2n_samples_amortized_to_8_scalar_unit");
}

static int
run_single_hawk_sampler(const bench_options *opts)
{
	if (!opts->no_header) {
		write_header();
	}
	for (unsigned trial_index = 0;
		trial_index < opts->trials; trial_index ++)
	{
		bench_hawk_sampler(opts->logn, trial_index);
	}
	return 0;
}

static uint64_t
bench_e8_sampler_full(unsigned logn, unsigned trial_index,
	int warm_cache, unsigned requested_threads,
	const char *threads_requested,
	unsigned rng_mode, const char *rng_mode_label,
	uint64_t baseline_cycles,
	const char *sampler_type, const char *notes)
{
	size_t n = (size_t)1 << logn;
	unsigned block_units = (unsigned)(n >> 2);
	uint8_t t0[MAXN], t1[MAXN];
	int32_t z0[MAXN], z1[MAXN];
	uint64_t norm = 0;
	e8_sampler_stats stats;
	bench_rng_state rng;
	uint64_t c0, c1, w0, w1, cycles_total, wall_ns_total;
	int accepted;
	unsigned threads_used;
	double speedup = 0.0;

	memset(&stats, 0, sizeof stats);
	memset(z0, 0, sizeof z0);
	memset(z1, 0, sizeof z1);
	fill_e8_parities(t0, t1, logn, trial_index);
	bench_rng_init(&rng, 1, logn, trial_index);
	e8_sampler_set_thread_count(requested_threads);
	e8_sampler_set_rng_mode(rng_mode);
	threads_used = e8_sampler_get_thread_count(logn);
	if (warm_cache && !e8_sampler_warm_cache(e8_sigma_sign(logn))) {
		write_row_threads(sampler_type, "amortized_block",
			logn, trial_index, e8_sigma_sign(logn), 0, 0,
			0, 0, 0, 0, threads_requested, threads_used,
			threads_used == 1 ? "serial" : "spin", rng_mode_label,
			0.0, NULL, "cache_warmup_failed");
		return 0;
	}

	e8_sampler_profile_reset();
	c0 = bench_cycles_start();
	w0 = bench_wall_ns();
	accepted = e8_sample_z_construction_a_cm(z0, z1, &norm,
		t0, t1, logn, e8_sigma_sign(logn), bench_rng, &rng,
		&stats);
	w1 = bench_wall_ns();
	c1 = bench_cycles_end();
	e8_sampler_profile profile;
	if (!e8_sampler_profile_get(&profile)) {
		memset(&profile, 0, sizeof profile);
	}

	cycles_total = bench_cycles_delta(c0, c1);
	wall_ns_total = bench_wall_delta(w0, w1);
	if (accepted) {
		if (baseline_cycles != 0 && cycles_total != 0) {
			speedup = (double)baseline_cycles / (double)cycles_total;
		} else if (requested_threads == 1) {
			speedup = 1.0;
		}
	}
	write_row_threads(sampler_type, "amortized_block", logn,
		trial_index, e8_sigma_sign(logn), accepted, 1,
		cycles_total, accepted ? bench_per_unit(cycles_total,
			block_units) : 0, wall_ns_total,
		accepted ? bench_per_unit(wall_ns_total, block_units) : 0,
		threads_requested, threads_used,
		threads_used == 1 ? "serial" : "spin", rng_mode_label,
		speedup, accepted ? &profile : NULL, notes);
	return accepted ? cycles_total : 0;
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
	fill_e8_parities(t0, t1, logn, trial_index);
	bench_rng_init(&rng, 7, logn, trial_index);
	e8_sampler_set_thread_count(requested_threads);
	e8_sampler_set_rng_mode(rng_mode);
	return e8_sample_z_construction_a_cm(z0, z1, &norm,
		t0, t1, logn, e8_sigma_sign(logn), bench_rng, &rng,
		&stats) && n != 0;
}

static int
run_single_config(const bench_options *opts)
{
	char threads_buf[16];
	const char *threads_text = thread_label(opts->threads,
		threads_buf, sizeof threads_buf);
	const char *rng_text = rng_mode_label(opts->rng_mode);
	unsigned warmups = opts->warmups;

	if (!e8_sampler_warm_cache(e8_sigma_sign(opts->logn))) {
		fprintf(stderr, "ERR: E8 sampler cache warm-up failed\n");
		return 1;
	}
	if (opts->threads > 1 && warmups == 0) {
		warmups = 1;
	}
	for (unsigned u = 0; u < warmups; u ++) {
		if (!bench_e8_sampler_warmup(opts->logn,
			0xA0000000u + u, opts->threads, opts->rng_mode))
		{
			fprintf(stderr,
				"ERR: E8 sampler warm-up failed"
				" logn=%u threads=%u rng=%s\n",
				opts->logn, opts->threads, rng_text);
			return 1;
		}
	}
	if (!opts->no_header) {
		write_header();
	}
	for (unsigned trial_index = 0;
		trial_index < opts->trials; trial_index ++)
	{
		if (bench_e8_sampler_full(opts->logn, trial_index, 0,
			opts->threads, threads_text, opts->rng_mode, rng_text,
			0, "e8_sampler_cached_warm_block",
			"isolated_single_configuration") == 0)
		{
			return 1;
		}
	}
	e8_sampler_set_thread_count(1);
	e8_sampler_set_rng_mode(E8_SAMPLER_RNG_PER_BLOCK);
	return 0;
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
	static const char prefix[] = "experimental e8 sampler bench";
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
			threads_used == 1 ? "serial" : "spin", rng_text, 0.0,
			NULL,
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
		threads_used == 1 ? "serial" : "spin", rng_text, 0.0, NULL,
		"end_to_end_signing_warm_cache");
}

static size_t
split_csv_simple(char *line, char **fields, size_t max_fields)
{
	size_t count = 0;

	if (line == NULL || fields == NULL || max_fields == 0) {
		return 0;
	}
	fields[count ++] = line;
	for (char *p = line; *p != 0; p ++) {
		if (*p == ',') {
			*p = 0;
			if (count < max_fields) {
				fields[count ++] = p + 1;
			}
		}
	}
	return count;
}

static void
print_csv_fields(char **fields, size_t count, size_t replace_index,
	const char *replace_value)
{
	for (size_t u = 0; u < count; u ++) {
		if (u != 0) {
			putchar(',');
		}
		fputs(u == replace_index ? replace_value : fields[u], stdout);
	}
	putchar('\n');
}

static int
emit_child_csv_line(const char *line_in, uint64_t *baseline_cycles,
	unsigned trials, int store_baseline)
{
	char line[MAX_BENCH_LINE];
	char *fields[48];
	size_t count;
	unsigned trial_index = 0;
	uint64_t cycles = 0;
	char speedup[64];

	if (line_in == NULL || line_in[0] == 0) {
		return 1;
	}
	if (strncmp(line_in, "sampler_type,", 13) == 0) {
		return 1;
	}
	snprintf(line, sizeof line, "%s", line_in);
	line[strcspn(line, "\r\n")] = 0;
	count = split_csv_simple(line, fields,
		sizeof fields / sizeof fields[0]);
	if (count < 30) {
		fprintf(stderr, "ERR: child emitted malformed CSV row\n");
		return 0;
	}

	if (!parse_unsigned_arg(fields[4], 0, MAX_BENCH_TRIALS,
		&trial_index))
	{
		fprintf(stderr, "ERR: child emitted invalid trial index\n");
		return 0;
	}
	cycles = strtoull(fields[8], NULL, 10);
	if (trial_index >= trials) {
		fprintf(stderr, "ERR: child trial index out of range\n");
		return 0;
	}
	if (store_baseline && baseline_cycles != NULL) {
		baseline_cycles[trial_index] = cycles;
	}
	if (store_baseline) {
		snprintf(speedup, sizeof speedup, "%.6f", 1.0);
	} else if (baseline_cycles != NULL
		&& baseline_cycles[trial_index] != 0 && cycles != 0)
	{
		snprintf(speedup, sizeof speedup, "%.6f",
			(double)baseline_cycles[trial_index] / (double)cycles);
	} else {
		snprintf(speedup, sizeof speedup, "%s", fields[16]);
	}
	print_csv_fields(fields, count, 16, speedup);
	return 1;
}

static int
run_child_config(const char *self_path, unsigned logn, unsigned threads,
	const char *worker_mode, unsigned rng_mode, unsigned trials,
	unsigned warmups, uint64_t *baseline_cycles, int store_baseline)
{
	int pipefd[2];
	pid_t pid;
	FILE *fp;
	char line[MAX_BENCH_LINE];
	char logn_arg[16], threads_arg[16], trials_arg[16], warmups_arg[16];
	const char *rng_text = rng_mode_label(rng_mode);
	int status;
	int ok = 1;

	snprintf(logn_arg, sizeof logn_arg, "%u", logn);
	snprintf(threads_arg, sizeof threads_arg, "%u", threads);
	snprintf(trials_arg, sizeof trials_arg, "%u", trials);
	snprintf(warmups_arg, sizeof warmups_arg, "%u", warmups);
	if (pipe(pipefd) != 0) {
		fprintf(stderr, "ERR: pipe failed: %s\n", strerror(errno));
		return 0;
	}
	fflush(stdout);
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "ERR: fork failed: %s\n", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return 0;
	}
	if (pid == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
			fprintf(stderr, "ERR: dup2 failed: %s\n", strerror(errno));
			_exit(127);
		}
		close(pipefd[1]);
		execlp(self_path, self_path,
			"--single-config",
			"--logn", logn_arg,
			"--threads", threads_arg,
			"--worker-mode", worker_mode,
			"--rng-mode", rng_text,
			"--trials", trials_arg,
			"--warmups", warmups_arg,
			"--no-header",
			(char *)NULL);
		fprintf(stderr, "ERR: exec failed: %s\n", strerror(errno));
		_exit(127);
	}
	close(pipefd[1]);
	fp = fdopen(pipefd[0], "r");
	if (fp == NULL) {
		fprintf(stderr, "ERR: fdopen failed: %s\n", strerror(errno));
		close(pipefd[0]);
		(void)waitpid(pid, &status, 0);
		return 0;
	}
	while (fgets(line, sizeof line, fp) != NULL) {
		if (!emit_child_csv_line(line, baseline_cycles, trials,
			store_baseline))
		{
			ok = 0;
		}
	}
	if (ferror(fp)) {
		fprintf(stderr, "ERR: failed reading child CSV output\n");
		ok = 0;
	}
	fclose(fp);
	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "ERR: waitpid failed: %s\n", strerror(errno));
		return 0;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		if (WIFSIGNALED(status)) {
			fprintf(stderr,
				"ERR: child failed logn=%u threads=%u rng=%s"
				" signal=%d\n",
				logn, threads, rng_text, WTERMSIG(status));
		} else {
			fprintf(stderr,
				"ERR: child failed logn=%u threads=%u rng=%s"
				" status=%d\n",
				logn, threads, rng_text,
				WIFEXITED(status) ? WEXITSTATUS(status) : status);
		}
		ok = 0;
	}
	return ok;
}

static int
run_child_hawk_sampler(const char *self_path, unsigned logn, unsigned trials)
{
	int pipefd[2];
	pid_t pid;
	FILE *fp;
	char line[MAX_BENCH_LINE];
	char logn_arg[16], trials_arg[16];
	int status;
	int ok = 1;
	unsigned rows = 0;

	snprintf(logn_arg, sizeof logn_arg, "%u", logn);
	snprintf(trials_arg, sizeof trials_arg, "%u", trials);
	if (pipe(pipefd) != 0) {
		fprintf(stderr, "ERR: pipe failed: %s\n", strerror(errno));
		return 0;
	}
	fflush(stdout);
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "ERR: fork failed: %s\n", strerror(errno));
		close(pipefd[0]);
		close(pipefd[1]);
		return 0;
	}
	if (pid == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
			fprintf(stderr, "ERR: dup2 failed: %s\n", strerror(errno));
			_exit(127);
		}
		close(pipefd[1]);
		execlp(self_path, self_path,
			"--single-hawk-sampler",
			"--logn", logn_arg,
			"--trials", trials_arg,
			"--no-header",
			(char *)NULL);
		fprintf(stderr, "ERR: exec failed: %s\n", strerror(errno));
		_exit(127);
	}
	close(pipefd[1]);
	fp = fdopen(pipefd[0], "r");
	if (fp == NULL) {
		fprintf(stderr, "ERR: fdopen failed: %s\n", strerror(errno));
		close(pipefd[0]);
		(void)waitpid(pid, &status, 0);
		return 0;
	}
	while (fgets(line, sizeof line, fp) != NULL) {
		if (line[0] != 0 && line[0] != '\n' && line[0] != '\r'
			&& strncmp(line, "sampler_type,", 13) != 0)
		{
			rows ++;
		}
		if (!emit_child_csv_line(line, NULL, trials, 0)) {
			ok = 0;
		}
	}
	if (ferror(fp)) {
		fprintf(stderr, "ERR: failed reading HAWK sampler child CSV output\n");
		ok = 0;
	}
	fclose(fp);
	if (waitpid(pid, &status, 0) < 0) {
		fprintf(stderr, "ERR: waitpid failed: %s\n", strerror(errno));
		return 0;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		if (WIFSIGNALED(status)) {
			fprintf(stderr,
				"ERR: HAWK sampler child failed logn=%u"
				" signal=%d\n",
				logn, WTERMSIG(status));
		} else {
			fprintf(stderr,
				"ERR: HAWK sampler child failed logn=%u"
				" status=%d\n",
				logn, WIFEXITED(status)
					? WEXITSTATUS(status) : status);
		}
		ok = 0;
	}
	if (rows != trials) {
		fprintf(stderr,
			"ERR: HAWK sampler child logn=%u emitted %u rows"
			" instead of %u\n",
			logn, rows, trials);
		ok = 0;
	}
	return ok;
}

static int
run_isolated_matrix(const char *self_path, unsigned trials, unsigned warmups)
{
	int ok = 1;

	write_header();
	for (unsigned logn = 8; logn <= 10; logn ++) {
		uint64_t *baseline_cycles =
			calloc(trials, sizeof *baseline_cycles);

		if (baseline_cycles == NULL) {
			fprintf(stderr, "ERR: baseline allocation failed\n");
			return 1;
		}
		if (!run_child_hawk_sampler(self_path, logn,
			HAWK_BASELINE_BENCH_TRIALS))
		{
			ok = 0;
		}
		if (!run_child_config(self_path, logn, 1, "serial",
			E8_SAMPLER_RNG_PER_BLOCK, trials, warmups,
			baseline_cycles, 1))
		{
			ok = 0;
		}
		for (size_t rm = 0;
			rm < sizeof E8_RNG_MODE_CASES / sizeof E8_RNG_MODE_CASES[0];
			rm ++)
		{
			for (size_t u = 1;
				u < sizeof E8_THREAD_CASES / sizeof E8_THREAD_CASES[0];
				u ++)
			{
				if (!run_child_config(self_path, logn,
					E8_THREAD_CASES[u].threads, "spin",
					E8_RNG_MODE_CASES[rm].mode,
					trials, warmups, baseline_cycles, 0))
				{
					ok = 0;
				}
			}
		}
		free(baseline_cycles);
	}
	return ok ? 0 : 1;
}

static int
run_selected_configs(const char *self_path, unsigned trials, unsigned warmups)
{
	int ok = 1;

	write_header();
	for (unsigned logn = 8; logn <= 10; logn ++) {
		unsigned threads;
		unsigned rng_mode;

		if (!bench_selected_e8_sampler_config(logn,
				&threads, &rng_mode))
		{
			fprintf(stderr,
				"ERR: no selected E8 sampler config"
				" for logn=%u\n", logn);
			ok = 0;
			continue;
		}
		if (!run_child_hawk_sampler(self_path, logn,
			HAWK_BASELINE_BENCH_TRIALS))
		{
			ok = 0;
		}
		if (!run_child_config(self_path, logn, threads,
			threads == 1 ? "serial" : "spin", rng_mode,
			trials, warmups, NULL, 0))
		{
			ok = 0;
		}
	}
	return ok ? 0 : 1;
}

static int
run_sign_bench(unsigned trials)
{
	write_header();
	for (unsigned logn = 8; logn <= 10; logn ++) {
		for (unsigned trial_index = 0;
			trial_index < trials; trial_index ++)
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
	bench_options opts;

	if (!parse_options(argc, argv, &opts)) {
		usage(argv[0]);
		return 1;
	}
	if (opts.mode == BENCH_MODE_SINGLE_CONFIG) {
		return run_single_config(&opts);
	}
	if (opts.mode == BENCH_MODE_SINGLE_HAWK_SAMPLER) {
		return run_single_hawk_sampler(&opts);
	}
	if (opts.mode == BENCH_MODE_SIGN) {
		return run_sign_bench(opts.trials);
	}
	if (opts.mode == BENCH_MODE_ISOLATED_MATRIX) {
		return run_isolated_matrix(argv[0], opts.trials, opts.warmups);
	}
	if (opts.mode == BENCH_MODE_SELECTED_CONFIGS) {
		return run_selected_configs(argv[0], opts.trials, opts.warmups);
	}
	usage(argv[0]);
	return 1;
}
