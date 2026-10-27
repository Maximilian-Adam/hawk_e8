/*
 * Coefficient ranges of the HAWK-E8-CM public form over many keys
 * (docs/verifier_range.md).
 *
 * For each degree, N keys: HAWK's key generator (Hawk_keygen(), as
 * e8_keygen() calls it, from the shared signing RNG seeded with
 * u32le(200) || u32le(logn) || u32le(key index)), then the key generation
 * form e8_compute_keygen_qform(). Reported per degree: the maximum
 * |coefficient| of q00, q01 and q11, with the constant terms q00[0] and
 * q11[0] also reported apart (as HAWK's limits exclude them), the bit
 * length of each maximum, the number of keys outside the E8 key
 * generation limits (e8_keygen_limits_ok()), the number of keys whose
 * q00 is not invertible modulo one of the E8_VRFY_PRIMES verifier
 * primes or modulo all E8_NTT_PRIME_COUNT reference primes, and the
 * mean number of (f, g) draws (RNG calls) per key.
 *
 * Usage: e8_keygen_limits [--keys N] [--jobs J] [--logn L]
 * (defaults: 10000 keys, 8 jobs, every degree). CSV on stdout.
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hawk_e8_inner.h"

#define MAXN   1024

typedef struct {
	unsigned logn;
	unsigned keys;
	unsigned jobs;
	unsigned job;
	int32_t max_q00_0, max_q00, max_q01, max_q11_0, max_q11;
	unsigned outside_limits;
	unsigned not_invertible_vrfy;
	unsigned not_invertible_ref;
	uint64_t rng_calls;
	int failed;
} job_state;

static int32_t
iabs32(int32_t x)
{
	return x < 0 ? -x : x;
}

static unsigned
bit_length(int32_t x)
{
	unsigned b = 0;

	while (x != 0) {
		b ++;
		x >>= 1;
	}
	return b;
}

static int
invertible(unsigned logn, const int32_t *q00, unsigned primes)
{
	size_t n = (size_t)1 << logn;
	uint32_t t[MAXN];

	for (unsigned i = 0; i < primes; i ++) {
		e8_ntt_to_ntt(logn, t, q00, &E8_NTT_PRIMES[i]);
		for (size_t u = 0; u < n; u ++) {
			if (t[u] == 0) {
				return 0;
			}
		}
	}
	return 1;
}

static void *
run_job(void *arg)
{
	job_state *js = arg;
	unsigned logn = js->logn;
	size_t n = (size_t)1 << logn;
	int8_t f[MAXN], g[MAXN], F[MAXN], G[MAXN];
	int32_t q00[MAXN], q01[MAXN], q11[MAXN];
	uint8_t seed[40];
	uint8_t *tmp = malloc((size_t)20 << logn);

	if (tmp == NULL) {
		js->failed = 1;
		return NULL;
	}
	for (unsigned k = js->job; k < js->keys; k += js->jobs) {
		cmn_sign_rng rng;
		uint8_t rs[12];
		uint32_t v[3] = { 200, logn, k };

		for (unsigned u = 0; u < 3; u ++) {
			for (unsigned w = 0; w < 4; w ++) {
				rs[4 * u + w] = (uint8_t)(v[u] >> (w << 3));
			}
		}
		cmn_sign_rng_init(&rng, rs, sizeof rs);
		if (Hawk_keygen(logn, f, g, F, G, NULL, NULL, NULL, seed,
			cmn_sign_rng_hawk, &rng, tmp, (size_t)20 << logn) != 0)
		{
			js->failed = 1;
			break;
		}
		js->rng_calls += rng.calls;
		e8_compute_keygen_qform(q00, q01, q11, f, g, F, G, logn);
		if (iabs32(q00[0]) > js->max_q00_0) {
			js->max_q00_0 = iabs32(q00[0]);
		}
		if (iabs32(q11[0]) > js->max_q11_0) {
			js->max_q11_0 = iabs32(q11[0]);
		}
		for (size_t u = 0; u < n; u ++) {
			if (u != 0 && iabs32(q00[u]) > js->max_q00) {
				js->max_q00 = iabs32(q00[u]);
			}
			if (u != 0 && iabs32(q11[u]) > js->max_q11) {
				js->max_q11 = iabs32(q11[u]);
			}
			if (iabs32(q01[u]) > js->max_q01) {
				js->max_q01 = iabs32(q01[u]);
			}
		}
		if (!e8_keygen_limits_ok(logn, q00, q01, q11)) {
			js->outside_limits ++;
		}
		if (!invertible(logn, q00, E8_VRFY_PRIMES)) {
			js->not_invertible_vrfy ++;
		}
		if (!invertible(logn, q00, E8_NTT_PRIME_COUNT)) {
			js->not_invertible_ref ++;
		}
	}
	free(tmp);
	return NULL;
}

int
main(int argc, char **argv)
{
	unsigned keys = 10000, jobs = 8, only_logn = 0;
	job_state js[256];
	pthread_t th[256];

	for (int i = 1; i < argc; i ++) {
		if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
			keys = (unsigned)strtoul(argv[++ i], NULL, 10);
		} else if (strcmp(argv[i], "--jobs") == 0 && i + 1 < argc) {
			jobs = (unsigned)strtoul(argv[++ i], NULL, 10);
		} else if (strcmp(argv[i], "--logn") == 0 && i + 1 < argc) {
			only_logn = (unsigned)strtoul(argv[++ i], NULL, 10);
		} else {
			fprintf(stderr, "usage: %s [--keys N] [--jobs J]"
				" [--logn L]\n", argv[0]);
			return 2;
		}
	}
	if (jobs == 0 || jobs > 256 || keys == 0) {
		fprintf(stderr, "e8_keygen_limits: bad --keys or --jobs\n");
		return 2;
	}
	printf("logn,keys,max_q00_0,max_q00,max_q01,max_q11_0,max_q11,"
		"bits_q00_0,bits_q00,bits_q01,bits_q11_0,bits_q11,"
		"lim_q00_0,lim_q00,lim_q01,lim_q11_0,lim_q11,"
		"outside_limits,not_invertible_vrfy,not_invertible_ref,"
		"mean_rng_calls\n");
	for (unsigned logn = 8; logn <= 10; logn ++) {
		job_state tot;
		unsigned lim[5];

		if (only_logn != 0 && logn != only_logn) {
			continue;
		}
		for (unsigned j = 0; j < jobs; j ++) {
			memset(&js[j], 0, sizeof js[j]);
			js[j].logn = logn;
			js[j].keys = keys;
			js[j].jobs = jobs;
			js[j].job = j;
			if (pthread_create(&th[j], NULL, run_job, &js[j]) != 0) {
				fprintf(stderr, "e8_keygen_limits: thread\n");
				return 1;
			}
		}
		memset(&tot, 0, sizeof tot);
		for (unsigned j = 0; j < jobs; j ++) {
			pthread_join(th[j], NULL);
			if (js[j].failed) {
				fprintf(stderr, "e8_keygen_limits: keygen"
					" failed (logn=%u)\n", logn);
				return 1;
			}
#define MAXF(x)   if (js[j].x > tot.x) tot.x = js[j].x
			MAXF(max_q00_0);
			MAXF(max_q00);
			MAXF(max_q01);
			MAXF(max_q11_0);
			MAXF(max_q11);
#undef MAXF
			tot.outside_limits += js[j].outside_limits;
			tot.not_invertible_vrfy += js[j].not_invertible_vrfy;
			tot.not_invertible_ref += js[j].not_invertible_ref;
			tot.rng_calls += js[j].rng_calls;
		}
		e8_keygen_limits_bits(logn, lim);
		printf("%u,%u,%ld,%ld,%ld,%ld,%ld,%u,%u,%u,%u,%u,"
			"%u,%u,%u,%u,%u,%u,%u,%u,%.4f\n",
			logn, keys,
			(long)tot.max_q00_0, (long)tot.max_q00,
			(long)tot.max_q01, (long)tot.max_q11_0,
			(long)tot.max_q11,
			bit_length(tot.max_q00_0), bit_length(tot.max_q00),
			bit_length(tot.max_q01), bit_length(tot.max_q11_0),
			bit_length(tot.max_q11),
			lim[0], lim[1], lim[2], lim[3], lim[4],
			tot.outside_limits, tot.not_invertible_vrfy,
			tot.not_invertible_ref,
			(double)tot.rng_calls / (double)keys);
		fflush(stdout);
	}
	return 0;
}
