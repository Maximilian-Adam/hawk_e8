#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hawk_e8_inner.h"
#include "cmn_rng.h"
#include "e8_tables_geom.h"
#include "hawk_shim.h"
#include "hawk_variants.h"

/*
 * Sampler histograms for the distribution tests of Step 6 of the
 * fair-comparison plan (e8/docs/validation.md; analysis:
 * tools/distribution_tests.py).
 *
 *   e8_distribution_sample --kind 1d|block|signorm --scheme hawk|e8
 *                          --variant NAME --logn 8|9|10 [--id I | --ids A-B]
 *                          --count N [--seed X] [--part P] [--header]
 *
 * Kinds (one job per call; every job draws from its own stream):
 *
 *   1d       N draws of one 1D table: HAWK parity I (values x in 2Z + I),
 *            E8 centre class I (product coordinates). Hooks: HAWK ref
 *            upstream sig_gauss() through hawk_shim_sig_gauss() with every
 *            parity bit I (2n draws per call); HAWK ref-T, hot64, hot32
 *            the variant's range sampler over 4096 coordinates per call;
 *            E8 hot64, hot32 e8_hot*_sample_1d(); E8 ref
 *            e8_ref_sample_1d_seq() over 4096 draws per call (draw groups
 *            continue across the draws, as in a block stream).
 *   block    N blocks of each E8 label tau in A..B, consecutive blocks
 *            from one stream as one sampler worker reads them
 *            (e8_hot*_sample_block(), e8_ref_sample_blocks() over 1024
 *            blocks per call), with the trace: the component (rows
 *            "component", id = tau), the squared norm ||P z||^2 (rows
 *            "blocknorm", id = tau) and every product coordinate by its
 *            centre class e8_tab_coord_class[tau][component][v] (rows
 *            "blockcoord", id = class, summed over the job's taus).
 *   signorm  N full-dimension samples for one fixed target through the
 *            variant's sampler as the signer calls it, serial: HAWK the
 *            variant hook after hawkv_select(v, 1) (upstream sig_gauss()
 *            for ref, the split sampler otherwise), returning ||x||^2;
 *            E8 the variant's sample_z() in the bound per-worker mode with
 *            an empty prefix, returning ||P z||^2. Rows "signorm"
 *            (id = 0, value = norm) and "signorm_target" (HAWK id =
 *            parity, E8 id = tau; count = coordinates or blocks of the
 *            target with that parity or label; written by part 0 only,
 *            so that merged parts count the target once).
 *
 * Streams: cmn_rng with domain "e8_distribution_sample v1", seed
 * u64le(seed) and label kind || 0 || scheme || 0 || variant || 0 ||
 * u32le(logn) || u32le(id) || u32le(part); signorm reads its sampler
 * seeds from the signing RNG (cmn_sign_rng) seeded with the same label
 * bytes (u64le(seed) || label; HAWK ref's 1d jobs likewise), and its
 * target from the stream of variant "target", id 0xFFFFFFFF and part 0
 * (the same target for every variant of a scheme and logn).
 *
 * Output (stdout): CSV rows kind,scheme,variant,logn,id,value,count for
 * every non-zero count (with --header, the header first). Exit status 0
 * on success, 1 on a sampler failure, 2 on a usage error.
 */

#define SCHEME_HAWK   0
#define SCHEME_E8     1

#define KIND_1D        0
#define KIND_BLOCK     1
#define KIND_SIGNORM   2

#define CHUNK          4096
#define REF_CHUNK      1024
#define VAL_OFF        128
#define VAL_RANGE      256
#define BLOCKNORM_MAX  (1u << 16)
#define SIGNORM_MAX    (1u << 21)
#define MAXN           1024

static const char *const SCHEME_NAME[] = { "hawk", "e8" };
static const char *const KIND_NAME[] = { "1d", "block", "signorm" };

typedef struct {
	unsigned kind;
	unsigned scheme;
	const char *variant;
	unsigned logn;
	uint32_t id_lo;
	uint32_t id_hi;
	uint64_t count;
	uint64_t seed;
	uint32_t part;
	int header;
} sample_options;

static uint64_t val_count[VAL_RANGE];
static uint64_t coord_count[E8_TAB_CLASSES][VAL_RANGE];
static uint64_t comp_count[16];
static uint64_t blocknorm_count[BLOCKNORM_MAX];
static uint64_t signorm_count[SIGNORM_MAX];

static void
put_u32le(uint8_t *dst, uint32_t x)
{
	for (unsigned u = 0; u < 4; u ++) {
		dst[u] = (uint8_t)(x >> (u << 3));
	}
}

/*
 * Label bytes of a job: kind || 0 || scheme || 0 || variant || 0 ||
 * u32le(logn) || u32le(id) || u32le(part). Returns the length.
 */
static size_t
make_label(uint8_t *buf, size_t cap, const sample_options *o,
	uint32_t id, uint32_t part)
{
	const char *parts[3] = { KIND_NAME[o->kind],
		SCHEME_NAME[o->scheme], o->variant };
	size_t len = 0;

	for (unsigned u = 0; u < 3; u ++) {
		size_t l = strlen(parts[u]);

		if (len + l + 1 + 12 > cap) {
			abort();
		}
		memcpy(buf + len, parts[u], l);
		len += l;
		buf[len ++] = 0;
	}
	put_u32le(buf + len, o->logn);
	put_u32le(buf + len + 4, id);
	put_u32le(buf + len + 8, part);
	return len + 12;
}

static void
job_stream(cmn_rng *s, const sample_options *o, uint32_t id, uint32_t part)
{
	static const char domain[] = "e8_distribution_sample v1";
	uint8_t seed[8], label[96];
	size_t label_len = make_label(label, sizeof label, o, id, part);

	for (unsigned u = 0; u < 8; u ++) {
		seed[u] = (uint8_t)(o->seed >> (u << 3));
	}
	cmn_rng_init(s, domain, sizeof domain - 1, seed, sizeof seed,
		label, label_len);
}

/*
 * The signing RNG of a job, seeded with u64le(seed) || label.
 */
static void
job_sign_rng(cmn_sign_rng *srng, const sample_options *o, uint32_t id,
	uint32_t part)
{
	uint8_t buf[104];
	size_t len;

	for (unsigned u = 0; u < 8; u ++) {
		buf[u] = (uint8_t)(o->seed >> (u << 3));
	}
	len = make_label(buf + 8, sizeof buf - 8, o, id, part);
	cmn_sign_rng_init(srng, buf, len + 8);
}

static int
count_value(int32_t v)
{
	if (v < -VAL_OFF || v >= VAL_RANGE - VAL_OFF) {
		fprintf(stderr, "ERR: value %ld out of range\n", (long)v);
		return 0;
	}
	val_count[v + VAL_OFF] ++;
	return 1;
}

static void
print_row(const sample_options *o, const char *kind, uint32_t id,
	long long value, uint64_t count)
{
	printf("%s,%s,%s,%u,%lu,%lld,%llu\n", kind, SCHEME_NAME[o->scheme],
		o->variant, o->logn, (unsigned long)id, value,
		(unsigned long long)count);
}

static void
print_values(const sample_options *o, const char *kind, uint32_t id,
	const uint64_t *counts)
{
	for (unsigned u = 0; u < VAL_RANGE; u ++) {
		if (counts[u] != 0) {
			print_row(o, kind, id, (long long)u - VAL_OFF,
				counts[u]);
		}
	}
}

static int
run_1d_hawk(const sample_options *o, const hawk_sampler_variant *v)
{
	static int8_t x[2 * MAXN > CHUNK ? 2 * MAXN : CHUNK];
	static uint8_t t[CHUNK / 8];
	size_t per_call = v->range == NULL ? (size_t)2 << o->logn : CHUNK;
	uint64_t done = 0;
	cmn_rng s;
	cmn_sign_rng srng;

	memset(t, o->id_lo ? 0xFF : 0x00, sizeof t);
	if (v->range == NULL) {
		job_sign_rng(&srng, o, o->id_lo, o->part);
	} else {
		job_stream(&s, o, o->id_lo, o->part);
	}
	while (done < o->count) {
		size_t take = per_call;
		uint32_t sn = 0;

		if (v->range == NULL) {
			(void)hawk_shim_sig_gauss(o->logn, cmn_sign_rng_hawk,
				&srng, NULL, x, t);
		} else if (!v->range(v->tables(o->logn), o->logn, x, t, 0,
			per_call, &s, &sn))
		{
			return 0;
		}
		if (o->count - done < take) {
			take = (size_t)(o->count - done);
		}
		for (size_t u = 0; u < take; u ++) {
			if (!count_value(x[u])) {
				return 0;
			}
		}
		done += take;
	}
	print_values(o, "1d", o->id_lo, val_count);
	return 1;
}

static int
run_1d_e8(const sample_options *o, const e8_sampler_variant *v)
{
	static int32_t buf[CHUNK];
	const void *tab = v->tables(o->logn);
	uint64_t done = 0;
	cmn_rng s;

	if (o->id_lo >= E8_TAB_CLASSES) {
		return 0;
	}
	job_stream(&s, o, o->id_lo, o->part);
	while (done < o->count) {
		size_t take = CHUNK;

		if (o->count - done < take) {
			take = (size_t)(o->count - done);
		}
		if (strcmp(v->name, "ref") == 0) {
			if (!e8_ref_sample_1d_seq(tab,
				e8_tab_center_num32[o->id_lo], &s, buf, take))
			{
				return 0;
			}
		} else {
			for (size_t u = 0; u < take; u ++) {
				int ok = strcmp(v->name, "hot64") == 0
					? e8_hot64_sample_1d(tab, o->id_lo, &s,
						&buf[u])
					: e8_hot32_sample_1d(tab, o->id_lo, &s,
						&buf[u]);

				if (!ok) {
					return 0;
				}
			}
		}
		for (size_t u = 0; u < take; u ++) {
			if (!count_value(buf[u])) {
				return 0;
			}
		}
		done += take;
	}
	print_values(o, "1d", o->id_lo, val_count);
	return 1;
}

static int
count_block(uint8_t tau, uint64_t n2, const e8_ca_sample_trace *tr)
{
	if (tr->component >= 16 || n2 >= BLOCKNORM_MAX) {
		fprintf(stderr, "ERR: block outside the histogram range\n");
		return 0;
	}
	comp_count[tr->component] ++;
	blocknorm_count[n2] ++;
	for (unsigned v = 0; v < 8; v ++) {
		unsigned cls = e8_tab_coord_class[tau][tr->component][v];
		int32_t c = tr->product_coords[v];

		if (c < -VAL_OFF || c >= VAL_RANGE - VAL_OFF) {
			fprintf(stderr, "ERR: coordinate out of range\n");
			return 0;
		}
		coord_count[cls][c + VAL_OFF] ++;
	}
	return 1;
}

static int
run_block(const sample_options *o, const e8_sampler_variant *v)
{
	static uint64_t n2[REF_CHUNK];
	static e8_ca_sample_trace tr[REF_CHUNK];
	const void *tab = v->tables(o->logn);
	int is_ref = strcmp(v->name, "ref") == 0;
	int is_hot64 = strcmp(v->name, "hot64") == 0;

	if (o->id_hi > 255) {
		return 0;
	}
	for (uint32_t tau = o->id_lo; tau <= o->id_hi; tau ++) {
		uint64_t done = 0;
		cmn_rng s;

		memset(comp_count, 0, sizeof comp_count);
		memset(blocknorm_count, 0, sizeof blocknorm_count);
		job_stream(&s, o, tau, o->part);
		while (done < o->count) {
			size_t take = REF_CHUNK;

			if (o->count - done < take) {
				take = (size_t)(o->count - done);
			}
			if (is_ref) {
				if (!e8_ref_sample_blocks(tab, (uint8_t)tau, &s,
					take, n2, tr))
				{
					return 0;
				}
			} else {
				for (size_t u = 0; u < take; u ++) {
					int32_t zblk[8];
					int ok = is_hot64
						? e8_hot64_sample_block(tab, zblk,
							(uint8_t)tau, &s, &n2[u],
							NULL, &tr[u])
						: e8_hot32_sample_block(tab, zblk,
							(uint8_t)tau, &s, &n2[u],
							NULL, &tr[u]);

					if (!ok) {
						return 0;
					}
				}
			}
			for (size_t u = 0; u < take; u ++) {
				if (!count_block((uint8_t)tau, n2[u], &tr[u])) {
					return 0;
				}
			}
			done += take;
		}
		for (unsigned c = 0; c < 16; c ++) {
			if (comp_count[c] != 0) {
				print_row(o, "component", tau, c,
					comp_count[c]);
			}
		}
		for (uint32_t u = 0; u < BLOCKNORM_MAX; u ++) {
			if (blocknorm_count[u] != 0) {
				print_row(o, "blocknorm", tau, u,
					blocknorm_count[u]);
			}
		}
	}
	for (unsigned c = 0; c < E8_TAB_CLASSES; c ++) {
		print_values(o, "blockcoord", c, coord_count[c]);
	}
	return 1;
}

static int
run_signorm(const sample_options *o, const hawk_sampler_variant *hv,
	const e8_sampler_variant *ev)
{
	static int8_t x[2 * MAXN];
	static uint8_t t[2 * MAXN / 8], t0[MAXN], t1[MAXN];
	static int32_t z0[MAXN], z1[MAXN];
	size_t n = (size_t)1 << o->logn;
	uint64_t target_count[256];
	cmn_rng ts;
	cmn_sign_rng srng;

	/* The fixed target: the same for every variant of the scheme. */
	job_stream(&ts, &(sample_options){ .kind = o->kind,
		.scheme = o->scheme, .variant = "target", .logn = o->logn,
		.seed = o->seed }, UINT32_MAX, 0);
	memset(target_count, 0, sizeof target_count);
	if (o->scheme == SCHEME_HAWK) {
		cmn_rng_read_bytes(&ts, t, n / 4);
		for (size_t v = 0; v < 2 * n; v ++) {
			target_count[(t[v >> 3] >> (v & 7)) & 1] ++;
		}
		if (!hawkv_select(hv, 1)) {
			return 0;
		}
	} else {
		cmn_rng_read_bytes(&ts, t0, n);
		cmn_rng_read_bytes(&ts, t1, n);
		for (size_t u = 0; u < n; u ++) {
			t0[u] &= 1;
			t1[u] &= 1;
		}
		for (size_t r = 0; r < n / 4; r ++) {
			target_count[e8_extract_tau(t0, t1, r, o->logn)] ++;
		}
		e8_sampler_set_thread_count(1);
	}
	job_sign_rng(&srng, o, 0, o->part);
	for (uint64_t k = 0; k < o->count; k ++) {
		uint64_t norm;

		if (o->scheme == SCHEME_HAWK) {
			norm = hawk_variant_sampler(o->logn, cmn_sign_rng_hawk,
				&srng, NULL, x, t);
		} else if (!ev->sample_z(ev->tables(o->logn), z0, z1, &norm,
			t0, t1, o->logn, E8_SAMPLER_RNG_PER_WORKER, NULL, 0,
			cmn_sign_rng_hawk, &srng, NULL))
		{
			return 0;
		}
		if (norm >= SIGNORM_MAX) {
			fprintf(stderr, "ERR: signature norm %llu out of"
				" range\n", (unsigned long long)norm);
			return 0;
		}
		signorm_count[norm] ++;
	}
	for (uint32_t u = 0; u < SIGNORM_MAX; u ++) {
		if (signorm_count[u] != 0) {
			print_row(o, "signorm", 0, u, signorm_count[u]);
		}
	}
	for (unsigned u = 0; u < 256 && o->part == 0; u ++) {
		if (target_count[u] != 0) {
			print_row(o, "signorm_target", u, 0, target_count[u]);
		}
	}
	return 1;
}

static int
parse_u64(const char *s, uint64_t max, uint64_t *out)
{
	char *end;
	unsigned long long v;

	if (s == NULL || *s == 0 || *s == '-') {
		return 0;
	}
	v = strtoull(s, &end, 0);
	if (*end != 0 || v > max) {
		return 0;
	}
	*out = v;
	return 1;
}

static int
parse_args(int argc, char **argv, sample_options *o)
{
	memset(o, 0, sizeof *o);
	o->kind = 3;
	o->scheme = 2;
	o->seed = 1;
	for (int i = 1; i < argc; i ++) {
		const char *a = argv[i];
		const char *v = i + 1 < argc ? argv[i + 1] : NULL;
		uint64_t x;

		if (strcmp(a, "--header") == 0) {
			o->header = 1;
			continue;
		}
		if (v == NULL) {
			return 0;
		}
		i ++;
		if (strcmp(a, "--kind") == 0) {
			for (o->kind = 0; o->kind < 3; o->kind ++) {
				if (strcmp(v, KIND_NAME[o->kind]) == 0) {
					break;
				}
			}
		} else if (strcmp(a, "--scheme") == 0) {
			if (strcmp(v, "hawk") == 0) {
				o->scheme = SCHEME_HAWK;
			} else if (strcmp(v, "e8") == 0) {
				o->scheme = SCHEME_E8;
			} else {
				return 0;
			}
		} else if (strcmp(a, "--variant") == 0) {
			o->variant = v;
		} else if (strcmp(a, "--logn") == 0) {
			if (!parse_u64(v, 10, &x) || x < 8) {
				return 0;
			}
			o->logn = (unsigned)x;
		} else if (strcmp(a, "--id") == 0) {
			if (!parse_u64(v, 255, &x)) {
				return 0;
			}
			o->id_lo = o->id_hi = (uint32_t)x;
		} else if (strcmp(a, "--ids") == 0) {
			char lo[8];
			const char *dash = strchr(v, '-');
			uint64_t y;

			if (dash == NULL || (size_t)(dash - v) >= sizeof lo) {
				return 0;
			}
			memcpy(lo, v, (size_t)(dash - v));
			lo[dash - v] = 0;
			if (!parse_u64(lo, 255, &x)
				|| !parse_u64(dash + 1, 255, &y) || y < x)
			{
				return 0;
			}
			o->id_lo = (uint32_t)x;
			o->id_hi = (uint32_t)y;
		} else if (strcmp(a, "--count") == 0) {
			if (!parse_u64(v, UINT64_C(1) << 40, &o->count)) {
				return 0;
			}
		} else if (strcmp(a, "--seed") == 0) {
			if (!parse_u64(v, UINT64_MAX, &o->seed)) {
				return 0;
			}
		} else if (strcmp(a, "--part") == 0) {
			if (!parse_u64(v, UINT32_MAX, &x)) {
				return 0;
			}
			o->part = (uint32_t)x;
		} else {
			return 0;
		}
	}
	return o->kind < 3 && o->scheme < 2 && o->variant != NULL
		&& o->logn != 0 && o->count != 0
		&& (o->kind == KIND_BLOCK || o->id_lo == o->id_hi)
		&& (o->kind != KIND_BLOCK || o->scheme == SCHEME_E8);
}

int
main(int argc, char **argv)
{
	sample_options o;
	const hawk_sampler_variant *hv = NULL;
	const e8_sampler_variant *ev = NULL;
	int ok;

	if (!parse_args(argc, argv, &o)) {
		fprintf(stderr, "usage: %s --kind 1d|block|signorm"
			" --scheme hawk|e8 --variant NAME --logn 8|9|10"
			" [--id I | --ids A-B] --count N [--seed X] [--part P]"
			" [--header]\n", argv[0]);
		return 2;
	}
	if (o.scheme == SCHEME_HAWK) {
		hv = hawk_sampler_variant_find(o.variant);
		if (hv == NULL || (o.kind == KIND_1D && o.id_lo > 1)) {
			fprintf(stderr, "ERR: unknown HAWK variant %s or"
				" parity\n", o.variant);
			return 2;
		}
	} else {
		ev = e8_sampler_variant_find(o.variant);
		if (ev == NULL) {
			fprintf(stderr, "ERR: unknown E8 variant %s\n",
				o.variant);
			return 2;
		}
	}
	if (o.header) {
		printf("kind,scheme,variant,logn,id,value,count\n");
	}
	switch (o.kind) {
	case KIND_1D:
		ok = hv != NULL ? run_1d_hawk(&o, hv) : run_1d_e8(&o, ev);
		break;
	case KIND_BLOCK:
		ok = run_block(&o, ev);
		break;
	default:
		ok = run_signorm(&o, hv, ev);
		break;
	}
	if (!ok) {
		fprintf(stderr, "ERR: sampling failed kind=%s scheme=%s"
			" variant=%s logn=%u id=%u\n", KIND_NAME[o.kind],
			SCHEME_NAME[o.scheme], o.variant, o.logn,
			(unsigned)o.id_lo);
		return 1;
	}
	return 0;
}
