# Comparison harness (plan.md Step 7)

One harness times every sampler variant of both schemes under identical
conditions, records the environment, and turns the per-signature CSVs into
the statistics and tables of Step 10.  The accounting (what each timed call
contains) and the per-signature CSV schema are defined in
[`perf_comparison_final.md`](perf_comparison_final.md).  This document
covers the harness, its builds, the run layout and the analysis.

| piece | file | role |
| --- | --- | --- |
| harness | `compare/bench_compare.c` | every (scheme, variant, threads, logn) configuration, interleaved; latency and throughput modes; pinning |
| signing core | `compare/bench_sign_core.{c,h}` | keygen, preparation, the timed `sign_finish` of every variant, verification, seeds, messages, row fields; shared with `e8_sign_bench` |
| pool affinity | `common/cmn_pool.{c,h}` | `cmn_pool_set_affinity()`, `cmn_pin_current_thread()` (both schemes alike) |
| environment | `compare/env_capture.sh` | `env.txt` beside every run's CSVs |
| preflight | `compare/preflight.sh` | machine-state checks before every run (Step 8) |
| CPU lists | `compare/ccd_cpus.sh` | Step 9's pinned CPU lists from the L3 topology (Step 8) |
| driver | `compare/run_compare.sh` | run sets; one run directory per run and flag set, then the analysis |
| analysis | `compare/analysis/analyze.py`, `compare_data.py`, `compare_tables.py` | statistics, ratios with bootstrap CIs, joins, acceptance checks, LaTeX and Markdown tables |
| sampler only | `compare/e8_sampler_bench.c --variant-matrix` | Step 10 table 3 |
| stages | `e8_sign_profile_bench`, `hawk_sign_profile_bench` (`--stage-csv`) | Step 10 table 6 |

## Running

```sh
make bench-compare-smoke   # Step 8: every configuration of both flag sets at
                           # small counts, the analysis self-test, the analysis
make bench-compare         # one run at Step 9's volume (10 keys x 1,000
                           # signatures per configuration), flag set O2
```

Both forward to `e8/` and call `compare/run_compare.sh`; make variables
`COMPARE_FLAGSETS` (default `O2`), `COMPARE_RUNS` (default 1) and
`COMPARE_ARGS` (passed to the driver) select more.  Step 9's two run sets
(the exact commands, machine setup and checks: the run book,
[`step9_runbook.md`](step9_runbook.md)):

```sh
make bench-compare COMPARE_FLAGSETS="O2 O3native" COMPARE_RUNS=5 \
  COMPARE_ARGS="--run-set headline --cpus $(sh e8/compare/ccd_cpus.sh headline) --name step9-headline"
make bench-compare COMPARE_FLAGSETS="O2 O3native" COMPARE_RUNS=5 \
  COMPARE_ARGS="--run-set cross-ccd --cpus $(sh e8/compare/ccd_cpus.sh cross-ccd) --name step9-cross-ccd"
```

Driver options: `--smoke`, `--run-set all|headline|cross-ccd` (below),
`--flagsets`, `--runs R` (runs and flag sets alternate: r1/O2,
r1/O3native, r2/O2, ...), `--keys`, `--sigs-per-key`, `--procs`, `--cpus`,
`--threads`, `--logn`, `--warmup`, `--sampler-trials`, `--stage-trials`,
`--bootstrap`, `--name` (the analysis directory; default the first run's
`run_id`, prefixed `RUNSET-` for the Step 9 run sets), `--no-analysis`,
`--force` (run despite a failed preflight; recorded), `--freq-tolerance
PCT` (the preflight's frequency check, default 1).  Defaults: smoke 2 keys
x 4 signatures, `P = 4`, 50 sampler trials, 20 stage trials, 200 bootstrap
resamples; full 10 x 1,000, `P = 4`, 2,000, 1,000, 2,000.

Run sets (`--run-set`; written as `run_set` to `env.txt` and
`commands.txt`):

| run set | steps | `T` (default) | `P` (default) | `--cpus` |
| --- | --- | --- | --- | --- |
| `all` (default) | all six | 1, 2, 4, 8, 16 | 4 | optional |
| `headline` | all six | 1, 2, 4, 8 | `\|--cpus\|` | required (not for `--smoke`) |
| `cross-ccd` | latency only | 1, 2, 4, 8, 16 | none | required (not for `--smoke`) |

`all` is the smoke run's and any ad hoc run's; `headline` and `cross-ccd`
are Step 9's (plan.md, decided before Step 8).  `--threads` and `--procs`
override the defaults.  Before any step the driver refuses (exit 2) a
`--cpus` list that is not a comma-separated list of CPU numbers, repeats a
CPU, or has fewer CPUs than the largest `T` (latency) or than `P`
(throughput), so a run cannot fail in its first step after the others
have run.  The sampler matrix runs with `--threads 1` (Table 3 uses
`T = 1` only; its default 1, 8, 16 put 16 spinning threads on an 8-CPU
list) and, like the stage benches, is pinned to the list's first CPU.

Preflight (`compare/preflight.sh --cpus LIST --binary BIN [--repo DIR]
[--freq-tolerance PCT] [--capture FILE]`): before every run (and before
its directory exists) the driver checks, through `env_capture.sh` taken
while a busy loop holds each pinned CPU: boost off; governor `performance`
on every pinned CPU; SMT off; no two pinned CPUs SMT siblings;
`perf_event_paranoid <= 2` with a working core-cycle counter; the pinned
CPUs' current frequencies within 1%; a clean tree.  Each condition is
`ok`, `FAIL` or `unavailable`.  A non-smoke run starts only if every
condition is `ok`, otherwise the driver exits 3 (refused) unless `--force`
is given; smoke runs go on.  `env.txt` and `commands.txt` record
`preflight: passed`, `failed (forced: --force)` or `failed (smoke run, not
enforced)` and every `preflight_NAME: STATUS (detail)` line.  On WSL
boost, governor and frequency are `unavailable` and SMT is on, so only
smoke runs pass without `--force`.  `--capture FILE` checks a given
capture instead (used to test the checks on WSL).

Exit status of the driver: 0 if every step succeeded and the analysis's
checks passed; 1 if a step or a check failed (each failed step is reported
with its exit status, and `commands.txt` records every step's command,
start, duration and exit status); 2 on a usage or configuration error; 3
if the preflight refused a run.

`bench_compare` itself: `--mode latency|throughput`, `--keys K`,
`--sigs-per-key M`, `--warmup W`, `--logn LIST`, `--threads LIST` (default
1,2,4,8,16), `--variant NAME`..., `--scheme hawk|e8`..., `--procs P`,
`--cpus LIST`, `--run-id ID`, `--out FILE`, `--list` (the configurations in
schedule order), `--build-info` (flag set, flags, compiler, core-cycle
counter).  Exit status 1 on any signing or verification failure.

## Schedule

- **Keys.**  K keys per (scheme, logn), generated and prepared once at
  the start, before any timed call (key k of a scheme: signing-RNG stream
  `1 + scheme`, index k, as in `e8_sign_bench`).  Every variant and thread
  count of the scheme signs with the same keys.  `keygen` and `prepare`
  rows are written once per key (variant and threads empty).
- **Messages.**  Signature m of key k is message `i = k M + m` at the
  degree, signed with signing-RNG stream `16 + accounting`, index i.  Both
  schemes and every variant sign the same messages with the same streams;
  the signatures are those of `e8_sign_bench` with `--sigs-per-key M`
  (checked: identical attempts and `rng_bytes` on all 8,400 signatures of
  a 10 x 20 run).
- **Groups.**  Configurations are grouped by thread count `T`.  Before a
  group the pool is resized to `T - 1` workers (`cmn_pool_shrink(0)`, then
  `cmn_pool_ensure(T - 1)`), and after it joined; no timed call starts or
  joins a thread (the harness checks the pool statistics and stops
  otherwise), and serial configurations never run beside spinning workers.
- **Warm-up.**  At the start of each group, W (default 1) signatures per
  configuration and accounting mode (key 0, message `0xFFFFFF`, stream 9),
  measured, verified and written as `warmup` rows; never in a result.
- **Interleaving.**  Within a group the schedule is round-robin per batch:
  for `k = 0 .. K-1`, for every configuration A, B, C, ... of the group (in
  `--list` order: degree, scheme, variant), one batch = key k's M
  signatures, each under `all_in` and then `prepared`.  Drift spreads over
  every configuration of the group; the `batch` column records the
  position.
- **Verification.**  Every signature (and warm-up) is verified with the
  scheme's verifier outside the timed region; failures are counted,
  written (`sign_failed`, `verify_ok` 0), reported and make the exit
  status 1.

## Modes

- **latency**: one signer process, `T` sampler threads (the caller and
  `T - 1` pool workers).  `T` in `--threads` (default 1, 2, 4, 8, 16);
  upstream HAWK `ref` only at `T = 1`.  93 configurations with the
  defaults (3 degrees x (HAWK: ref + 3 x 5, E8: 3 x 5)).
- **throughput**: `P` (`--procs`, default 2; driver default 4) independent
  single-thread signer processes, forked after key generation (they share
  the keys), each running the same `T = 1` schedule (21 configurations),
  released together by a start barrier.  Rows carry `proc`; each process
  writes its own temporary CSV and summary, which the parent appends in
  process order.  Throughput per core is derived from the rows (analysis):
  per process `count / sum(wall_ns)` signatures per second of signing time,
  averaged over the processes.

## Pinning

`--cpus LIST` (recorded in the summary and by `env_capture.sh`):

- latency: the caller is pinned to `LIST[0]` (`cmn_pin_current_thread()`)
  and pool worker `w` to `LIST[w mod |LIST|]`
  (`cmn_pool_set_affinity()`, applied when the list is set and at thread
  creation through `pthread_attr_setaffinity_np`, so a worker never runs
  elsewhere, and costing nothing per dispatch); the harness requires
  `|LIST| >= max T`, so the `T` threads run on distinct CPUs;
- throughput: process `p` is pinned to `LIST[p]`; `|LIST| >= P`;
- the sampler and stage benches (one thread each) run under `taskset -c
  LIST[0]`.

The pool affinity is a shared-module addition used identically by both
schemes; without a list nothing changes.  `test_cmn_common` checks pinning
at creation, re-pinning of live threads, rejection of invalid CPUs and
clearing (its output is unchanged).

## Builds

Fairness item 6: one compiler, one flag set for every object of a timed
binary, and the E8 debug checks off.  Each flag set `FS` has its own object
directory `e8/build/bench-FS/` with every object (upstream HAWK, the
shim, the HAWK variants, the shared modules, E8 with
`-DHAWK_E8_DEBUG_CHECKS=0`, the signing core); the default objects and
`CFLAGS` are unchanged.  `make bench-builds` (part of `make`) builds, per
flag set:

| binary | contents |
| --- | --- |
| `bin/bench_compare-FS` | the harness |
| `bin/bench_compare_avx2-FS` | HAWK AVX2 context: upstream `Optimized_Implementation/avx2/` (generated by `build.py avx2`, unmodified) compiled into `build/bench-FS/avx2/` with its own shim, shared modules and signing core (`-DBSC_HAWK_ONLY=1`); HAWK only, variant `ref-avx2` |
| `bin/e8_sign_bench-FS` | `e8_sign_bench` |
| `bin/e8_sampler_bench-FS` | sampler-only timing |
| `bin/e8_sign_profile_bench-FS`, `bin/hawk_sign_profile_bench-FS` | stage breakdown (profile timers on) |

Flag sets (`BENCH_CFLAGS_FS` in `e8/Makefile`; every object also gets
`-pthread`, E8 objects `-DHAWK_ENABLE_E8_EXPERIMENTAL=1
-DHAWK_E8_DEBUG_CHECKS=0`):

- `O2`: `-Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always`
  (upstream's reference flags);
- `O3native`: `-Wall -Wextra -Wshadow -Wundef -O3 -march=native
  -fno-strict-aliasing -fdiagnostics-color=always`.

`-fno-strict-aliasing` is there because upstream `hawk_sign.c` has
undefined behaviour that `-O3 -march=native` (GCC 15.2, `znver5`) exposes:
every HAWK signature of the first smoke run failed verification, and so
does upstream's own `test_self` (`8: verify error`).  Root cause: with
`HAWK_LE && HAWK_UNALIGNED` (the x86-64 default) the reference code reads
and writes byte buffers through casted pointers (`dec32le`, `dec64le`,
`enc64le`, `bp_xor_*`).  In the GF(2) Karatsuba multiply that computes the
target coset mod 2, `bp_muladd_128` writes `t2` as `uint64_t`
(`bp_xor_64`) and `bp_muladd_64` then reads the same bytes as `uint32_t`
(`dec32le`), a strict-aliasing violation.  GCC may assume the two do not
alias, and once its basic-block (SLP) vectorizer turns `bp_xor_64` and
`enc64le` into vector stores it reorders them.  Evidence (scratch builds of
upstream `hawk_sign.c`, every other object at `-O2`):

- only `hawk_sign.c` breaks; only with AVX-512 plus Zen tuning (`-march=
  znver4` / `znver5`, or `x86-64-v4 -mtune=znver5`; not `x86-64-v4`
  alone, `-mtune=generic` or `-mavx2`);
- `-fno-tree-slp-vectorize` fixes it, `-fno-tree-loop-vectorize` does not;
  bisecting with `-fdbg-cnt=vect_slp` gives the first two SLP regions, both
  in `bp_muladd_128` (the inlined `bp_xor_64` and `enc64le`), needed
  together;
- strict aliasing kept on, `-DHAWK_UNALIGNED=0` (byte-wise access) fixes
  it, and so does replacing only the four `dec32le` reads in
  `bp_muladd_64` with `memcpy`.  So it is a source bug (undefined
  behaviour), not a compiler bug; `-O2` happens to compile it as
  intended.  UBSan does not check strict aliasing.

The AVX2 `hawk_sign.c` multiplies with carry-less-multiply instructions
instead and has no `bp_muladd_64`; its signatures verify at every flag
set.  Cost of the flag: at O3native, E8 `ref` +1.2 to +1.5%, E8 `hot64` /
`hot32` within 0.6%; at O2 (where both builds work), both schemes within
+-1.5% (`ref` pair +0.3 to +0.6% on both sides).  It applies to every
object of the flag set, both schemes alike.  Recheck on the Step 9
machine's compiler.  The harness verifies every signature, so a broken
build cannot produce a timing silently.

`build/bench-FS/bench_build_info.h` (regenerated only when its content
changes) holds the flag-set name and the full flags.  Every object of the
flag set depends on it, so a flag change rebuilds the set.  It is printed
by `--build-info`, recorded in `env.txt` and written to every row's
`flags` column.

## Output layout

```text
e8/results/compare/
  FS/RUN_ID/                one harness run (self-describing)
    env.txt                 env_capture.sh, then run_set and the preflight
    commands.txt            run parameters (run_set, steps, threads, ...),
                            the preflight, every command, start time,
                            duration, exit status
    latency.csv/.txt        bench_compare-FS --mode latency (CSV / summary)
    throughput.csv/.txt     bench_compare-FS --mode throughput
    latency_avx2.csv/.txt   bench_compare_avx2-FS (context)
    sampler.csv/.txt        e8_sampler_bench-FS --variant-matrix
    stages_hawk.csv/.txt    hawk_sign_profile_bench-FS --stage-csv
    stages_e8.csv/.txt      e8_sign_profile_bench-FS --variant ref --threads 1 --stage-csv
  analysis/NAME/            analyze.py over one or more run directories
    summary.csv runs.csv ratios.csv acceptance.txt tables.md tables.tex
```

The sampler and stage CSVs carry no `run_id`; the directory does.  Copy
anything worth committing (the final tables, a reference summary) to
`e8/docs/`.

## Environment capture

`compare/env_capture.sh --out FILE [--run-id ID] [--cpus LIST] [--binary
BIN] [--cc CC] [--repo DIR]` writes `key: value` lines: `run_id`,
`captured_utc`, `hostname`, `kernel`, `wsl`, `cpu_model`, `microcode`,
`cpus_online`, `cpus_pinned`, `isolated_cpus`, `nohz_full`, `governor`,
`scaling_driver`, `cur_freq_khz` (per pinned CPU), `boost`
(`cpufreq/boost`, `intel_pstate/no_turbo` or `amd_pstate/cpb_boost`),
`smt_control`, `smt_active`, `thread_siblings`, `l3_id` and `l3_size` (per
pinned CPU: which CCD and whether it has the V-cache), `perf_event_paranoid`,
`core_cycles` (the binary's counter status), `flags`, `cflags`,
`compiler_built` (from the binary's `--build-info`), `cc`, `cc_version`,
`march_native` (what `-march=native` resolves to), `binary`, `git_commit`,
`git_dirty` (tracked files only), `loadavg`.  Anything it cannot read is
`unavailable`; it never fails.  The driver appends `run_set`, `preflight`
and the `preflight_NAME` lines (Step 8).  On WSL: governor, scaling driver, frequency
and boost are unavailable, microcode reads `0xffffffff`, SMT is on.

## Analysis

`python3 -I compare/analysis/analyze.py [--out-dir DIR] [--bootstrap B]
[--seed S] [--run-tolerance F] [--core-tsc-tolerance F] [--precision CSV]
[--validation CSV] [--no-fail] PATH...` (standard library, no bytecode
written).  `PATH`: run directories, or single CSVs in the schema (an
`e8_sign_bench` output works; missing Step 7 columns default to `mode =
latency`); `NAME.csv.gz` is read like `NAME.csv`.  `--self-test` checks
the bootstrap against brute-force resampling, determinism, and that the
checks pass on good synthetic runs and fail on a verification failure, a
missing row, a 20% slower run, a truncated CSV and a forced preflight,
and that a missing run directory and mixed run sets are refused.
Exit status: 0, 1 if a check fails, 2 if the inputs cannot be analysed (a
missing path, a single CSV that is empty or not in the schema, inputs of
more than one run set, no signature rows).

- **Keys** of the statistics: (flags, mode, scheme, variant, threads,
  logn, accounting); runs are combined, flag sets never.
- **Run sets**: every input must belong to one run set (`run_set` in
  `env.txt` or `commands.txt`; runs written before Step 8 have none and
  count as `all`); mixed inputs are refused, so the headline and
  cross-CCD runs never share a key or a table.  The run set is named in
  the header and the table titles.
- **Memory**: per run and key, the timing columns are kept as sorted
  compact arrays (4 bytes per value, 8 if a value needs it) and attempts
  and `rng_bytes` as sums (all the statistics use), read with
  `csv.reader`; see "Step 8" for the measurements.  The statistics are
  those of Step 7 (identical `summary.csv`, `runs.csv`, `ratios.csv` on
  the same inputs and seed).
- **Per key and timing column** (`core_cycles`, primary; `tsc_cycles`,
  `wall_ns`, `cpu_ns_all_threads`): per-run median; the estimate is the
  mean of the per-run medians; the run spread is (max - min) / estimate;
  pooled median, quartiles (IQR) and mean.  Attempts and `rng_bytes` per
  signature, signature bytes, warm-ups, failures.  Throughput mode:
  signatures per second per core (above), mean over runs.
- **Ratios** E8 / HAWK for matched pairs: same flags, mode, threads, logn,
  accounting, and the variant pairs `ref`/`ref`, `ref`/`ref-T`,
  `hot64`/`hot64`, `hot32`/`hot32` (marked `fair` when threshold bits and
  technique agree), plus `ref`/`ref-avx2` (marked `context`).
- **Bootstrap 95% CIs** (percentile, `--bootstrap` resamples, seeded per
  pair and metric from `--seed`, so reruns are identical): resample runs
  with replacement (the same run indices for both schemes, which ran in the
  same processes), then signatures within each chosen run.  The median of
  a resample is drawn exactly from its order statistic (the k-th of n
  uniform draws is Beta(k, n + 1 - k); even n also draws the next order
  statistic), so a replicate costs O(1) instead of a resort.  Throughput
  resamples each process's mean wall time from its normal approximation.
- **Joins** on (scheme, variant, logn), `ref-avx2` as `ref`:
  `docs/precision_summary.csv` (`threshold_bits`, `sig_sd_bound_log2`,
  `sig_rd513_m1_log2`, `cond6_ok`, `a_opt`, `loss_opt`) and
  `docs/validation_summary.csv` (`signatures`, `sign_failures`,
  `verify_failures`, `attempts_per_sig`).
- **Acceptance checks** (Step 9; any FAIL makes the exit status 1):
  - `verify`: zero signing or verification failures, warm-ups included;
  - `complete`: per CSV part (file, mode, process): every configuration of
    the grid (every variant at every thread count and degree seen;
    serial-only variants at `T = 1`), both accounting modes, equal
    signature counts, warm-up rows, `keygen` and `prepare` rows for every
    key signed with, flags equal to `env.txt`'s; and per run directory
    written by the driver: every step of the run set started and exited
    0 (`commands.txt`), its CSV and `env.txt` present, no CSV empty,
    header-only, unreadable or with malformed rows (a wrong column count
    or an unreadable number, as a killed process leaves);
  - `attempts`: per key, the excess attempts within 5 sigma (geometric
    variance) of Step 6's `attempts_per_sig` (HAWK n = 256 about 1.1796,
    the rest about 1.0000), plus 2 attempts of slack for rare retries;
  - `run_spread`: per key with at least 2 runs, per-run medians (primary
    metric; throughput) within `--run-tolerance` (default 3%); SKIP with
    one run;
  - `core_vs_tsc`: only if every run's `env.txt` has `boost: off`: per run,
    the serial latency keys' median core / TSC ratios within
    `--core-tsc-tolerance` (default 2%); SKIP otherwise;
  - `joins`: every key has its precision and validation rows and the same
    `threshold_bits`;
  - `preflight`: PASS if every run passed the preflight (`env.txt`); FAIL
    if a run on bare metal was forced past it; SKIP for smoke runs, WSL
    runs and runs without a preflight record.
- **Tables** (one set per flag set; `tables.md` for review, `tables.tex`
  with booktabs and graphicx: a table wider than the text is shrunk to the
  line width, one longer than 40 rows continues in further floats with the
  same number): 1 headline `ref` vs `ref` (one core, both accountings,
  cycles / TSC / us, attempts, signature and random bytes, precision
  columns, ratio and CI); 2 `hot64` and `hot32` pairs; 3 sampler only
  (cycles and bytes per 8 coordinates, `T = 1`); 4 parallel latency
  (HAWK `ref-T` / `hot64` / `hot32` vs E8, wall-clock TSC and total core
  cycles per `T`); 5 throughput per core; 6 stage breakdown (`ref`, one
  thread, all-in; includes the profile timers); 7 HAWK AVX2 context,
  labelled as not a fair pair.  A `cross-ccd` run set gives Table 4b
  alone, "Parallel latency across both CCDs", whose note gives its
  conditions and the known asymmetry (E8's sampler workers write 8n bytes
  per call, HAWK's 2n).
- **Other outputs**: `summary.csv` (every key with statistics and joins),
  `runs.csv` (per key and run), `ratios.csv` (every ratio and CI),
  `acceptance.txt`.

## Decisions (Step 7)

- **Threaded and serial configurations**: interleaved within groups of
  equal `T`; between groups the pool is joined and resized and every
  configuration re-warmed outside timed regions (the handoff's default).
  The alternative, a pool that parks idle workers, would change the shared
  pool's dispatch path for both schemes; it was not needed: no serial row
  runs beside spinning workers, and the remaining idle spinning inside a
  threaded group is identical for both schemes and reported per group.
- **CSV schema**: `mode`, `proc`, `flags`, `batch` appended (existing
  columns and order unchanged) and a `warmup` kind; `cmn_csv.h`, its test
  and `perf_comparison_final.md` updated.  `e8_sign_bench` switched to the
  new schema (`mode = latency`, its build's `flags`, `proc` and `batch`
  empty, no warm-up rows), so one reader handles both.
- **`bench_compare` vs `e8_sign_bench`**: the shared signing, keygen,
  verification, seed, message and row code moved to
  `compare/bench_sign_core.{c,h}`, used by both; `e8_sign_bench` and `make
  sign-bench` keep working (same seeds and schedule; its profile build now
  records every E8 block and writes `--stage-csv`); `bench_compare` is the
  harness of record.  `e8_correctness.c` was left as it is.
- **Sampler-only and stage tables**: separate binaries
  (`e8_sampler_bench`, the profile benches) built with the same flag sets
  and run by the driver into the same run directory (whose `env.txt`
  carries the `run_id`); the analysis reads their CSVs.
- **O3native**: `-O3 -march=native -fno-strict-aliasing` for every object
  of the flag set, both schemes alike (decided by the user after the root
  cause was found; see "Builds").  Rejected: patching `hawk_sign.c` (the
  timed HAWK would no longer be upstream), `-fno-tree-slp-vectorize` (hides
  the bug instead of tolerating it), dropping `-march=native` (changes
  what Step 9 measures).

## Findings for Steps 8 and 9

See plan.md, Step 7 status, for the smoke-run figures.

- **Run time** here (WSL, 32 CPUs, O2): latency 4.8 s per 100 signatures
  per configuration (93 configurations x 2 accounting modes; only about a
  quarter of it is timed signing, the rest is verification, messages and
  keys), throughput (P = 4) 1.1 s, AVX2 0.03 s, sampler matrix (2,000
  trials) 2.4 s, stages (1,000 trials; the HAWK profile bench generates a
  key per trial) 41 s.  At Step 9's volume (10 x 1,000) one run of one flag
  set is about 8 + 2 + 1 min, so 5 runs x 2 flag sets about 1.75 h.
- **T = 16 and one CCD**: the 9950X3D has 8 cores per CCD.  With SMT off
  (Step 9's setup) one CCD has 8 CPUs, so the harness (which requires
  `|--cpus| >= T`) runs `T` up to 8 there; `T = 16` needs both CCDs or SMT
  siblings, and either changes the conditions (cross-CCD L3, shared
  cores).  Suggested: `--threads 1,2,4,8` on the V-cache CCD, and `T = 16`
  as a separately labelled run if wanted.
- **Counters on WSL**: perf core cycles are available
  (`perf_event_paranoid` 2 suffices for user-space counting of the own
  thread); governor, boost and frequency are not readable, so
  `core_vs_tsc` is skipped, and core cycles run about 1.3x TSC (boost).
- **Pinning on WSL** works (`sched_setaffinity` succeeds, the test passes);
  thread siblings are (2k, 2k + 1), so use even CPUs for distinct cores.
- **O3native** needs `-fno-strict-aliasing` (above): without it upstream
  `hawk_sign.c` is miscompiled at `-O3 -march=native` (undefined
  behaviour in upstream); the flag costs at most 1.5% here.  In the smoke
  run O3native made E8 `hot64` / `hot32` slower than O2 (n = 256, all-in:
  253k vs 207k core cycles; not the flag, which leaves E8 `hot*`
  unchanged) while HAWK got faster, so the hot pairs' ratios moved from
  about 1.6-1.7 (O2) to 2.1-2.4; recheck on bare metal before reading
  anything into it.
- **Interleaving cost**: against `e8_sign_bench` (same signatures),
  `bench_compare` medians differ by 0.6% (median) and up to 2.4%, the
  larger values for small E8 `hot*` signatures, consistent with colder
  caches after another configuration's batch; this is the intended
  condition and applies to both schemes.
- **Pool idle CPU** inside threaded groups grows with `T` (spinning during
  verification and between batches): about 0.2 / 0.7 / 1.7 / 4.4 ms per
  signature for `T` = 2 / 4 / 8 / 16 in the smoke run; it is excluded from
  every row and reported per group.

## Step 8: the dress rehearsal (WSL)

Everything Step 9 runs was run here (WSL, 32 CPUs, SMT siblings (2k,
2k + 1), so the pinned lists use even CPUs), from a clean build, at small
and at full volume, with every failure path exercised.  No number here
goes into the paper; the timings only check the machinery and estimate
Step 9's run time.  The run book for Step 9 is
[`step9_runbook.md`](step9_runbook.md).

### What was run

- **Clean build**: `make -C e8 clean`, `make`: no warnings, all twelve
  `bin/*-O2` / `bin/*-O3native` binaries, `check-upstream` passes (the
  regenerated AVX2 sources are gitignored).
- **Smoke** (`make bench-compare-smoke`, 4.3 s): 0 failures, every check
  PASS or SKIP; rows counted by hand from `--list` (sign = configurations
  x K x M x 2 (x P), warm-up = configurations x W x 2 (x P), `keygen` /
  `prepare` = K per scheme and degree, 24 columns): every CSV matches.
- **Step-9-shaped**, both flag sets, 2 runs each, 2 x 20 signatures:
  headline (`--run-set headline --cpus 0,2,...,14`: T 1, 2, 4, 8, P = 8;
  55 s) and cross-CCD (`--run-set cross-ccd --cpus 0,2,...,30`: T 1, 2,
  4, 8, 16, latency only; 21 s).  Pinning checked by sampling every
  thread's `Cpus_allowed_list` and last CPU (`/proc/PID/task/*/stat`)
  every 50 ms: every latency thread, throughput signer, sampler and stage
  bench ran only on its own listed CPU (the throughput parent, which only
  generates the keys before forking and then waits, is not pinned).
  Hand counts match; `ratios.csv` CIs come from both runs (`runs` = 2);
  `run_spread` reports (FAIL at 40 signatures on WSL, see below); the two
  flag sets stay apart in every table; all 14 tables (7 per flag set) are
  non-empty, the cross-CCD analysis gives Table 4b alone.
- **Tables**: `tables.tex` compiles in a minimal `article` with `booktabs`
  and `graphicx` (`latexmk -pdf`, out of tree) with no error, overfull box
  or float warning.  Before the fix: 12 overfull boxes (Tables 1, 2, 4,
  5, 7 up to 258 pt too wide) and 4 floats too tall (Tables 4 and 6, 72
  and 66 rows).
- **Failure paths** (copies of real run directories in a scratch
  directory; a real driver run for the kill): `verify_ok` 0 -> `verify`
  FAIL, exit 1; `bench_compare` killed with `SIGKILL` mid-run -> the
  driver reports `latency failed (exit 137 (signal 9))` and the failed
  steps, exits 1, and `complete` FAILs (exit status, malformed last row,
  unequal counts); missing `latency.csv`, an empty or header-only CSV ->
  `complete` FAIL; a missing run directory, an empty CSV given directly,
  mixed run sets -> exit 2 with a message; two copies of the full run ->
  `run_spread` PASS, one with its timing columns x 1.1 -> FAIL (the 150
  latency keys); `boost: off` with real WSL data -> `core_vs_tsc` PASS,
  with core = 1.25 TSC everywhere -> PASS, with E8's core cycles x 1.1 ->
  FAIL; a forced or refused preflight (below).
- **Full volume** (10 x 1,000, O2, headline, pinned): see "Measurements".

### Fixes

1. **Sampler matrix oversubscription** (known issue 1): the driver runs
   `e8_sampler_bench --variant-matrix --threads 1` pinned to the list's
   first CPU (Table 3 uses `T = 1` only).  Full trials (2,000) under an
   8-CPU `taskset`: 5.7 s before (T = 16 rows on 8 CPUs), 1.5 s after.
2. **`--cpus` without a matching `--threads`** (known issue 2): run sets
   (`--run-set`, "Running") set the thread list and `P`; the driver
   checks the list (comma-separated numbers, no repeats, at least max T
   and P CPUs) before any step and exits 2 with a message.
3. **Analysis memory** (known issue 3): compact sorted arrays for the
   timing columns, sums for attempts and `rng_bytes`, `csv.reader`; the
   default output directory no longer loads the first run twice.  Same
   statistics (below).
4. **Preflight** (known issue 4): `compare/preflight.sh` ("Running").
   Tested on WSL: refusing (exit 3, no run directory created: SMT on,
   dirty tree, boost / governor / frequency unavailable), `--force` (runs,
   records `preflight: failed (forced: --force)` and every condition in
   `env.txt` and `commands.txt`), smoke (reported, not enforced), and
   every condition's `ok` and `FAIL` branch through `--capture` with
   synthetic captures (boost on, `powersave`, SMT on, siblings pinned
   together, paranoid 3, a 16% frequency spread, a dirty tree).
5. **`attempts` check counted copies as independent** (found in Step 8):
   every run and every throughput process signs the same messages with the
   same seeds, so their attempts are identical copies (checked: all 8
   processes of the full run, every key).  The check summed them as
   independent, which shrinks its 5 sigma tolerance by sqrt(R P): at Step
   9's 5 runs x 8 processes to about 0.8 sigma, so it would have failed
   HAWK n = 256 keys by chance (the old code fails 5 of 198 keys on 10
   copies of one run).  It now requires the copies to agree (n, attempts
   and `rng_bytes` sums per run and process; a disagreement means
   non-deterministic signing and FAILs) and tests one copy against Step 6.
6. **Step failures reported into the step's own log** (found in Step 8):
   `run_step`'s "failed" message was inside the caller's `2> STEP.txt`
   redirection, so it went to `latency.txt` instead of the console.  The
   driver now writes it to its own stderr and lists the failed steps at the
   end.
7. **Silent or crashing inputs** (found in Step 8): a missing run directory
   crashed the analysis (`FileNotFoundError`); an empty CSV, a missing step
   CSV and a header-only CSV were ignored (their keys simply absent, so
   `complete` could PASS); a truncated last row could crash `int()`.  Now:
   a clear error and exit 2, or a `complete` FAIL naming the problem;
   `commands.txt` step exit statuses are checked too.
8. **LaTeX**: wide tables shrink to the line width (`\resizebox`, only
   when wider), tables over 40 rows continue in further floats with the
   same number ("continued").  Table notes now define all-in / prepared,
   sig B (HAWK compressed, E8 uncompressed), attempts and rng B, and say
   that the throughput processes sign the same signatures.

Left as they are: the throughput parent is not pinned (it only generates
keys, untimed, then waits); LaTeX numbers the second flag set's tables 8-14
(the Markdown and the labels say which is which; Step 10 chooses the
paper's layout); Tables 1 and 2 are shrunk to about 60% in an `article`
column (15 columns), so Step 10 should split or rotate them.

### Decisions (Step 8)

- **Run sets**: a driver option, `--run-set all|headline|cross-ccd`, with
  the thread list, `P`, steps and `--cpus` requirement of the decided
  Step 9 runs; `all` keeps the former behaviour for the smoke run.  The
  analysis keeps run sets apart by refusing mixed inputs (exit 2) rather
  than adding a key column: the run sets never share a table, each has its
  own analysis directory (`--name`), and the statistics of a single run
  set are unchanged.
- **Analysis storage**: compact in-memory columns (the default), not a
  streaming pass with per-key sorted files: at Step 9's volume the
  analysis then needs under 1 GB and 2 minutes, so the extra complexity
  buys nothing.
- **CSV compression**: plain CSV (the default).  A headline run writes 672
  MB of CSV (latency 201 MB, throughput 463 MB, AVX2 8 MB); gzip (level 6)
  makes it 78 MB (8.6x) in 5.4 s per run, and the analysis reads
  `NAME.csv.gz` like `NAME.csv` in 10% more time (13.6 s against 12.4 s,
  identical outputs).  Step 9 needs about 10 GB of disk for both run sets,
  which is no constraint; compressing inside the run would add a step
  that can fail; the copy back is compressed anyway (run book).
- **Failure messages**: the driver's exit status distinguishes a failed
  step or check (1), a usage or configuration error (2) and a refused
  preflight (3).
- **CPU lists**: `compare/ccd_cpus.sh` derives both Step 9 lists from the
  L3 topology (one CPU per core, the larger-L3 CCD first), so the run book
  needs no hand-typed list (tested on synthetic sysfs trees with SMT on
  and off and the V-cache on either CCD; on WSL it refuses: one L3
  domain).

### Measurements (WSL, not for the paper)

| what | value |
| --- | --- |
| clean `make -j16` | 4.2 s |
| smoke (both flag sets) | 4.3 s |
| Step-9-shaped headline, 2 x 20, 2 runs x 2 flag sets | 55 s |
| Step-9-shaped cross-CCD, 2 x 20, 2 runs x 2 flag sets | 21 s |
| full volume headline run (10 x 1,000, O2, T 1-8, P = 8) | 8 min 49 s: latency 373 s, throughput 111 s, AVX2 2 s, sampler 2 s, stages 11 + 29 s |
| full volume cross-CCD run (O2, T 1-16, 16 CPUs, latency only) | 7 min 53 s (latency 472 s) |
| CSV per headline run | latency 201 MB (1.50 M rows), throughput 463 MB (3.36 M rows), AVX2 8 MB |
| CSV per cross-CCD run | latency 250 MB (1.86 M rows; gzip 31 MB) |
| analysis, 1 full cross-CCD run | 6.1 s, 53 MB |
| analysis, 1 full run: before / after | 25 s, 1.04 GB / 12.4 s, 109 MB |
| analysis, 10 copies (Step 9's 5 runs x 2 flag sets): before / after | 4 min 26 s, 10.2 GB / 1 min 59 s, 865 MB |

Statistics unchanged: the new analysis writes byte-identical
`summary.csv`, `runs.csv` and `ratios.csv` (same seed) on the smoke
output, the full run and 10 copies of it; `tables.md` and `tables.tex`
differ only in the header (run set), the table titles and notes and the
new `preflight` line.

### Findings for Step 9

- **Run time**: per run and flag set here, headline 8.8 min (latency 6.2,
  throughput 1.9, the rest 0.7) and cross-CCD 7.9 min; most of it is
  verification, messages and keys, not timed signing.  Step 9 (5 runs x 2
  flag sets per run set) at this speed: 88 + 79 min, about 2.8 h.  On bare
  metal with boost off the cores run at the base clock instead of WSL's
  boosted one (core / TSC about 1.3 here), so expect about 1.3x: headline
  about 1.9 h, cross-CCD about 1.7 h, 3.6 h in all, plus the smoke run and
  the analyses (minutes); plan for 4 h.  (Step 7's estimate, 1.75 h for
  one run set at T up to 16 and P = 4 at WSL speed, becomes 1.5 h for the
  headline at T up to 8.)  Disk: 6.7 GB of CSV for the headline run set,
  2.5 GB for the cross-CCD one (0.8 and 0.3 GB gzipped).
- **Determinism**: every run and every throughput process signs the same
  signatures (fixed seeds); the analysis now relies on it (`attempts`).
  Repeated runs therefore vary only in timing, which is what `run_spread`
  and the bootstrap over runs measure.
- **Run spread on WSL**: at 2 x 20 signatures, 124 of 396 headline keys
  and 133 of 372 cross-CCD keys exceeded 3%, the worst threaded ones
  (HAWK `ref-T` T = 4, E8 `ref` T = 4: 30-34%); at full volume one run
  only was taken.  Bare metal with boost off, pinned cores and no desktop
  should be far quieter; if threaded keys still fail, look at the pool's
  spinning workers and interrupts on the listed CPUs before anything else.
- **`core_vs_tsc`** passes on real WSL data with `boost: off` forced into
  `env.txt` (the serial ratios agree within 2% even with boost), so on
  bare metal it is a meaningful check only together with the preflight.
- **Do not touch the repository during a run**: the driver is a shell
  script that `dash` reads as it goes, so editing or checking out
  `run_compare.sh` mid-run can corrupt the rest of the run (noticed here
  while editing during a run, and reverted before the script read the
  changed part).  The preflight checks a clean
  tree only at the start of each run.
- **`make clean` deletes `e8/results/`**, the runs included.
