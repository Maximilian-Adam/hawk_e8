#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include "cmn_csv.h"
#include "cmn_pool.h"
#include "cmn_rng.h"
#include "cmn_timing.h"

/*
 * Tests of the shared measurement modules in e8/common/: RNG streams
 * (known answer, buffering invariance, exact counters), worker pool
 * (dispatch coverage, reductions, repeated dispatches, resizing,
 * accounting), timing (monotonic clocks, perf unavailability reported
 * rather than fatal) and the CSV writer.
 */

static void
fail(const char *what)
{
	fprintf(stderr, "FAIL: %s\n", what);
	exit(EXIT_FAILURE);
}

static void
check(int cond, const char *what)
{
	if (!cond) {
		fail(what);
	}
}

/* ==================================================================== */
/* RNG */

#define RNG_TOTAL   4096

static const char RNG_DOMAIN[] = "cmn rng test domain";
static const uint8_t RNG_SEED[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
static const uint8_t RNG_LABEL[] = { 0xA5, 0x5A, 0x00, 0xFF };

static void
rng_test_init(cmn_rng *r)
{
	cmn_rng_init(r, RNG_DOMAIN, sizeof RNG_DOMAIN - 1,
		RNG_SEED, sizeof RNG_SEED, RNG_LABEL, sizeof RNG_LABEL);
}

/*
 * Reference output: SHAKE256(domain || seed || label), extracted in one
 * call.
 */
static void
rng_reference(uint8_t *out, size_t len)
{
	shake_context sc;
	uint8_t prefix[sizeof RNG_DOMAIN - 1 + sizeof RNG_SEED
		+ sizeof RNG_LABEL];

	memcpy(prefix, RNG_DOMAIN, sizeof RNG_DOMAIN - 1);
	memcpy(prefix + sizeof RNG_DOMAIN - 1, RNG_SEED, sizeof RNG_SEED);
	memcpy(prefix + sizeof RNG_DOMAIN - 1 + sizeof RNG_SEED,
		RNG_LABEL, sizeof RNG_LABEL);
	shake_init(&sc, 256);
	shake_inject(&sc, prefix, sizeof prefix);
	shake_flip(&sc);
	shake_extract(&sc, out, len);
}

static uint64_t
ceil_div(uint64_t a, uint64_t b)
{
	return (a + b - 1) / b;
}

static void
check_counters(const cmn_rng *r, uint64_t delivered, const char *what)
{
	cmn_rng_counters c;
	uint64_t absorbed = sizeof RNG_DOMAIN - 1 + sizeof RNG_SEED
		+ sizeof RNG_LABEL;

	cmn_rng_counters_get(r, &c);
	check(c.bytes_delivered == delivered, what);
	check(c.bytes_squeezed >= c.bytes_delivered
		&& c.bytes_squeezed - c.bytes_delivered < CMN_RNG_BUF_BYTES,
		what);
	check(c.permutations == absorbed / 136
		+ ceil_div(c.bytes_squeezed, 136), what);
}

typedef struct {
	uint8_t next;
	uint64_t calls;
	uint64_t bytes;
} counting_source;

static void
counting_source_read(void *ctx, void *dst, size_t len)
{
	counting_source *cs = ctx;
	uint8_t *buf = dst;

	cs->calls ++;
	cs->bytes += len;
	for (size_t u = 0; u < len; u ++) {
		buf[u] = cs->next ++;
	}
}

static void
test_rng(void)
{
	static uint8_t ref[RNG_TOTAL], out[RNG_TOTAL];
	static const size_t sizes[] = {
		1, 3, 8, 7, 13, 127, 128, 129, 5, 300, 0, 64, 1000
	};
	cmn_rng r;
	cmn_rng_counters c;
	size_t off;
	unsigned step;

	printf("common rng: ");
	fflush(stdout);
	rng_reference(ref, sizeof ref);

	/* Known answer, through the buffer. */
	rng_test_init(&r);
	cmn_rng_read_bytes(&r, out, sizeof out);
	check(memcmp(out, ref, sizeof ref) == 0, "rng known answer (bytes)");
	check_counters(&r, sizeof out, "rng counters (bytes)");

	/* Known answer, one bulk read. */
	rng_test_init(&r);
	cmn_rng_read_bulk(&r, out, sizeof out);
	check(memcmp(out, ref, sizeof ref) == 0, "rng known answer (bulk)");
	cmn_rng_counters_get(&r, &c);
	check(c.bytes_delivered == sizeof out && c.bytes_squeezed == sizeof out
		&& c.refills == 1, "rng counters (bulk)");

	/*
	 * Buffering invariance: mixed u64, byte and bulk reads deliver the
	 * same sequence as the one bulk read, and the counters stay exact
	 * after every read.
	 */
	rng_test_init(&r);
	off = 0;
	step = 0;
	while (off < sizeof out) {
		size_t len = sizes[step % (sizeof sizes / sizeof sizes[0])];

		switch (step % 3) {
		case 0:
			if (off + 8 <= sizeof out) {
				uint64_t x = cmn_rng_read_u64le(&r);

				for (unsigned u = 0; u < 8; u ++) {
					out[off ++] = (uint8_t)(x >> (u << 3));
				}
			}
			break;
		case 1:
			if (len > sizeof out - off) {
				len = sizeof out - off;
			}
			cmn_rng_read_bytes(&r, out + off, len);
			off += len;
			break;
		default:
			if (len > sizeof out - off) {
				len = sizeof out - off;
			}
			cmn_rng_read_bulk(&r, out + off, len);
			off += len;
			break;
		}
		check_counters(&r, off, "rng counters (mixed)");
		step ++;
	}
	check(memcmp(out, ref, sizeof ref) == 0, "rng buffering invariance");

	/* u64 reads only: one refill per 16 reads, nothing discarded. */
	rng_test_init(&r);
	for (unsigned u = 0; u < 16; u ++) {
		uint64_t x = cmn_rng_read_u64le(&r);
		uint64_t y = 0;

		for (unsigned v = 0; v < 8; v ++) {
			y |= (uint64_t)ref[8 * u + v] << (v << 3);
		}
		check(x == y, "rng u64 little-endian");
	}
	cmn_rng_counters_get(&r, &c);
	check(c.bytes_delivered == 128 && c.bytes_squeezed == 128
		&& c.refills == 1 && c.permutations == 1, "rng u64 counters");
	(void)cmn_rng_read_u64le(&r);
	cmn_rng_counters_get(&r, &c);
	check(c.bytes_delivered == 136 && c.bytes_squeezed == 256
		&& c.refills == 2 && c.permutations == 2,
		"rng u64 counters after refill");

	/* Summation. */
	{
		cmn_rng_counters sum;

		memset(&sum, 0, sizeof sum);
		cmn_rng_counters_add(&sum, &c);
		cmn_rng_counters_add(&sum, &c);
		check(sum.bytes_delivered == 272 && sum.bytes_squeezed == 512
			&& sum.refills == 4 && sum.permutations == 4,
			"rng counters add");
	}

	/* Callback source: same buffering, 128-byte requests. */
	{
		counting_source cs;
		uint8_t buf[300];

		memset(&cs, 0, sizeof cs);
		cmn_rng_init_source(&r, counting_source_read, &cs);
		cmn_rng_read_bytes(&r, buf, 100);
		cmn_rng_read_bulk(&r, buf + 100, 200);
		for (unsigned u = 0; u < sizeof buf; u ++) {
			check(buf[u] == (uint8_t)u, "rng source order");
		}
		check(cs.calls == 2 && cs.bytes == 128 + 172,
			"rng source requests");
		cmn_rng_counters_get(&r, &c);
		check(c.bytes_delivered == 300 && c.bytes_squeezed == 300
			&& c.refills == 2 && c.permutations == 0,
			"rng source counters");
	}

	/* Signing RNG: a counted SHAKE256 stream of its own domain. */
	{
		static const char domain[] = "HAWK-E8 bench signing RNG v1";
		cmn_sign_rng s;
		uint8_t a[200], b[200];
		shake_context sc;

		cmn_sign_rng_init(&s, RNG_SEED, sizeof RNG_SEED);
		cmn_sign_rng_hawk(&s, a, 14);
		cmn_sign_rng_hawk(&s, a + 14, 40);
		cmn_sign_rng_hawk(&s, a + 54, 146);
		shake_init(&sc, 256);
		shake_inject(&sc, domain, sizeof domain - 1);
		shake_inject(&sc, RNG_SEED, sizeof RNG_SEED);
		shake_flip(&sc);
		shake_extract(&sc, b, sizeof b);
		check(memcmp(a, b, sizeof a) == 0, "signing rng output");
		check(s.calls == 3 && s.bytes == 200, "signing rng counts");
	}
	printf("done.\n");
}

/* ==================================================================== */
/* Pool */

#define POOL_ITEMS   (1u << 16)

typedef struct {
	volatile unsigned runs[CMN_POOL_MAX_THREADS + 1];
	unsigned threads;
	const uint32_t *items;
	uint64_t partial[CMN_POOL_MAX_THREADS + 1];
	unsigned burn;
} pool_test_ctx;

static void
pool_test_job(void *ctx, unsigned worker)
{
	pool_test_ctx *t = ctx;
	size_t start = ((size_t)POOL_ITEMS * worker) / t->threads;
	size_t end = ((size_t)POOL_ITEMS * (worker + 1)) / t->threads;
	uint64_t sum = 0;

	(void)__sync_fetch_and_add(&t->runs[worker], 1);
	for (unsigned rep = 0; rep <= t->burn; rep ++) {
		sum = 0;
		for (size_t u = start; u < end; u ++) {
			sum += t->items[u] ^ (uint32_t)rep;
		}
	}
	t->partial[worker] = sum;
}

/*
 * One full run on T threads (the caller is worker 0); returns the
 * reduced sum.
 */
static uint64_t
pool_run(pool_test_ctx *t, unsigned threads)
{
	uint64_t sum = 0;

	t->threads = threads;
	memset(t->partial, 0, sizeof t->partial);
	check(cmn_pool_dispatch(pool_test_job, t, threads - 1),
		"pool dispatch");
	pool_test_job(t, 0);
	check(cmn_pool_wait(), "pool wait");
	for (unsigned u = 0; u < threads; u ++) {
		sum += t->partial[u];
	}
	return sum;
}

static void
test_pool(void)
{
	static const unsigned thread_cases[] = { 2, 3, 4, 8, 16, 24 };
	static uint32_t items[POOL_ITEMS];
	static pool_test_ctx t;
	uint64_t expected = 0;
	cmn_pool_stats st;
	cmn_pool_dispatch_acct acct;

	printf("common pool: ");
	fflush(stdout);
	for (unsigned u = 0; u < POOL_ITEMS; u ++) {
		items[u] = u * 2654435761u;
		expected += items[u];
	}
	memset(&t, 0, sizeof t);
	t.items = items;

	/* Coverage, reductions and repeated dispatches. */
	for (size_t c = 0; c < sizeof thread_cases / sizeof thread_cases[0];
		c ++)
	{
		unsigned threads = thread_cases[c];

		memset((void *)t.runs, 0, sizeof t.runs);
		for (unsigned rep = 0; rep < 200; rep ++) {
			check(pool_run(&t, threads) == expected,
				"pool reduction");
		}
		for (unsigned u = 0; u <= CMN_POOL_MAX_THREADS; u ++) {
			check(t.runs[u] == (u < threads ? 200u : 0u),
				"pool worker runs once per dispatch");
		}
		check(cmn_pool_thread_count() == threads - 1,
			"pool thread count after dispatch");
	}

	/* Growing and shrinking. */
	cmn_pool_stats_reset();
	check(cmn_pool_ensure(4) && cmn_pool_thread_count() == 4,
		"pool ensure shrinks");
	check(cmn_pool_ensure(12) && cmn_pool_thread_count() == 12,
		"pool ensure grows");
	cmn_pool_shrink(20);
	check(cmn_pool_thread_count() == 12, "pool shrink no-op");
	cmn_pool_shrink(2);
	check(cmn_pool_thread_count() == 2, "pool shrink");
	check(pool_run(&t, 8) == expected, "pool run after shrink");
	cmn_pool_shrink(0);
	check(cmn_pool_thread_count() == 0, "pool shrink to zero");
	check(pool_run(&t, 3) == expected, "pool run after shrink to zero");
	check(!cmn_pool_ensure(CMN_POOL_MAX_THREADS + 1),
		"pool rejects too many threads");
	check(!cmn_pool_dispatch(pool_test_job, &t, 0)
		&& !cmn_pool_dispatch(NULL, &t, 1),
		"pool rejects bad dispatch");
	cmn_pool_stats_get(&st);
	check(st.threads_started == 8 + 5 + 2
		&& st.threads_joined == 19 + 10 + 7,
		"pool thread start/join counts");
	cmn_pool_shutdown();
	check(cmn_pool_thread_count() == 0, "pool shutdown");
	check(pool_run(&t, 4) == expected, "pool run after shutdown");

	/* Without accounting, the workers' share is unavailable. */
	check(!cmn_pool_get_accounting(), "pool accounting off by default");
	cmn_pool_last_dispatch(&acct);
	check(acct.workers == 3 && acct.cpu_ns == CMN_UNAVAILABLE
		&& acct.core_cycles == CMN_UNAVAILABLE,
		"pool unaccounted dispatch");
	{
		cmn_pool_stamp s;
		cmn_measure m;

		cmn_pool_measure_start(&s);
		(void)pool_run(&t, 4);
		cmn_pool_measure_stop(&m, &s);
		check(m.cpu_ns_all_threads == CMN_UNAVAILABLE
			&& m.core_cycles == CMN_UNAVAILABLE
			&& m.wall_ns != 0,
			"pool measure without accounting");
	}

	/* Per-worker CPU accounting. */
	check(cmn_pool_set_accounting(1) && cmn_pool_get_accounting(),
		"pool accounting on");
	cmn_pool_stats_reset();
	t.burn = 200;
	{
		cmn_pool_stamp s;
		cmn_measure m;
		uint64_t worker_cpu;

		cmn_pool_measure_start(&s);
		check(pool_run(&t, 8) == expected, "pool accounted run");
		cmn_pool_measure_stop(&m, &s);
		cmn_pool_last_dispatch(&acct);
		check(acct.workers == 7 && acct.cpu_ns != CMN_UNAVAILABLE
			&& acct.cpu_ns > 0, "pool per-worker cpu");
		check(cmn_core_cycles_available() != 1
			|| (acct.core_cycles != CMN_UNAVAILABLE
			&& acct.core_cycles > 0), "pool per-worker cycles");
		check(m.cpu_ns_all_threads != CMN_UNAVAILABLE
			&& m.cpu_ns_all_threads > acct.cpu_ns,
			"pool measure includes caller and workers");
		worker_cpu = acct.cpu_ns;
		cmn_pool_stats_get(&st);
		check(st.dispatches == 1 && st.unaccounted_dispatches == 0
			&& st.job_cpu_ns == worker_cpu
			&& st.thread_cpu_ns >= st.job_cpu_ns,
			"pool accounted stats");
	}
	t.burn = 0;
	cmn_pool_shrink(0);
	cmn_pool_stats_get(&st);
	check(st.thread_cpu_ns >= st.job_cpu_ns
		&& st.idle_cpu_ns == st.thread_cpu_ns - st.job_cpu_ns,
		"pool idle cpu after join");
	check(cmn_pool_set_accounting(0), "pool accounting off");
	printf("done.\n");
}

/* ==================================================================== */
/* Timing */

/*
 * In a child process with no free file descriptor, perf_event_open()
 * fails; the failure must be recorded and reported, and measurements must
 * go on without core cycles.
 */
static void
test_perf_unavailable(void)
{
	pid_t pid;
	int status;

	fflush(stdout);
	pid = fork();
	check(pid >= 0, "fork");
	if (pid == 0) {
		struct rlimit rl;
		cmn_measure m;
		cmn_stamp s;
		int ok;

		rl.rlim_cur = 0;
		rl.rlim_max = 0;
		ok = setrlimit(RLIMIT_NOFILE, &rl) == 0
			&& cmn_core_cycles_available() == -1
			&& !cmn_core_cycles_open()
			&& cmn_core_cycles_available() == 0
			&& strncmp(cmn_core_cycles_status(), "unavailable: ",
				13) == 0
			&& cmn_core_cycles_read() == CMN_UNAVAILABLE
			&& !cmn_core_cycles_open();
		cmn_measure_start(&s);
		cmn_measure_stop(&m, &s);
		ok = ok && m.core_cycles == CMN_UNAVAILABLE
			&& m.tsc_cycles != CMN_UNAVAILABLE;
		_exit(ok ? 0 : 1);
	}
	check(waitpid(pid, &status, 0) == pid, "waitpid");
	check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"perf unavailability reported, not fatal");
}

static void
test_timing(void)
{
	uint64_t w0, t0, c0;
	volatile uint64_t spin = 0;
	cmn_stamp s;
	cmn_measure m;

	printf("common timing: ");
	fflush(stdout);
	check(cmn_delta(0, 0) == 0 && cmn_delta(5, 5) == 1
		&& cmn_delta(7, 3) == 1 && cmn_delta(3, 10) == 7,
		"timing delta");
	test_perf_unavailable();

	w0 = cmn_wall_ns();
	t0 = cmn_tsc_start();
	c0 = cmn_thread_cpu_ns();
	check(w0 != 0 && c0 != 0, "timing clocks readable");
	for (unsigned u = 0; u < 1000; u ++) {
		uint64_t w1 = cmn_wall_ns();
		uint64_t t1 = cmn_tsc_end();
		uint64_t c1 = cmn_thread_cpu_ns();

		check(w1 >= w0 && t1 >= t0 && c1 >= c0, "timing monotonic");
		w0 = w1;
		t0 = t1;
		c0 = c1;
	}

	cmn_core_cycles_open();
	check(cmn_core_cycles_available() != -1
		&& cmn_core_cycles_status() != NULL, "timing perf state");
	cmn_measure_start(&s);
	for (unsigned u = 0; u < 1000000; u ++) {
		spin += u;
	}
	cmn_measure_stop(&m, &s);
	check(m.tsc_cycles > 1 && m.wall_ns > 1 && m.cpu_ns_all_threads > 1,
		"timing measure");
	check(cmn_core_cycles_available() == 1
		? m.core_cycles != CMN_UNAVAILABLE && m.core_cycles > 1
		: m.core_cycles == CMN_UNAVAILABLE, "timing core cycles");
	printf("core_cycles %s, done.\n",
		cmn_core_cycles_available() == 1 ? "available" : "unavailable");
}

/* ==================================================================== */
/* CSV */

static void
test_csv(void)
{
	static const char *const cols[] = { "a", "b", "c" };
	char *buf = NULL;
	size_t len = 0;
	FILE *fp;
	cmn_csv csv;
	cmn_sign_row row;

	printf("common csv: ");
	fflush(stdout);
	fp = open_memstream(&buf, &len);
	check(fp != NULL, "open_memstream");
	cmn_csv_init(&csv, fp, cols, 3);
	cmn_csv_str(&csv, "x");
	cmn_csv_u64(&csv, 42);
	cmn_csv_u64(&csv, CMN_UNAVAILABLE);
	cmn_csv_end_row(&csv);
	cmn_csv_header(&csv);
	cmn_csv_empty(&csv);
	cmn_csv_str(&csv, NULL);
	cmn_csv_u64(&csv, 0);
	cmn_csv_end_row(&csv);
	fflush(fp);
	check(strcmp(buf, "a,b,c\nx,42,\n,,0\n") == 0, "csv basic");
	fclose(fp);
	free(buf);

	buf = NULL;
	fp = open_memstream(&buf, &len);
	check(fp != NULL, "open_memstream");
	cmn_csv_sign_init(&csv, fp);
	cmn_sign_row_clear(&row);
	row.kind = "sign";
	row.logn = 8;
	cmn_csv_sign_row(&csv, &row);
	fflush(fp);
	check(strcmp(buf, "kind,run_id,scheme,variant,threshold_bits,"
		"technique,threads,logn,key_index,msg_index,attempts,"
		"sig_bytes,rng_bytes,tsc_cycles,core_cycles,wall_ns,"
		"cpu_ns_all_threads,accounting,encoding_included,verify_ok\n"
		"sign,,,,,,,8,,,,,,,,,,,,\n") == 0, "csv sign schema");
	fclose(fp);
	free(buf);
	printf("done.\n");
}

int
main(void)
{
	/*
	 * Timing first: its perf-unavailability child must fork before
	 * this process opens a core-cycle counter.
	 */
	test_timing();
	test_rng();
	test_pool();
	test_csv();
	return 0;
}
