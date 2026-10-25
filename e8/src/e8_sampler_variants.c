#include "hawk_e8_inner.h"

#if HAWK_ENABLE_E8_EXPERIMENTAL

#include <string.h>

/*
 * Registry of the E8 sampler variants (see hawk_e8_inner.h). Every
 * variant is compiled into the same binary from its own translation unit
 * with its own symbols; the signer calls the selected variant through
 * sample_z, one indirect call per attempt. Integer-only.
 */

const e8_sampler_variant e8_sampler_variants[] = {
	{ "ref", 78, "ct_full_scan", e8_sample_z_ref,
		e8_ref_tables_get, e8_ref_sample_stream },
	{ "hot64", 64, "vt_hot_table", e8_sample_z_hot64,
		e8_hot64_tables_get, e8_hot64_sample_stream },
	{ "hot32", 32, "vt_hot_table", e8_sample_z_hot32,
		e8_hot32_tables_get, e8_hot32_sample_stream }
};

const size_t e8_sampler_variant_count =
	sizeof e8_sampler_variants / sizeof e8_sampler_variants[0];

/* see hawk_e8_inner.h */
const e8_sampler_variant *
e8_sampler_variant_find(const char *name)
{
	if (name == NULL) {
		return NULL;
	}
	for (size_t u = 0; u < e8_sampler_variant_count; u ++) {
		if (strcmp(e8_sampler_variants[u].name, name) == 0) {
			return &e8_sampler_variants[u];
		}
	}
	return NULL;
}

/* see hawk_e8_inner.h */
const e8_sampler_variant *
e8_sampler_variant_default(void)
{
	return &e8_sampler_variants[1];
}

#endif
