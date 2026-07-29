#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE   200809L
#endif

#ifndef HAWK_PROFILE_SIGN
#define HAWK_PROFILE_SIGN   1
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Pull in the build-generated, instrumented copy of normal HAWK signing.
 * The canonical HAWK source remains unchanged, and this benchmark does not
 * include or call any E8 code.
 */
#include "../build/hawk_sign_profile.c"

#define DEFAULT_HAWK_PROFILE_TRIALS    16
#define MAX_HAWK_PROFILE_TRIALS        1000000

typedef struct {
	uint64_t trials;
	uint64_t accepted;
	uint64_t failed;
	uint64_t attempts;
	uint64_t norm_rejections;
	uint64_t bound_rejections;
	uint64_t encode_rejections;
	uint64_t cycles_total;
	uint64_t cycles_key_expand;
	uint64_t cycles_hash_message;
	uint64_t cycles_salt;
	uint64_t cycles_challenge;
	uint64_t cycles_target;
	uint64_t cycles_sample;
	uint64_t cycles_norm_check;
	uint64_t cycles_reconstruct;
	uint64_t cycles_symbreak;
	uint64_t cycles_encode;
	uint64_t wall_ns_total;
	uint64_t wall_ns_key_expand;
	uint64_t wall_ns_hash_message;
	uint64_t wall_ns_salt;
	uint64_t wall_ns_challenge;
	uint64_t wall_ns_target;
	uint64_t wall_ns_sample;
	uint64_t wall_ns_norm_check;
	uint64_t wall_ns_reconstruct;
	uint64_t wall_ns_symbreak;
	uint64_t wall_ns_encode;
} hawk_profile_totals;

typedef struct {
	uint64_t state;
} bench_rng_state;

typedef struct {
	unsigned trials;
	int no_header;
} profile_options;

static hawk_profile_totals profile_totals[11];

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
	unsigned stream_id, unsigned logn, unsigned trial_index)
{
	uint64_t seed = UINT64_C(0xA11B5EED51A60000);

	seed ^= (uint64_t)stream_id * UINT64_C(0x9E3779B97F4A7C15);
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
	return parse_bench_count_env("HAWK_SIGN_PROFILE_TRIALS",
		DEFAULT_HAWK_PROFILE_TRIALS, 1, MAX_HAWK_PROFILE_TRIALS);
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

static void
usage(const char *prog)
{
	fprintf(stderr, "usage: %s [--trials N] [--no-header]\n", prog);
}

static int
parse_options(int argc, char **argv, profile_options *opts)
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
				1, MAX_HAWK_PROFILE_TRIALS, &opts->trials))
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
profile_add(unsigned logn, int accepted, const hawk_sign_profile *profile)
{
	hawk_profile_totals *total;

	if (logn > 10 || profile == NULL) {
		return;
	}
	total = &profile_totals[logn];
	total->trials ++;
	if (accepted) {
		total->accepted ++;
	} else {
		total->failed ++;
	}
	total->attempts += profile->attempts_total;
	total->norm_rejections += profile->norm_rejections;
	total->bound_rejections += profile->bound_rejections;
	total->encode_rejections += profile->encode_rejections;
	total->cycles_total += profile->cycles_sign_total;
	total->cycles_key_expand += profile->cycles_key_expand;
	total->cycles_hash_message += profile->cycles_hash_message;
	total->cycles_salt += profile->cycles_salt;
	total->cycles_challenge += profile->cycles_challenge;
	total->cycles_target += profile->cycles_target;
	total->cycles_sample += profile->cycles_sample;
	total->cycles_norm_check += profile->cycles_norm_check;
	total->cycles_reconstruct += profile->cycles_reconstruct;
	total->cycles_symbreak += profile->cycles_symbreak;
	total->cycles_encode += profile->cycles_encode;
	total->wall_ns_total += profile->wall_ns_sign_total;
	total->wall_ns_key_expand += profile->wall_ns_key_expand;
	total->wall_ns_hash_message += profile->wall_ns_hash_message;
	total->wall_ns_salt += profile->wall_ns_salt;
	total->wall_ns_challenge += profile->wall_ns_challenge;
	total->wall_ns_target += profile->wall_ns_target;
	total->wall_ns_sample += profile->wall_ns_sample;
	total->wall_ns_norm_check += profile->wall_ns_norm_check;
	total->wall_ns_reconstruct += profile->wall_ns_reconstruct;
	total->wall_ns_symbreak += profile->wall_ns_symbreak;
	total->wall_ns_encode += profile->wall_ns_encode;
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
		"\nHAWK_PROFILE_SIGN summary (hawk_sign_finish)\n");
	for (unsigned logn = 8; logn <= 10; logn ++) {
		const hawk_profile_totals *total = &profile_totals[logn];
		uint64_t stage_cycles, stage_wall_ns;
		uint64_t other_cycles = 0, other_wall_ns = 0;

		if (total->trials == 0) {
			continue;
		}
		stage_cycles = total->cycles_key_expand
			+ total->cycles_hash_message
			+ total->cycles_salt
			+ total->cycles_challenge
			+ total->cycles_target
			+ total->cycles_sample
			+ total->cycles_norm_check
			+ total->cycles_reconstruct
			+ total->cycles_symbreak
			+ total->cycles_encode;
		stage_wall_ns = total->wall_ns_key_expand
			+ total->wall_ns_hash_message
			+ total->wall_ns_salt
			+ total->wall_ns_challenge
			+ total->wall_ns_target
			+ total->wall_ns_sample
			+ total->wall_ns_norm_check
			+ total->wall_ns_reconstruct
			+ total->wall_ns_symbreak
			+ total->wall_ns_encode;
		if (total->cycles_total > stage_cycles) {
			other_cycles = total->cycles_total - stage_cycles;
		}
		if (total->wall_ns_total > stage_wall_ns) {
			other_wall_ns = total->wall_ns_total - stage_wall_ns;
		}

		fprintf(stderr,
			"logn=%u n=%u trials=%llu accepted=%llu failed=%llu "
			"attempts=%llu norm_rejections=%llu "
			"bound_rejections=%llu encode_rejections=%llu\n",
			logn, 1u << logn,
			(unsigned long long)total->trials,
			(unsigned long long)total->accepted,
			(unsigned long long)total->failed,
			(unsigned long long)total->attempts,
			(unsigned long long)total->norm_rejections,
			(unsigned long long)total->bound_rejections,
			(unsigned long long)total->encode_rejections);
		fprintf(stderr,
			"  total cycles=%llu wall_ns=%llu "
			"avg_cycles/sign=%llu avg_wall_ns/sign=%llu\n",
			(unsigned long long)total->cycles_total,
			(unsigned long long)total->wall_ns_total,
			(unsigned long long)profile_avg(total->cycles_total,
				total->trials),
			(unsigned long long)profile_avg(total->wall_ns_total,
				total->trials));
		profile_print_stage("key_expand",
			total->cycles_key_expand, total->cycles_total,
			total->wall_ns_key_expand, total->wall_ns_total);
		profile_print_stage("hash_msg_hpub",
			total->cycles_hash_message, total->cycles_total,
			total->wall_ns_hash_message, total->wall_ns_total);
		profile_print_stage("salt_derivation",
			total->cycles_salt, total->cycles_total,
			total->wall_ns_salt, total->wall_ns_total);
		profile_print_stage("challenge_hash",
			total->cycles_challenge, total->cycles_total,
			total->wall_ns_challenge, total->wall_ns_total);
		profile_print_stage("target_coset",
			total->cycles_target, total->cycles_total,
			total->wall_ns_target, total->wall_ns_total);
		profile_print_stage("gaussian_sampler",
			total->cycles_sample, total->cycles_total,
			total->wall_ns_sample, total->wall_ns_total);
		profile_print_stage("norm_rejection",
			total->cycles_norm_check, total->cycles_total,
			total->wall_ns_norm_check, total->wall_ns_total);
		profile_print_stage("reconstruct_s1",
			total->cycles_reconstruct, total->cycles_total,
			total->wall_ns_reconstruct, total->wall_ns_total);
		profile_print_stage("symbreak_bounds",
			total->cycles_symbreak, total->cycles_total,
			total->wall_ns_symbreak, total->wall_ns_total);
		profile_print_stage("encode_sig",
			total->cycles_encode, total->cycles_total,
			total->wall_ns_encode, total->wall_ns_total);
		profile_print_stage("other",
			other_cycles, total->cycles_total,
			other_wall_ns, total->wall_ns_total);
	}
}

static void
write_header(void)
{
	printf("sampler_type,scope,logn,n,trial_index,sigma,"
		"accepted,attempts,cycles_total,cycles_per_unit,"
		"wall_ns_total,wall_ns_per_unit,notes\n");
}

static void
write_row(unsigned logn, unsigned trial_index, int accepted,
	const hawk_sign_profile *profile, const char *notes)
{
	uint64_t cycles_total = 0;
	uint64_t wall_ns_total = 0;
	uint64_t attempts = 0;

	if (profile != NULL) {
		cycles_total = profile->cycles_sign_total;
		wall_ns_total = profile->wall_ns_sign_total;
		attempts = profile->attempts_total;
	}
	printf("hawk_sign_profile,signature,%u,%u,%u,%.3f,%d,%llu,"
		"%llu,%llu,%llu,%llu,%s\n",
		logn, 1u << logn, trial_index, hawk_sigma_sign(logn),
		accepted,
		(unsigned long long)attempts,
		(unsigned long long)cycles_total,
		(unsigned long long)(accepted ? cycles_total : 0),
		(unsigned long long)wall_ns_total,
		(unsigned long long)(accepted ? wall_ns_total : 0),
		notes);
}

static void
bench_make_message_context(shake_context *sc_data,
	unsigned logn, unsigned trial_index)
{
	static const char prefix[] = "ordinary hawk profile bench";
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
	hawk_sign_profile profile;
	int accepted;

	bench_rng_init(&key_rng, 1, logn, trial_index);
	bench_rng_init(&sign_rng, 2, logn, trial_index);
	bench_make_message_context(&sc_data, logn, trial_index);
	memset(sig, 0, sizeof sig);
	memset(&profile, 0, sizeof profile);

	if (!hawk_keygen(logn, priv, pub, bench_rng, &key_rng,
		tmp_keygen, HAWK_TMPSIZE_KEYGEN(logn)))
	{
		write_row(logn, trial_index, 0, NULL, "keygen_setup_failed");
		return;
	}

	hawk_sign_profile_reset();
	accepted = hawk_sign_finish(logn, bench_rng, &sign_rng,
		sig, &sc_data, priv, tmp_sign, HAWK_TMPSIZE_SIGN(logn));
	if (!hawk_sign_profile_get(&profile)) {
		write_row(logn, trial_index, accepted, NULL,
			"profile_unavailable");
		return;
	}
	profile_add(logn, accepted, &profile);
	write_row(logn, trial_index, accepted, &profile,
		"ordinary_hawk_sign_finish_encoded_private_key");
}

static int
run_profile(const profile_options *opts)
{
	if (!opts->no_header) {
		write_header();
	}
	for (unsigned logn = 8; logn <= 10; logn ++) {
		for (unsigned trial_index = 0;
			trial_index < opts->trials; trial_index ++)
		{
			bench_hawk_sign(logn, trial_index);
		}
	}
	profile_print_summary();
	return 0;
}

int
main(int argc, char **argv)
{
	profile_options opts;

	if (!parse_options(argc, argv, &opts)) {
		usage(argv[0]);
		return 1;
	}
	return run_profile(&opts);
}
