#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/hawk_e8_inner.h"
#include "e8_tables_ref.h"

/*
 * Structural constant-time check of the E8 ref sampler, linked with the
 * E8_REF_COUNT_OPS build of e8_sampler_ref.c, whose loops count their
 * iterations. Every block, for every tau and every random input, must run
 * exactly the same loop counts:
 *
 *   ops[0]  masked tau scan             256
 *   ops[1]  masked row-class selection  E8_REF_ROW_CLASSES * 15
 *   ops[2]  threshold comparisons       15 + 8 * E8_REF_1D_LEN
 *   ops[3]  masked residue selection    8 * 4 * E8_REF_1D_LEN
 *
 * It also checks the draw format's byte count: a stream of b blocks
 * delivers ceil(9b / 4) * 40 bytes. This shows that no loop exits early
 * and that no table scan depends on secret data; that the remaining code
 * has no secret-dependent branch or memory index rests on code review
 * (valgrind / ctgrind is not available here).
 */

#define MAXN   1024

static uint64_t
lcg_next(uint64_t *state)
{
	*state = *state * UINT64_C(6364136223846793005)
		+ UINT64_C(1442695040888963407);
	return *state;
}

int
main(void)
{
	static uint8_t t0[MAXN], t1[MAXN];
	static int32_t z0[MAXN], z1[MAXN];
	const uint64_t expect[4] = {
		256, E8_REF_ROW_CLASSES * 15, 15 + 8 * E8_REF_1D_LEN,
		8 * 4 * E8_REF_1D_LEN
	};
	uint64_t state = UINT64_C(0xE8C7C7C700000000);
	unsigned blocks = 0;

	for (unsigned logn = 8; logn <= 10; logn ++) {
		size_t n = (size_t)1 << logn, k = n >> 2;
		const void *tab = e8_ref_tables_get(logn);
		uint64_t ops[4];

		(void)e8_ref_ops_get(ops);
		for (unsigned rep = 0; rep < 8; rep ++) {
			uint8_t seed[8];
			cmn_rng s;

			for (size_t u = 0; u < n; u ++) {
				t0[u] = (uint8_t)(lcg_next(&state) >> 63);
				t1[u] = (uint8_t)(lcg_next(&state) >> 63);
			}
			for (unsigned u = 0; u < 8; u ++) {
				seed[u] = (uint8_t)(lcg_next(&state) >> 56);
			}
			cmn_rng_init(&s, "test_e8_ref_ct", 14, seed, 8, NULL, 0);
			for (size_t r = 0; r < k; r ++) {
				uint64_t norm;

				if (!e8_ref_sample_stream(tab, z0, z1, &norm,
					t0, t1, logn, r, r + 1, &s)
					|| !e8_ref_ops_get(ops))
				{
					fprintf(stderr, "ERR: ref sampling\n");
					return 1;
				}
				for (unsigned i = 0; i < 4; i ++) {
					if (ops[i] != expect[i]) {
						fprintf(stderr, "ERR: ref block"
							" loop count %u: %llu,"
							" expected %llu (tau=%u)\n",
							i, (unsigned long long)
							ops[i],
							(unsigned long long)
							expect[i],
							(unsigned)e8_extract_tau(
							t0, t1, r, logn));
						return 1;
					}
				}
				blocks ++;
			}

			/* Draw format: ceil(9b / 4) groups of 40 bytes. */
			for (size_t b = 1; b <= 9; b ++) {
				cmn_rng_counters c;
				uint64_t norm;

				cmn_rng_init(&s, "test_e8_ref_ct", 14, seed, 8,
					NULL, 0);
				if (!e8_ref_sample_stream(tab, z0, z1, &norm,
					t0, t1, logn, 0, b, &s))
				{
					return 1;
				}
				(void)e8_ref_ops_get(ops);
				cmn_rng_counters_get(&s, &c);
				if (c.bytes_delivered != (9 * b + 3) / 4 * 40) {
					fprintf(stderr, "ERR: ref bytes for %u"
						" blocks: %llu\n", (unsigned)b,
						(unsigned long long)
						c.bytes_delivered);
					return 1;
				}
			}
		}
	}

	/* Every tau, many draws. */
	for (unsigned tau = 0; tau < 256; tau ++) {
		for (unsigned rep = 0; rep < 16; rep ++) {
			uint64_t ops[4], norm;
			uint8_t seed[3] = { (uint8_t)tau, (uint8_t)rep, 1 };
			cmn_rng s;

			for (unsigned u = 0; u < 4; u ++) {
				t0[u * 64] = (uint8_t)((tau >> u) & 1);
				t1[u * 64] = (uint8_t)((tau >> (u + 4)) & 1);
			}
			cmn_rng_init(&s, "test_e8_ref_ct", 14, seed, 3, NULL, 0);
			if (e8_extract_tau(t0, t1, 0, 8) != tau
				|| !e8_ref_sample_stream(e8_ref_tables_get(8),
				z0, z1, &norm, t0, t1, 8, 0, 1, &s)
				|| !e8_ref_ops_get(ops)
				|| memcmp(ops, expect, sizeof ops) != 0)
			{
				fprintf(stderr, "ERR: ref loop counts tau=%u\n",
					tau);
				return 1;
			}
			blocks ++;
		}
	}
	printf("E8 ref constant-time structure: %u blocks, every block"
		" ran %llu tau scans, %llu row-class entries, %llu"
		" comparisons, %llu residue entries; ceil(9b/4)*40 bytes"
		" per stream of b blocks: done.\n", blocks,
		(unsigned long long)expect[0], (unsigned long long)expect[1],
		(unsigned long long)expect[2], (unsigned long long)expect[3]);
	return 0;
}
