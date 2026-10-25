#ifndef CMN_CSV_H__
#define CMN_CSV_H__

#include <stdint.h>
#include <stdio.h>

/*
 * Minimal CSV writer for the benchmark outputs. The header is written once,
 * before the first row. Fields are never quoted: a field containing a
 * comma, a double quote or a line break is a programming error (assert).
 * An unavailable value (CMN_UNAVAILABLE for numbers, NULL for strings) is
 * written as an empty field. Each row must have exactly as many fields as
 * the header (assert).
 */

typedef struct {
	FILE *fp;
	const char *const *columns;
	unsigned column_count;
	unsigned field;
	int header_written;
} cmn_csv;

void cmn_csv_init(cmn_csv *csv, FILE *fp,
	const char *const *columns, unsigned column_count);

/*
 * Write the header if it has not been written yet; rows do this too.
 * cmn_csv_skip_header() marks it written without writing it (for output
 * appended to an existing file).
 */
void cmn_csv_header(cmn_csv *csv);

void cmn_csv_skip_header(cmn_csv *csv);

void cmn_csv_str(cmn_csv *csv, const char *value);

void cmn_csv_u64(cmn_csv *csv, uint64_t value);

void cmn_csv_empty(cmn_csv *csv);

void cmn_csv_end_row(cmn_csv *csv);

/*
 * The per-signature schema (plan.md, Step 7), one row per signature or
 * setup operation:
 *
 *   kind               sign, sign_failed, keygen or prepare
 *   run_id             identifier of the benchmark run
 *   scheme             hawk or e8
 *   variant            sampler variant (ref, hot64, ...)
 *   threshold_bits     sampler threshold width
 *   technique          ct_full_scan, vt_hot_table, ...
 *   threads            sampler threads (1 = serial)
 *   logn               degree
 *   key_index          key of the row
 *   msg_index          message of the row
 *   attempts           signing attempts
 *   sig_bytes          signature size
 *   rng_bytes          random bytes consumed (top-level RNG plus sampler
 *                      streams)
 *   tsc_cycles         TSC cycles of the timed call
 *   core_cycles        core cycles, all threads (empty if unavailable)
 *   wall_ns            wall-clock time
 *   cpu_ns_all_threads CPU time, caller plus pool jobs (empty if
 *                      unavailable)
 *   accounting         all_in or prepared (empty for keygen)
 *   encoding_included  1 if the timed call includes signature encoding
 *   verify_ok          1 if the signature verifies (empty for setup rows)
 *
 * Numeric fields set to CMN_UNAVAILABLE and string fields set to NULL are
 * written empty.
 */
#define CMN_CSV_SIGN_COLUMN_COUNT   20

extern const char *const cmn_csv_sign_columns[CMN_CSV_SIGN_COLUMN_COUNT];

typedef struct {
	const char *kind;
	const char *run_id;
	const char *scheme;
	const char *variant;
	uint64_t threshold_bits;
	const char *technique;
	uint64_t threads;
	uint64_t logn;
	uint64_t key_index;
	uint64_t msg_index;
	uint64_t attempts;
	uint64_t sig_bytes;
	uint64_t rng_bytes;
	uint64_t tsc_cycles;
	uint64_t core_cycles;
	uint64_t wall_ns;
	uint64_t cpu_ns_all_threads;
	const char *accounting;
	uint64_t encoding_included;
	uint64_t verify_ok;
} cmn_sign_row;

/*
 * Set every field of a row to unavailable.
 */
void cmn_sign_row_clear(cmn_sign_row *row);

void cmn_csv_sign_init(cmn_csv *csv, FILE *fp);

void cmn_csv_sign_row(cmn_csv *csv, const cmn_sign_row *row);

#endif
