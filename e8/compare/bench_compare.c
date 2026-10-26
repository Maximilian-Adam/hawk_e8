#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bench_sign_core.h"

/*
 * The comparison harness (plan.md Step 7): every (scheme, variant,
 * threads, logn) configuration of HAWK and HAWK-E8-CM signing, timed under
 * identical conditions in one binary built with one flag set.
 *
 * Signing, accounting, seeds, messages, verification and rng_bytes are
 * bench_sign_core.c's (as in e8_sign_bench): both accounting modes
 * (all_in, prepared) through each variant's own sign_finish, every
 * signature verified outside the timed region, attempts = rng_calls / 2.
 *
 * Keys: K keys per (scheme, logn), generated once at the start (key k of a
 * scheme from signing-RNG stream 1 + scheme, index k) and prepared once;
 * every variant and thread count of that scheme signs with the same keys.
 * Key generation and preparation are timed and written as keygen /
 * prepare rows (variant and threads empty: the key is shared).
 *
 * Messages: signature m of key k is message i = k M + m at degree logn,
 * signed with signing-RNG stream 16 + accounting, index i; both schemes
 * and every variant sign the same messages with the same streams.
 *
 * Interleaving: configurations are grouped by thread count T (latency
 * mode: T in --threads, default 1, 2, 4, 8, 16; throughput mode: T = 1).
 * Within a group the schedule is round-robin per batch: for k = 0 .. K-1,
 * for every configuration A, B, C, ... of the group (in the order of
 * --list), one batch = key k's M signatures, each under all_in and then
 * prepared. Drift therefore spreads over every configuration of the
 * group. Between groups the pool is resized (cmn_pool_ensure(T - 1),
 * outside every timed region) and every configuration of the next group
 * is warmed again, so that no timed call starts or joins a thread and
 * serial configurations never run beside spinning pool workers (Step 4
 * finding (d)).
 *
 * Warm-up: before a group's first batch, --warmup W untimed-for-the-
 * results signatures per configuration and accounting mode (key 0,
 * message 0xFFFFFF, signing-RNG stream 9), measured, verified and written
 * as warmup rows.
 *
 * Modes:
 *   latency     one signer process; T sampler threads (the caller plus
 *               T - 1 pool workers).
 *   throughput  P independent single-thread signer processes (fork()ed
 *               after key generation), each running the same schedule
 *               and started together; rows carry the process index
 *               (proc). Signatures per second per core follow from the
 *               rows' wall_ns.
 *
 * Pinning (--cpus LIST): latency pins the caller to LIST[0] and pool
 * worker w to LIST[w mod |LIST|] (cmn_pool affinity), so T <= |LIST|
 * threads run on distinct CPUs; throughput pins process p to LIST[p]. The
 * list is printed in the summary (env_capture.sh records it).
 *
 * stdout (or --out FILE): one CSV row per signature, warm-up signature,
 * key generation and key preparation, in the shared schema (cmn_csv.h)
 * with mode, proc, flags (the build's flag set) and batch (position of
 * the batch in the process's schedule). stderr: a summary per
 * configuration and accounting mode, the pool activity per group, and
 * the failure count. The exit status is 1 if any signature failed to
 * sign or verify.
 */

#define DEFAULT_KEYS           4
#define DEFAULT_SIGS_PER_KEY   16
#define DEFAULT_WARMUP         1
#define DEFAULT_PROCS          2
#define MAX_KEYS               100000
#define MAX_SIGS_PER_KEY       100000
#define MAX_SIGNATURES         10000000
#define MAX_WARMUP             1000
#define MAX_PROCS              256
#define MAX_CPUS               256
#define MAX_THREAD_CASES       8
#define MAX_VARIANT_NAMES      8
#define MAX_CONFIGS            256
#define WARMUP_MESSAGE         0xFFFFFFu

#define MODE_LATENCY      0
#define MODE_THROUGHPUT   1

static const char *const MODE_NAME[] = { "latency", "throughput" };

typedef struct {
	int mode;
	unsigned keys;
	unsigned sigs_per_key;
	unsigned warmup;
	unsigned procs;
	unsigned logns[3];
	unsigned logn_count;
	unsigned thread_cases[MAX_THREAD_CASES];
	unsigned thread_case_count;
	const char *variant_names[MAX_VARIANT_NAMES];
	unsigned variant_name_count;
	unsigned scheme_mask;
	unsigned cpus[MAX_CPUS];
	unsigned cpu_count;
	const char *run_id;
	const char *out_path;
	int no_header;
	int pool_accounting;
} compare_options;

/*
 * Per configuration and accounting mode: the timed values of the
 * signatures (for the summary medians) and the counts.
 */
typedef struct {
	uint64_t *tsc;
	uint64_t *core;
	uint64_t *wall;
	uint64_t *cpu;
	uint64_t signatures;
	uint64_t attempts;
	uint64_t rng_bytes;
	uint64_t sign_failures;
	uint64_t verify_failures;
	uint64_t warmups;
} acct_stats;

typedef struct {
	bsc_config cfg;
	unsigned group_threads;
	acct_stats acct[BSC_ACCT_COUNT];
} compare_config;

static compare_config configs[MAX_CONFIGS];
static unsigned config_count;

/* keys[scheme][logn - 8][k] */
static bsc_key *keys[BSC_SCHEME_COUNT][3];

static char run_id_buf[48];
static const char *run_id;
static cmn_csv csv;
static uint8_t sig_buf[BSC_SIG_SIZE];

/* Process index in throughput mode (CMN_UNAVAILABLE in latency mode). */
static uint64_t proc_index = CMN_UNAVAILABLE;
static int run_mode;
static uint64_t batch_counter;

static int
parse_unsigned_arg(const char *text, unsigned min_value,
	unsigned max_value, unsigned *out)
{
	char *end = NULL;
	unsigned long x;

	if (text == NULL || out == NULL) {
		return 0;
	}
	errno = 0;
	x = strtoul(text, &end, 10);
	if (end == text || *end != 0 || errno != 0
		|| x < min_value || x > max_value)
	{
		return 0;
	}
	*out = (unsigned)x;
	return 1;
}

/*
 * Comma-separated list of unsigned values in [min_value, max_value];
 * returns the count, 0 on error.
 */
static unsigned
parse_list(const char *text, unsigned *out, unsigned max_count,
	unsigned min_value, unsigned max_value)
{
	char buf[1024];
	char *tok, *save = NULL;
	unsigned count = 0;

	if (strlen(text) >= sizeof buf) {
		return 0;
	}
	strcpy(buf, text);
	for (tok = strtok_r(buf, ",", &save); tok != NULL;
		tok = strtok_r(NULL, ",", &save))
	{
		if (count == max_count || !parse_unsigned_arg(tok,
			min_value, max_value, &out[count]))
		{
			return 0;
		}
		count ++;
	}
	return count;
}

static int
variant_selected(const compare_options *opts, const char *name)
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
 * The configurations in schedule order: by thread group, then degree,
 * scheme and variant (registry order).
 */
static int
build_configs(const compare_options *opts)
{
	unsigned tcount = opts->mode == MODE_THROUGHPUT
		? 1 : opts->thread_case_count;

	config_count = 0;
	for (unsigned c = 0; c < tcount; c ++) {
		unsigned threads = opts->mode == MODE_THROUGHPUT
			? 1 : opts->thread_cases[c];
		int seen = 0;

		for (unsigned d = 0; d < c; d ++) {
			seen |= opts->thread_cases[d] == threads;
		}
		if (seen) {
			continue;
		}
		for (unsigned l = 0; l < opts->logn_count; l ++) {
			for (unsigned scheme = 0; scheme < BSC_SCHEME_COUNT;
				scheme ++)
			{
				if (!(opts->scheme_mask & (1u << scheme))) {
					continue;
				}
				for (size_t vi = 0;
					vi < bsc_variant_count(scheme); vi ++)
				{
					compare_config *cc;

					if (config_count == MAX_CONFIGS) {
						fprintf(stderr, "ERR: too many"
							" configurations\n");
						return 0;
					}
					cc = &configs[config_count];
					memset(cc, 0, sizeof *cc);
					if (!bsc_config_init(&cc->cfg, scheme, vi,
						opts->logns[l], threads)
						|| !variant_selected(opts,
						cc->cfg.variant))
					{
						continue;
					}
					cc->group_threads = threads;
					config_count ++;
				}
			}
		}
	}
	if (config_count == 0) {
		fprintf(stderr, "ERR: no configuration selected\n");
		return 0;
	}
	return 1;
}

static void
list_configs(const compare_options *opts)
{
	printf("mode,index,group_threads,scheme,variant,threshold_bits,"
		"technique,threads,logn\n");
	for (unsigned u = 0; u < config_count; u ++) {
		const bsc_config *c = &configs[u].cfg;

		printf("%s,%u,%u,%s,%s,%u,%s,%u,%u\n", MODE_NAME[opts->mode],
			u, configs[u].group_threads, bsc_scheme_name[c->scheme],
			c->variant, c->threshold_bits, c->technique,
			c->threads, c->logn);
	}
}

static int
alloc_stats(const compare_options *opts)
{
	size_t len = (size_t)opts->keys * opts->sigs_per_key
		* sizeof(uint64_t);

	for (unsigned u = 0; u < config_count; u ++) {
		for (unsigned a = 0; a < BSC_ACCT_COUNT; a ++) {
			acct_stats *s = &configs[u].acct[a];

			s->tsc = malloc(len);
			s->core = malloc(len);
			s->wall = malloc(len);
			s->cpu = malloc(len);
			if (s->tsc == NULL || s->core == NULL
				|| s->wall == NULL || s->cpu == NULL)
			{
				return 0;
			}
		}
	}
	return 1;
}

/*
 * Common fields of a row of configuration c (or of a shared key when c
 * is NULL).
 */
static void
row_init(cmn_sign_row *row, const char *kind, const bsc_config *c,
	unsigned scheme, unsigned logn, unsigned key_index,
	const cmn_measure *m)
{
	bsc_row_init(row, run_id, kind, c, scheme, logn, key_index, m);
	row->mode = MODE_NAME[run_mode];
	row->proc = proc_index;
}

/*
 * Generate and prepare K keys for every (scheme, logn) in use, writing
 * keygen and prepare rows.
 */
static int
generate_keys(const compare_options *opts)
{
	for (unsigned u = 0; u < config_count; u ++) {
		const bsc_config *c = &configs[u].cfg;
		bsc_key **slot = &keys[c->scheme][c->logn - 8];

		if (*slot != NULL) {
			continue;
		}
		*slot = calloc(opts->keys, sizeof **slot);
		if (*slot == NULL) {
			fprintf(stderr, "ERR: out of memory (keys)\n");
			return 0;
		}
		for (unsigned k = 0; k < opts->keys; k ++) {
			cmn_measure m;
			cmn_sign_row row;

			if (!bsc_keygen(c->scheme, c->logn, k, &(*slot)[k], &m)) {
				fprintf(stderr, "ERR: keygen failed scheme=%s"
					" logn=%u key=%u\n",
					bsc_scheme_name[c->scheme], c->logn, k);
				return 0;
			}
			row_init(&row, "keygen", NULL, c->scheme, c->logn, k,
				&m);
			cmn_csv_sign_row(&csv, &row);
			if (!bsc_prepare(c->scheme, c->logn, &(*slot)[k], &m)) {
				fprintf(stderr, "ERR: key preparation failed"
					" scheme=%s logn=%u key=%u\n",
					bsc_scheme_name[c->scheme], c->logn, k);
				return 0;
			}
			row_init(&row, "prepare", NULL, c->scheme, c->logn, k,
				&m);
			row.accounting = bsc_acct_name[BSC_ACCT_PREPARED];
			cmn_csv_sign_row(&csv, &row);
		}
	}
	return 1;
}

/*
 * One signature of configuration cc under acct: timed sign, verification
 * outside the timed region, statistics and row. kind is "sign" or
 * "warmup"; batch is CMN_UNAVAILABLE for warm-up signatures.
 */
static void
sign_one(compare_config *cc, unsigned acct, unsigned key_index,
	unsigned msg_index, unsigned stream, unsigned rng_index,
	const char *kind, uint64_t batch)
{
	const bsc_config *c = &cc->cfg;
	const bsc_key *key = &keys[c->scheme][c->logn - 8][key_index];
	acct_stats *s = &cc->acct[acct];
	shake_context sc_data;
	cmn_sign_rng rng;
	cmn_measure m;
	cmn_sign_row row;
	unsigned attempts;
	uint64_t rng_bytes;
	int sign_ok, verify_ok = 0;
	int warmup = strcmp(kind, "warmup") == 0;

	bsc_make_message(&sc_data, c->logn, msg_index);
	bsc_rng_init(&rng, stream, c->logn, rng_index);
	sign_ok = bsc_sign(c, acct, key, &sc_data, &rng, sig_buf,
		&attempts, &rng_bytes, &m, NULL, NULL);
	if (sign_ok) {
		verify_ok = bsc_verify(c->scheme, c->logn, key, sig_buf,
			&sc_data);
	}
	if (!sign_ok) {
		s->sign_failures ++;
	} else if (!verify_ok) {
		s->verify_failures ++;
	}
	if (warmup) {
		s->warmups ++;
	} else if (sign_ok) {
		uint64_t k = s->signatures ++;

		s->tsc[k] = m.tsc_cycles;
		s->core[k] = m.core_cycles;
		s->wall[k] = m.wall_ns;
		s->cpu[k] = m.cpu_ns_all_threads;
		s->attempts += attempts;
		s->rng_bytes += rng_bytes;
	}
	row_init(&row, sign_ok ? kind : (warmup ? "warmup" : "sign_failed"),
		c, 0, 0, key_index, &m);
	if (!warmup) {
		row.msg_index = msg_index;
	}
	row.attempts = attempts;
	row.sig_bytes = c->sig_bytes;
	row.rng_bytes = rng_bytes;
	row.accounting = bsc_acct_name[acct];
	row.encoding_included = 1;
	row.verify_ok = (uint64_t)(sign_ok && verify_ok);
	row.batch = batch;
	cmn_csv_sign_row(&csv, &row);
}

/*
 * Pool activity of one thread group, after its warm-up.
 */
typedef struct {
	unsigned threads;
	cmn_pool_stats pool;
	uint64_t signatures;
} group_stats;

static group_stats groups[MAX_THREAD_CASES];
static unsigned group_count;

/*
 * Run the interleaved schedule over every configuration (one thread
 * group after the other). Returns 0 on a fatal error.
 */
static int
run_schedule(const compare_options *opts)
{
	unsigned first = 0;

	while (first < config_count) {
		unsigned T = configs[first].group_threads;
		unsigned end = first;
		group_stats *g = &groups[group_count ++];

		while (end < config_count && configs[end].group_threads == T) {
			end ++;
		}
		g->threads = T;

		/*
		 * Pool sizing outside every timed region: T - 1 workers for
		 * the whole group, none for serial groups.
		 */
		cmn_pool_shrink(0);
		if (T > 1 && !cmn_pool_ensure(T - 1)) {
			fprintf(stderr, "ERR: cannot start %u pool threads\n",
				T - 1);
			return 0;
		}

		/* Warm-up of every configuration of the group. */
		for (unsigned u = first; u < end; u ++) {
			compare_config *cc = &configs[u];

			if (!bsc_select(&cc->cfg)) {
				fprintf(stderr, "ERR: cannot select %s %s"
					" threads=%u\n",
					bsc_scheme_name[cc->cfg.scheme],
					cc->cfg.variant, cc->cfg.threads);
				return 0;
			}
			for (unsigned w = 0; w < opts->warmup; w ++) {
				for (unsigned a = 0; a < BSC_ACCT_COUNT; a ++) {
					sign_one(cc, a, 0, WARMUP_MESSAGE,
						BSC_STREAM_WARMUP,
						w * BSC_ACCT_COUNT + a,
						"warmup", CMN_UNAVAILABLE);
				}
			}
		}
		cmn_pool_stats_reset();

		/* Round-robin batches. */
		for (unsigned k = 0; k < opts->keys; k ++) {
			for (unsigned u = first; u < end; u ++) {
				compare_config *cc = &configs[u];
				uint64_t batch = batch_counter ++;

				(void)bsc_select(&cc->cfg);
				for (unsigned m = 0; m < opts->sigs_per_key;
					m ++)
				{
					unsigned msg = k * opts->sigs_per_key + m;

					for (unsigned a = 0; a < BSC_ACCT_COUNT;
						a ++)
					{
						sign_one(cc, a, k, msg,
							BSC_STREAM_SIGN + a, msg,
							"sign", batch);
						g->signatures ++;
					}
				}
			}
		}
		cmn_pool_stats_get(&g->pool);
		bsc_deselect();
		cmn_pool_shrink(0);
		if (g->pool.threads_started != 0 || g->pool.threads_joined != 0)
		{
			fprintf(stderr, "ERR: pool threads started or joined"
				" inside the timed schedule (group T=%u)\n", T);
			return 0;
		}
		first = end;
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

/*
 * Median (sorts v), or CMN_UNAVAILABLE if there are no values or any
 * value is unavailable.
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

static void
print_u64(FILE *fp, uint64_t x)
{
	if (x != CMN_UNAVAILABLE) {
		fprintf(fp, "%llu", (unsigned long long)x);
	}
}

/*
 * Summary of this process's configurations and groups; returns the
 * number of signing or verification failures.
 */
static uint64_t
print_summary(FILE *fp, const compare_options *opts)
{
	uint64_t failures = 0;

	fprintf(fp, "\nBENCH_COMPARE run_id=%s mode=%s", run_id,
		MODE_NAME[opts->mode]);
	if (proc_index != CMN_UNAVAILABLE) {
		fprintf(fp, " proc=%llu", (unsigned long long)proc_index);
	}
	fprintf(fp, " flags=%s keys=%u sigs_per_key=%u warmup=%u"
		" core_cycles=%s pool_accounting=%s\n"
		"scheme,variant,logn,threads,accounting,signatures,warmups,"
		"median_tsc_cycles,median_core_cycles,median_wall_ns,"
		"median_cpu_ns_all_threads,attempts_per_sign,"
		"rng_bytes_per_sign,sig_bytes,sign_failures,"
		"verify_failures\n",
		bsc_build_flagset, opts->keys, opts->sigs_per_key,
		opts->warmup, cmn_core_cycles_status(),
		opts->pool_accounting ? "on" : "off");
	for (unsigned u = 0; u < config_count; u ++) {
		compare_config *cc = &configs[u];

		for (unsigned a = 0; a < BSC_ACCT_COUNT; a ++) {
			acct_stats *s = &cc->acct[a];
			uint64_t n = s->signatures;

			fprintf(fp, "%s,%s,%u,%u,%s,%llu,%llu,",
				bsc_scheme_name[cc->cfg.scheme],
				cc->cfg.variant, cc->cfg.logn,
				cc->cfg.threads, bsc_acct_name[a],
				(unsigned long long)n,
				(unsigned long long)s->warmups);
			print_u64(fp, median_u64(s->tsc, n));
			fputc(',', fp);
			print_u64(fp, median_u64(s->core, n));
			fputc(',', fp);
			print_u64(fp, median_u64(s->wall, n));
			fputc(',', fp);
			print_u64(fp, median_u64(s->cpu, n));
			fprintf(fp, ",%.4f,%.1f,%u,%llu,%llu\n",
				n == 0 ? 0.0
					: (double)s->attempts / (double)n,
				n == 0 ? 0.0
					: (double)s->rng_bytes / (double)n,
				cc->cfg.sig_bytes,
				(unsigned long long)s->sign_failures,
				(unsigned long long)s->verify_failures);
			failures += s->sign_failures + s->verify_failures;
		}
	}
	fprintf(fp, "\nBENCH_COMPARE pool per thread group, after warm-up"
		" (idle CPU is pool spinning between dispatches, excluded"
		" from every row)\n"
		"threads,signatures,dispatches,threads_started,"
		"threads_joined,job_cpu_ns,idle_cpu_ns,idle_cpu_ns_per_sign\n");
	for (unsigned u = 0; u < group_count; u ++) {
		const group_stats *g = &groups[u];

		fprintf(fp, "%u,%llu,%llu,%llu,%llu,%llu,%llu,%.0f\n",
			g->threads, (unsigned long long)g->signatures,
			(unsigned long long)g->pool.dispatches,
			(unsigned long long)g->pool.threads_started,
			(unsigned long long)g->pool.threads_joined,
			(unsigned long long)g->pool.job_cpu_ns,
			(unsigned long long)g->pool.idle_cpu_ns,
			g->signatures == 0 ? 0.0 : (double)g->pool.idle_cpu_ns
				/ (double)g->signatures);
	}
	return failures;
}

static void
print_cpus(FILE *fp, const compare_options *opts)
{
	fprintf(fp, "BENCH_COMPARE cpus=");
	if (opts->cpu_count == 0) {
		fprintf(fp, "none");
	}
	for (unsigned u = 0; u < opts->cpu_count; u ++) {
		fprintf(fp, "%s%u", u == 0 ? "" : ",", opts->cpus[u]);
	}
	fputc('\n', fp);
}

static int
run_latency(const compare_options *opts)
{
	uint64_t failures;

	if (opts->cpu_count > 0) {
		unsigned n = opts->cpu_count < CMN_POOL_MAX_THREADS + 1
			? opts->cpu_count : CMN_POOL_MAX_THREADS + 1;

		if (!cmn_pin_current_thread(opts->cpus[0])
			|| !cmn_pool_set_affinity(opts->cpus, n))
		{
			fprintf(stderr, "ERR: cannot pin to the --cpus list\n");
			return 1;
		}
	}
	if (!generate_keys(opts) || !run_schedule(opts)) {
		return 1;
	}
	fflush(csv.fp);
	print_cpus(stderr, opts);
	failures = print_summary(stderr, opts);
	if (failures != 0) {
		fprintf(stderr, "\nERR: %llu signing or verification"
			" failures\n", (unsigned long long)failures);
		return 1;
	}
	return 0;
}

/*
 * Copy the contents of a temporary file to fp.
 */
static int
copy_file(FILE *src, FILE *dst)
{
	char buf[1 << 16];
	size_t len;

	if (fflush(src) != 0 || fseek(src, 0, SEEK_SET) != 0) {
		return 0;
	}
	while ((len = fread(buf, 1, sizeof buf, src)) > 0) {
		if (fwrite(buf, 1, len, dst) != len) {
			return 0;
		}
	}
	return !ferror(src);
}

static int
run_throughput(const compare_options *opts)
{
	FILE *csv_tmp[MAX_PROCS], *sum_tmp[MAX_PROCS];
	pid_t pid[MAX_PROCS];
	int start_pipe[2];
	int status_all = 0;

	if (!generate_keys(opts)) {
		return 1;
	}
	if (pipe(start_pipe) != 0) {
		fprintf(stderr, "ERR: pipe: %s\n", strerror(errno));
		return 1;
	}
	for (unsigned p = 0; p < opts->procs; p ++) {
		csv_tmp[p] = tmpfile();
		sum_tmp[p] = tmpfile();
		if (csv_tmp[p] == NULL || sum_tmp[p] == NULL) {
			fprintf(stderr, "ERR: tmpfile: %s\n", strerror(errno));
			return 1;
		}
	}

	/*
	 * Children inherit the keys; no pool thread exists. Flush every
	 * stream first so that no buffered output is written twice.
	 */
	fflush(NULL);
	for (unsigned p = 0; p < opts->procs; p ++) {
		pid[p] = fork();
		if (pid[p] < 0) {
			fprintf(stderr, "ERR: fork: %s\n", strerror(errno));
			return 1;
		}
		if (pid[p] == 0) {
			char c;
			uint64_t failures;
			int ok = 1;

			close(start_pipe[1]);
			proc_index = p;
			if (opts->cpu_count > 0
				&& !cmn_pin_current_thread(opts->cpus[p]))
			{
				fprintf(stderr, "ERR: cannot pin process %u to"
					" cpu %u\n", p, opts->cpus[p]);
				_exit(2);
			}

			/*
			 * The inherited core-cycle counter counts the parent
			 * thread; open this process's own.
			 */
			cmn_core_cycles_close();
			(void)cmn_core_cycles_open();
			cmn_csv_sign_init(&csv, csv_tmp[p]);
			cmn_csv_skip_header(&csv);

			/* Start barrier: the parent closes the pipe. */
			while (read(start_pipe[0], &c, 1) < 0
				&& errno == EINTR)
			{
			}
			close(start_pipe[0]);
			ok = run_schedule(opts);
			failures = print_summary(sum_tmp[p], opts);
			fflush(NULL);
			_exit(!ok ? 2 : failures != 0 ? 1 : 0);
		}
	}
	close(start_pipe[0]);
	close(start_pipe[1]);
	for (unsigned p = 0; p < opts->procs; p ++) {
		int status;

		while (waitpid(pid[p], &status, 0) < 0) {
			if (errno != EINTR) {
				fprintf(stderr, "ERR: waitpid: %s\n",
					strerror(errno));
				return 1;
			}
		}
		if (!WIFEXITED(status) || WEXITSTATUS(status) == 2) {
			fprintf(stderr, "ERR: signer process %u failed\n", p);
			status_all = 1;
		} else if (WEXITSTATUS(status) != 0) {
			fprintf(stderr, "ERR: signer process %u had signing"
				" or verification failures\n", p);
			status_all = 1;
		}
	}
	for (unsigned p = 0; p < opts->procs; p ++) {
		if (!copy_file(csv_tmp[p], csv.fp)
			|| !copy_file(sum_tmp[p], stderr))
		{
			fprintf(stderr, "ERR: cannot copy the output of"
				" process %u\n", p);
			status_all = 1;
		}
		fclose(csv_tmp[p]);
		fclose(sum_tmp[p]);
	}
	fflush(csv.fp);
	fprintf(stderr, "\nBENCH_COMPARE procs=%u\n", opts->procs);
	print_cpus(stderr, opts);
	return status_all;
}

static void
usage(const char *prog)
{
	fprintf(stderr,
		"usage: %s [--mode latency|throughput] [--keys K]"
		" [--sigs-per-key M]\n"
		"       [--warmup W] [--logn LIST] [--threads LIST]"
		" [--variant NAME]...\n"
		"       [--scheme hawk|e8]... [--procs P] [--cpus LIST]"
		" [--run-id ID]\n"
		"       [--out FILE] [--no-header] [--no-pool-accounting]"
		" [--list] [--build-info]\n"
		"  --mode MODE       latency (one signer, T sampler threads;"
		" default) or\n"
		"                    throughput (P single-thread signer"
		" processes)\n"
		"  --keys K          keys per scheme and degree (default %u)\n"
		"  --sigs-per-key M  signatures per key and configuration, each"
		" under both\n"
		"                    accounting modes (default %u)\n"
		"  --warmup W        warm-up signatures per configuration and"
		" accounting\n"
		"                    mode at the start of its thread group"
		" (default %u)\n"
		"  --logn LIST       degrees, from 8,9,10 (default all)\n"
		"  --threads LIST    latency thread counts T, 1..%u (default"
		" 1,2,4,8,16);\n"
		"                    upstream HAWK ref runs with T = 1 only;"
		" throughput\n"
		"                    mode always uses T = 1\n"
		"  --variant NAME    this variant of every scheme that has it"
		" (repeatable;\n"
		"                    default all)\n"
		"  --scheme NAME     this scheme only (repeatable; default"
		" both)\n"
		"  --procs P         throughput signer processes (default"
		" %u)\n"
		"  --cpus LIST       pin: latency caller to LIST[0] and pool"
		" worker w to\n"
		"                    LIST[w mod |LIST|] (needs |LIST| >= T);"
		" throughput\n"
		"                    process p to LIST[p] (needs |LIST| >="
		" P)\n"
		"  --run-id ID       run identifier (default: UTC time and a"
		" random suffix)\n"
		"  --out FILE        write the CSV to FILE (default stdout)\n"
		"  --list            print the configurations in schedule"
		" order and exit\n"
		"  --build-info      print the build's flag set, flags,"
		" compiler and the\n"
		"                    core-cycle counter status and exit\n",
		prog, DEFAULT_KEYS, DEFAULT_SIGS_PER_KEY, DEFAULT_WARMUP,
		CMN_POOL_MAX_THREADS + 1, DEFAULT_PROCS);
}

static int
valid_run_id(const char *s)
{
	size_t len = strlen(s);

	if (len == 0 || len >= sizeof run_id_buf) {
		return 0;
	}
	for (size_t u = 0; u < len; u ++) {
		char c = s[u];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
			|| (c >= 'A' && c <= 'Z') || c == '-' || c == '_'
			|| c == '.'))
		{
			return 0;
		}
	}
	return 1;
}

/*
 * Returns 1 to run, 0 on error, -1 to exit successfully (--list,
 * --build-info).
 */
static int
parse_options(int argc, char **argv, compare_options *opts)
{
	int list = 0;

	memset(opts, 0, sizeof *opts);
	opts->mode = MODE_LATENCY;
	opts->keys = DEFAULT_KEYS;
	opts->sigs_per_key = DEFAULT_SIGS_PER_KEY;
	opts->warmup = DEFAULT_WARMUP;
	opts->procs = DEFAULT_PROCS;
	opts->logns[0] = 8;
	opts->logns[1] = 9;
	opts->logns[2] = 10;
	opts->logn_count = 3;
	opts->thread_cases[0] = 1;
	opts->thread_cases[1] = 2;
	opts->thread_cases[2] = 4;
	opts->thread_cases[3] = 8;
	opts->thread_cases[4] = 16;
	opts->thread_case_count = 5;
	opts->pool_accounting = 1;
	for (unsigned s = 0; s < BSC_SCHEME_COUNT; s ++) {
		if (bsc_variant_count(s) > 0) {
			opts->scheme_mask |= 1u << s;
		}
	}

	for (int i = 1; i < argc; i ++) {
		const char *arg = argv[i];
		const char *val = i + 1 < argc ? argv[i + 1] : NULL;
		int ok = 1;

		if (strcmp(arg, "--help") == 0) {
			usage(argv[0]);
			exit(0);
		} else if (strcmp(arg, "--list") == 0) {
			list = 1;
			continue;
		} else if (strcmp(arg, "--build-info") == 0) {
			(void)cmn_core_cycles_open();
			printf("flags: %s\ncflags: %s\ncompiler: %s\n"
				"core_cycles: %s\n", bsc_build_flagset,
				bsc_build_cflags, bsc_build_compiler,
				cmn_core_cycles_status());
			return -1;
		} else if (strcmp(arg, "--no-header") == 0) {
			opts->no_header = 1;
			continue;
		} else if (strcmp(arg, "--no-pool-accounting") == 0) {
			opts->pool_accounting = 0;
			continue;
		}
		if (val == NULL) {
			fprintf(stderr, "ERR: missing value or unknown"
				" argument: %s\n", arg);
			return 0;
		}
		i ++;
		if (strcmp(arg, "--mode") == 0) {
			if (strcmp(val, "latency") == 0) {
				opts->mode = MODE_LATENCY;
			} else if (strcmp(val, "throughput") == 0) {
				opts->mode = MODE_THROUGHPUT;
			} else {
				ok = 0;
			}
		} else if (strcmp(arg, "--keys") == 0) {
			ok = parse_unsigned_arg(val, 1, MAX_KEYS, &opts->keys);
		} else if (strcmp(arg, "--sigs-per-key") == 0) {
			ok = parse_unsigned_arg(val, 1, MAX_SIGS_PER_KEY,
				&opts->sigs_per_key);
		} else if (strcmp(arg, "--warmup") == 0) {
			ok = parse_unsigned_arg(val, 0, MAX_WARMUP,
				&opts->warmup);
		} else if (strcmp(arg, "--procs") == 0) {
			ok = parse_unsigned_arg(val, 1, MAX_PROCS,
				&opts->procs);
		} else if (strcmp(arg, "--logn") == 0) {
			opts->logn_count = parse_list(val, opts->logns, 3,
				8, 10);
			ok = opts->logn_count > 0;
		} else if (strcmp(arg, "--threads") == 0) {
			opts->thread_case_count = parse_list(val,
				opts->thread_cases, MAX_THREAD_CASES,
				1, CMN_POOL_MAX_THREADS + 1);
			ok = opts->thread_case_count > 0;
		} else if (strcmp(arg, "--cpus") == 0) {
			opts->cpu_count = parse_list(val, opts->cpus,
				MAX_CPUS, 0, 4095);
			ok = opts->cpu_count > 0;
		} else if (strcmp(arg, "--variant") == 0) {
			ok = opts->variant_name_count < MAX_VARIANT_NAMES;
			if (ok) {
				opts->variant_names[
					opts->variant_name_count ++] = val;
			}
		} else if (strcmp(arg, "--scheme") == 0) {
			static unsigned explicit_mask = 0;
			unsigned s = strcmp(val, "hawk") == 0 ? BSC_SCHEME_HAWK
				: strcmp(val, "e8") == 0 ? BSC_SCHEME_E8
				: BSC_SCHEME_COUNT;

			ok = s < BSC_SCHEME_COUNT
				&& bsc_variant_count(s) > 0;
			if (ok) {
				explicit_mask |= 1u << s;
				opts->scheme_mask = explicit_mask;
			}
		} else if (strcmp(arg, "--run-id") == 0) {
			ok = valid_run_id(val);
			opts->run_id = val;
		} else if (strcmp(arg, "--out") == 0) {
			opts->out_path = val;
		} else {
			fprintf(stderr, "ERR: unknown argument: %s\n", arg);
			return 0;
		}
		if (!ok) {
			fprintf(stderr, "ERR: invalid %s\n", arg);
			return 0;
		}
	}
	if ((uint64_t)opts->keys * opts->sigs_per_key > MAX_SIGNATURES) {
		fprintf(stderr, "ERR: --keys times --sigs-per-key exceeds"
			" %u\n", MAX_SIGNATURES);
		return 0;
	}
	for (unsigned u = 0; u < opts->variant_name_count; u ++) {
		int found = 0;

		for (unsigned s = 0; s < BSC_SCHEME_COUNT; s ++) {
			for (size_t vi = 0; vi < bsc_variant_count(s); vi ++) {
				bsc_config c;

				(void)bsc_config_init(&c, s, vi, 8, 1);
				found |= strcmp(c.variant,
					opts->variant_names[u]) == 0;
			}
		}
		if (!found) {
			fprintf(stderr, "ERR: unknown variant %s\n",
				opts->variant_names[u]);
			return 0;
		}
	}
	if (!build_configs(opts)) {
		return 0;
	}
	if (list) {
		list_configs(opts);
		return -1;
	}
	if (opts->cpu_count > 0) {
		unsigned need = 1;

		if (opts->mode == MODE_THROUGHPUT) {
			need = opts->procs;
		} else {
			for (unsigned u = 0; u < config_count; u ++) {
				if (configs[u].group_threads > need) {
					need = configs[u].group_threads;
				}
			}
		}
		if (opts->cpu_count < need) {
			fprintf(stderr, "ERR: --cpus lists %u CPUs, %u"
				" needed\n", opts->cpu_count, need);
			return 0;
		}
	}
	return 1;
}

int
main(int argc, char **argv)
{
	compare_options opts;
	FILE *out = stdout;
	int r;

	r = parse_options(argc, argv, &opts);
	if (r < 0) {
		return 0;
	}
	if (r == 0) {
		usage(argv[0]);
		return 1;
	}
	if (opts.run_id != NULL) {
		strcpy(run_id_buf, opts.run_id);
	} else {
		bsc_make_run_id(run_id_buf, sizeof run_id_buf);
	}
	run_id = run_id_buf;
	run_mode = opts.mode;
	if (opts.out_path != NULL) {
		out = fopen(opts.out_path, "w");
		if (out == NULL) {
			fprintf(stderr, "ERR: cannot open %s: %s\n",
				opts.out_path, strerror(errno));
			return 1;
		}
	}
	if (!alloc_stats(&opts)) {
		fprintf(stderr, "ERR: out of memory\n");
		return 1;
	}
	cmn_csv_sign_init(&csv, out);
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
	r = opts.mode == MODE_LATENCY
		? run_latency(&opts) : run_throughput(&opts);
	if (out != stdout && fclose(out) != 0) {
		fprintf(stderr, "ERR: cannot write %s\n", opts.out_path);
		return 1;
	}
	return r;
}
