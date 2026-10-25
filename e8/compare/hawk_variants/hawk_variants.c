/*
 * Benchmark-only HAWK sampler variants: registry, selection and the split
 * sampler (see hawk_variants.h). Integer-only. The worker streams, the
 * pool and the counters are the shared modules used by E8 (cmn_rng,
 * cmn_pool), with the same settings.
 */

#include <string.h>

#include "hawk_variants.h"
#include "hawk_shim.h"
#include "cmn_pool.h"

#define HAWKV_MAX_THREADS   24
#define HAWKV_SEED_BYTES    40

typedef char hawkv_pool_size_check[
	HAWKV_MAX_THREADS - 1 <= CMN_POOL_MAX_THREADS ? 1 : -1];

const hawk_sampler_variant hawk_sampler_variants[] = {
	{ "ref", 78, "ct_full_scan", NULL, NULL },
	{ "ref-T", 78, "ct_full_scan", hawkv_ref_range,
		hawkv_ref_tables_get },
	{ "hot64", 64, "vt_hot_table", hawkv_hot64_range,
		hawkv_hot64_tables_get },
	{ "hot32", 32, "vt_hot_table", hawkv_hot32_range,
		hawkv_hot32_tables_get }
};

const size_t hawk_sampler_variant_count =
	sizeof hawk_sampler_variants / sizeof hawk_sampler_variants[0];

hawk_sampler_fn hawk_variant_sampler = hawk_shim_sig_gauss;

static const hawk_sampler_variant *hawkv_selected = &hawk_sampler_variants[0];
static unsigned hawkv_threads = 1;
static cmn_rng_counters hawkv_rng_total;

/* see hawk_variants.h */
const hawk_sampler_variant *
hawk_sampler_variant_find(const char *name)
{
	if (name == NULL) {
		return NULL;
	}
	for (size_t u = 0; u < hawk_sampler_variant_count; u ++) {
		if (strcmp(hawk_sampler_variants[u].name, name) == 0) {
			return &hawk_sampler_variants[u];
		}
	}
	return NULL;
}

/* see hawk_variants.h */
int
hawkv_select(const hawk_sampler_variant *v, unsigned threads)
{
	if (v == NULL || threads == 0 || threads > HAWKV_MAX_THREADS) {
		return 0;
	}
	hawkv_selected = v;
	hawkv_threads = threads;
	hawk_variant_sampler = v->range == NULL
		? hawk_shim_sig_gauss : hawkv_sampler_split;
	return 1;
}

/* see hawk_variants.h */
void
hawkv_set_hook(hawk_sampler_fn fn)
{
	hawk_variant_sampler = fn;
}

/* see hawk_variants.h */
void
hawkv_rng_counters_get(cmn_rng_counters *total)
{
	*total = hawkv_rng_total;
}

/* see hawk_variants.h */
void
hawkv_rng_counters_reset(void)
{
	memset(&hawkv_rng_total, 0, sizeof hawkv_rng_total);
}

typedef struct {
	const hawk_sampler_variant *v;
	const void *tables;
	const shake_context *prefix;
	const uint8_t *seed;
	unsigned logn;
	unsigned worker_id;
	int8_t *x;
	const uint8_t *t;
	size_t start;
	size_t end;
	uint32_t sn;
	cmn_rng_counters rng;
	int ok;
} hawkv_worker;

/*
 * Worker stream:
 *   SHAKE256(prefix || "HAWK bench sampler worker v1" || seed
 *            || u8 logn || u8 worker_id || u64le(start) || u64le(end))
 */
static void
hawkv_worker_run(hawkv_worker *w)
{
	static const char domain[] = "HAWK bench sampler worker v1";
	uint8_t label[18];
	cmn_rng s;

	label[0] = (uint8_t)w->logn;
	label[1] = (uint8_t)w->worker_id;
	enc64le(label + 2, (uint64_t)w->start);
	enc64le(label + 10, (uint64_t)w->end);
	if (w->prefix != NULL) {
		cmn_rng_init_prefixed(&s, w->prefix, domain, sizeof domain - 1,
			w->seed, HAWKV_SEED_BYTES, label, sizeof label);
	} else {
		cmn_rng_init(&s, domain, sizeof domain - 1,
			w->seed, HAWKV_SEED_BYTES, label, sizeof label);
	}
	w->sn = 0;
	w->ok = w->v->range(w->tables, w->logn, w->x, w->t,
		w->start, w->end, &s, &w->sn);
	cmn_rng_counters_get(&s, &w->rng);
}

static void
hawkv_worker_job(void *ctx, unsigned worker)
{
	hawkv_worker *w = ctx;

	hawkv_worker_run(&w[worker]);
}

/* see hawk_variants.h */
uint32_t
hawkv_sampler_split(unsigned logn,
	void (*rng)(void *ctx, void *dst, size_t len), void *rng_context,
	const shake_context *sc_extra, int8_t *x, const uint8_t *t)
{
	const hawk_sampler_variant *v = hawkv_selected;
	size_t m = (size_t)2 << logn;
	unsigned T = hawkv_threads;
	uint8_t seed[HAWKV_SEED_BYTES];
	hawkv_worker workers[HAWKV_MAX_THREADS];
	uint32_t sn = 0;
	int ok = 1;

	/*
	 * The signer has no failure path for the sampler; a failure
	 * (which the generated tables rule out) returns a norm above every
	 * bound, so the attempt is rejected.
	 */
	rng(rng_context, seed, sizeof seed);
	for (unsigned w = 0; w < T; w ++) {
		hawkv_worker *wk = &workers[w];

		wk->v = v;
		wk->tables = v->tables(logn);
		wk->prefix = sc_extra;
		wk->seed = seed;
		wk->logn = logn;
		wk->worker_id = w;
		wk->x = x;
		wk->t = t;
		wk->start = (m * w) / T;
		wk->end = (m * (w + 1)) / T;
	}
	if (T == 1) {
		hawkv_worker_run(&workers[0]);
	} else {
		if (!cmn_pool_dispatch(hawkv_worker_job, workers, T - 1)) {
			return UINT32_MAX;
		}
		hawkv_worker_run(&workers[0]);
		if (!cmn_pool_wait()) {
			return UINT32_MAX;
		}
	}
	for (unsigned w = 0; w < T; w ++) {
		cmn_rng_counters_add(&hawkv_rng_total, &workers[w].rng);
		ok &= workers[w].ok;
		sn += workers[w].sn;
	}
	return ok ? sn : UINT32_MAX;
}
