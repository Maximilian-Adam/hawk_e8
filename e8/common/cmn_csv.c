#include <assert.h>
#include <string.h>

#include "cmn_csv.h"
#include "cmn_timing.h"

/* see cmn_csv.h */
const char *const cmn_csv_sign_columns[CMN_CSV_SIGN_COLUMN_COUNT] = {
	"kind", "run_id", "scheme", "variant", "threshold_bits",
	"technique", "threads", "logn", "key_index", "msg_index",
	"attempts", "sig_bytes", "rng_bytes",
	"tsc_cycles", "core_cycles", "wall_ns", "cpu_ns_all_threads",
	"accounting", "encoding_included", "verify_ok",
	"mode", "proc", "flags", "batch"
};

static void
cmn_csv_check_field(const char *s)
{
	assert(strpbrk(s, ",\"\r\n") == NULL);
	(void)s;
}

/* see cmn_csv.h */
void
cmn_csv_init(cmn_csv *csv, FILE *fp,
	const char *const *columns, unsigned column_count)
{
	csv->fp = fp;
	csv->columns = columns;
	csv->column_count = column_count;
	csv->field = 0;
	csv->header_written = 0;
}

/* see cmn_csv.h */
void
cmn_csv_header(cmn_csv *csv)
{
	if (csv->header_written) {
		return;
	}
	csv->header_written = 1;
	for (unsigned u = 0; u < csv->column_count; u ++) {
		cmn_csv_check_field(csv->columns[u]);
		fprintf(csv->fp, "%s%s", u == 0 ? "" : ",", csv->columns[u]);
	}
	fputc('\n', csv->fp);
}

/* see cmn_csv.h */
void
cmn_csv_skip_header(cmn_csv *csv)
{
	csv->header_written = 1;
}

static void
cmn_csv_sep(cmn_csv *csv)
{
	if (csv->field == 0) {
		cmn_csv_header(csv);
	} else {
		fputc(',', csv->fp);
	}
	assert(csv->field < csv->column_count);
	csv->field ++;
}

/* see cmn_csv.h */
void
cmn_csv_str(cmn_csv *csv, const char *value)
{
	cmn_csv_sep(csv);
	if (value != NULL) {
		cmn_csv_check_field(value);
		fputs(value, csv->fp);
	}
}

/* see cmn_csv.h */
void
cmn_csv_u64(cmn_csv *csv, uint64_t value)
{
	cmn_csv_sep(csv);
	if (value != CMN_UNAVAILABLE) {
		fprintf(csv->fp, "%llu", (unsigned long long)value);
	}
}

/* see cmn_csv.h */
void
cmn_csv_empty(cmn_csv *csv)
{
	cmn_csv_sep(csv);
}

/* see cmn_csv.h */
void
cmn_csv_end_row(cmn_csv *csv)
{
	assert(csv->field == csv->column_count);
	fputc('\n', csv->fp);
	csv->field = 0;
}

/* see cmn_csv.h */
void
cmn_sign_row_clear(cmn_sign_row *row)
{
	row->kind = NULL;
	row->run_id = NULL;
	row->scheme = NULL;
	row->variant = NULL;
	row->threshold_bits = CMN_UNAVAILABLE;
	row->technique = NULL;
	row->threads = CMN_UNAVAILABLE;
	row->logn = CMN_UNAVAILABLE;
	row->key_index = CMN_UNAVAILABLE;
	row->msg_index = CMN_UNAVAILABLE;
	row->attempts = CMN_UNAVAILABLE;
	row->sig_bytes = CMN_UNAVAILABLE;
	row->rng_bytes = CMN_UNAVAILABLE;
	row->tsc_cycles = CMN_UNAVAILABLE;
	row->core_cycles = CMN_UNAVAILABLE;
	row->wall_ns = CMN_UNAVAILABLE;
	row->cpu_ns_all_threads = CMN_UNAVAILABLE;
	row->accounting = NULL;
	row->encoding_included = CMN_UNAVAILABLE;
	row->verify_ok = CMN_UNAVAILABLE;
	row->mode = NULL;
	row->proc = CMN_UNAVAILABLE;
	row->flags = NULL;
	row->batch = CMN_UNAVAILABLE;
}

/* see cmn_csv.h */
void
cmn_csv_sign_init(cmn_csv *csv, FILE *fp)
{
	cmn_csv_init(csv, fp, cmn_csv_sign_columns,
		CMN_CSV_SIGN_COLUMN_COUNT);
}

/* see cmn_csv.h */
void
cmn_csv_sign_row(cmn_csv *csv, const cmn_sign_row *row)
{
	cmn_csv_str(csv, row->kind);
	cmn_csv_str(csv, row->run_id);
	cmn_csv_str(csv, row->scheme);
	cmn_csv_str(csv, row->variant);
	cmn_csv_u64(csv, row->threshold_bits);
	cmn_csv_str(csv, row->technique);
	cmn_csv_u64(csv, row->threads);
	cmn_csv_u64(csv, row->logn);
	cmn_csv_u64(csv, row->key_index);
	cmn_csv_u64(csv, row->msg_index);
	cmn_csv_u64(csv, row->attempts);
	cmn_csv_u64(csv, row->sig_bytes);
	cmn_csv_u64(csv, row->rng_bytes);
	cmn_csv_u64(csv, row->tsc_cycles);
	cmn_csv_u64(csv, row->core_cycles);
	cmn_csv_u64(csv, row->wall_ns);
	cmn_csv_u64(csv, row->cpu_ns_all_threads);
	cmn_csv_str(csv, row->accounting);
	cmn_csv_u64(csv, row->encoding_included);
	cmn_csv_u64(csv, row->verify_ok);
	cmn_csv_str(csv, row->mode);
	cmn_csv_u64(csv, row->proc);
	cmn_csv_str(csv, row->flags);
	cmn_csv_u64(csv, row->batch);
	cmn_csv_end_row(csv);
}
