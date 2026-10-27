# Comparison harness

One harness times every sampler variant of both schemes under identical
conditions, records the environment, and turns the per-signature CSVs into
the statistics and comparison tables.  The accounting (what each timed call
contains) and the per-signature CSV schema are defined in
[`perf_comparison_final.md`](perf_comparison_final.md).  This document
covers the harness, its builds, the run layout and the analysis.

| piece | file | role |
| --- | --- | --- |
| harness | `compare/bench_compare.c` | every (scheme, variant, threads, logn) configuration, interleaved; latency and throughput modes; pinning; keyverify mode (key generation and verification); `--sizes` |
| signing core | `compare/bench_sign_core.{c,h}` | keygen, preparation, the timed `sign_finish` of every variant, verification, seeds, messages, row fields; the paired keygen stream, `keygen_core`, public-key decoding and timed verification of keyverify mode; shared with `e8_sign_bench` |
| pool affinity | `common/cmn_pool.{c,h}` | `cmn_pool_set_affinity()`, `cmn_pin_current_thread()` (both schemes alike) |
| environment | `compare/env_capture.sh` | `env.txt` beside every run's CSVs |
| preflight | `compare/preflight.sh` | machine-state checks before every run |
| CPU lists | `compare/ccd_cpus.sh` | the run sets' pinned CPU lists from the L3 topology |
| driver | `compare/run_compare.sh` | run sets; one run directory per run and flag set, then the analysis |
| analysis | `compare/analysis/analyze.py`, `compare_data.py`, `compare_tables.py` | statistics, ratios with bootstrap CIs, joins, acceptance checks, LaTeX and Markdown tables |
| sampler only | `compare/e8_sampler_bench.c --variant-matrix` | table 3 |
| stages | `e8_sign_profile_bench`, `hawk_sign_profile_bench` (`--stage-csv`) | table 6 |

## Running

```sh
make bench-compare-smoke   # every configuration of both flag sets at
                           # small counts, the analysis self-test, the analysis
make bench-compare         # one run at full volume (10 keys x 1,000
                           # signatures per configuration), flag set O2
```

Both forward to `e8/` and call `compare/run_compare.sh`; make variables
`COMPARE_FLAGSETS` (default `O2`), `COMPARE_RUNS` (default 1) and
`COMPARE_ARGS` (passed to the driver) select more.  The two bare-metal run sets
(the exact commands, machine setup and checks: the run book,
[`measurement_runbook.md`](measurement_runbook.md)):

```sh
make bench-compare COMPARE_FLAGSETS="O2 O3native" COMPARE_RUNS=5 \
  COMPARE_ARGS="--run-set headline --cpus $(sh e8/compare/ccd_cpus.sh headline) --name headline"
make bench-compare COMPARE_FLAGSETS="O2 O3native" COMPARE_RUNS=5 \
  COMPARE_ARGS="--run-set cross-ccd --cpus $(sh e8/compare/ccd_cpus.sh cross-ccd) --name cross-ccd"
```

Driver options: `--smoke`, `--run-set all|headline|cross-ccd` (below),
`--flagsets`, `--runs R` (runs and flag sets alternate: r1/O2,
r1/O3native, r2/O2, ...), `--keys`, `--sigs-per-key`, `--procs`, `--cpus`,
`--threads`, `--logn`, `--warmup`, `--sampler-trials`, `--stage-trials`,
`--keygen-keys N` (timed key generations per scheme and degree in the
`keyverify` steps), `--bootstrap`, `--name` (the analysis directory;
default the first run's
`run_id`, prefixed `RUNSET-` for the `headline` and `cross-ccd` run sets), `--no-analysis`,
`--force` (run despite a failed preflight; recorded), `--freq-tolerance
PCT` (the preflight's frequency check, default 1).  Defaults: smoke 2 keys
x 4 signatures, 20 keygen keys, `P = 4`, 50 sampler trials, 20 stage
trials, 200 bootstrap resamples; full 10 x 1,000, 1,000 keygen keys,
`P = 4`, 2,000, 1,000, 2,000.  The keyverify steps use `--keys` and
`--sigs-per-key` for their verification inputs (10 keys x 1,000
signatures at full volume).

Run sets (`--run-set`; written as `run_set` to `env.txt` and
`commands.txt`):

| run set | steps | `T` (default) | `P` (default) | `--cpus` |
| --- | --- | --- | --- | --- |
| `all` (default) | all eight | 1, 2, 4, 8, 16 | 4 | optional |
| `headline` | all eight | 1, 2, 4, 8 | `\|--cpus\|` | required (not for `--smoke`) |
| `cross-ccd` | latency only | 1, 2, 4, 8, 16 | none | required (not for `--smoke`) |

The eight steps, in order: `keyverify`, `keyverify_avx2` (before
`latency`), `latency`, `throughput`, `latency_avx2`, `sampler`,
`stages_hawk`, `stages_e8`; every run also writes `sizes.csv`.
`all` is the smoke run's and any ad hoc run's; `headline` and `cross-ccd`
are the bare-metal measurement's.  `--threads` and `--procs`
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

`bench_compare` itself: `--mode latency|throughput|keyverify`,
`--keygen-keys N` (keyverify), `--sizes` (the sizes CSV), `--keys K`,
`--sigs-per-key M`, `--warmup W`, `--logn LIST`, `--threads LIST` (default
1,2,4,8,16), `--variant NAME`..., `--scheme hawk|e8`..., `--procs P`,
`--cpus LIST`, `--run-id ID`, `--out FILE`, `--list` (the configurations in
schedule order; keyverify: the expected row counts), `--build-info` (flag
set, flags, compiler, core-cycle counter).  Exit status 1 on any signing or
verification failure (keyverify: also a failed key generation, key check
or decoding, or an accepted tampered signature).

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
- **keyverify**: key generation and verification, single thread, pinned
  to `LIST[0]`, no pool; schedule in "Key generation and verification"
  below.
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
object of the flag set, both schemes alike.  Recheck on the measurement
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
    sizes.csv/.txt          bench_compare-FS --sizes
    keyverify.csv/.txt      bench_compare-FS --mode keyverify
    keyverify_list.csv      the same with --list: expected row counts
    keyverify_avx2.csv/.txt, keyverify_avx2_list.csv
                            bench_compare_avx2-FS --mode keyverify (context)
    latency.csv/.txt        bench_compare-FS --mode latency (CSV / summary)
    throughput.csv/.txt     bench_compare-FS --mode throughput
    latency_avx2.csv/.txt   bench_compare_avx2-FS (context)
    sampler.csv/.txt        e8_sampler_bench-FS --variant-matrix
    stages_hawk.csv/.txt    hawk_sign_profile_bench-FS --stage-csv
    stages_e8.csv/.txt      e8_sign_profile_bench-FS --variant ref --threads 1 --stage-csv
  analysis/NAME/            analyze.py over one or more run directories
    summary.csv runs.csv ratios.csv acceptance.txt tables.md tables.tex
    keyverify_summary.csv keyverify_ratios.csv
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
and the `preflight_NAME` lines.  On WSL: governor, scaling driver, frequency
and boost are unavailable, microcode reads `0xffffffff`, SMT is on.

## Analysis

`python3 -I compare/analysis/analyze.py [--out-dir DIR] [--bootstrap B]
[--seed S] [--run-tolerance F] [--core-tsc-tolerance F] [--precision CSV]
[--validation CSV] [--no-fail] PATH...` (standard library, no bytecode
written).  `PATH`: run directories, or single CSVs in the schema (an
`e8_sign_bench` output works; missing harness columns default to `mode =
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
  `env.txt` or `commands.txt`; older runs have none and
  count as `all`); mixed inputs are refused, so the headline and
  cross-CCD runs never share a key or a table.  The run set is named in
  the header and the table titles.
- **Memory**: per run and key, the timing columns are kept as sorted
  compact arrays (4 bytes per value, 8 if a value needs it) and attempts
  and `rng_bytes` as sums (all the statistics use), read with
  `csv.reader` (under 1 GB and about 2 minutes for 5 full runs x 2 flag
  sets).
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
- **Acceptance checks** (any FAIL makes the exit status 1):
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
    variance) of the validation's `attempts_per_sig` (HAWK n = 256 about 1.1796,
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
  `acceptance.txt`; `keyverify_summary.csv` and `keyverify_ratios.csv`
  (below).
- **Key generation and verification**: rows of mode
  `keyverify` are kept apart from the signing keys. Their statistics,
  checks (`keyverify_complete`, `verify_failures`, `verify_reject`,
  `run_spread` on the verification medians) and Tables 8-10 are described
  in "Key generation and verification" below.

## Key generation and verification

`bench_compare --mode keyverify` times key generation and verification for
Tables 8-10 in the same session, build, machine state and pinning as
signing. The accounting is in `perf_comparison_final.md` ("Key generation
and verification"); the E8 verifier and the key generation limits in
`HAWK_E8_VARIANT.md`; why E8 verifies with 3 primes and HAWK with 2 in
`verifier_range.md`.

### Mode

`bench_compare --mode keyverify [--keygen-keys N] [--keys K]
[--sigs-per-key M] [--warmup W] [--logn LIST] [--cpus LIST]` runs one
thread pinned to `LIST[0]`, with no pool (the E8 sampler at `T = 1`). The
accounting is in `perf_comparison_final.md`, "Key generation and
verification". Per degree, in `--logn` order, the key generation phase
runs first for every degree and then the verification phase:

- **Key generation phase**: W warm-up calls per unit (`warmup_keygen`
  rows, stream `BSC_STREAM_KEYGEN_WARMUP` = 25). Then, for key index
  `i = 0 .. N-1`, the units HAWK `keygen`, E8 `keygen` and `keygen_core`
  (`Hawk_keygen()` alone, scheme `hawk_core`), all from the paired stream
  `BSC_STREAM_KEYGEN_PAIRED` = 24 with index i, in an order rotated by i.
  `attempts` = RNG calls (one per (f, g) draw), `rng_bytes` = bytes drawn.
  Every HAWK and E8 key then signs one message with the ref signer and
  must verify (untimed). The signing keys of the other modes (streams
  `1 + scheme`) are unchanged.
- **Verification phase**: keys `0 .. K-1` of the paired stream (untimed);
  `pub_decode` rows, one per key and scheme, in rotated order; `K x M`
  signatures per scheme by its `ref` signer, `all_in`, `T = 1`, with the
  harness's messages `i = k M + m` and signing stream `16 + all_in`
  (untimed). Then:
  - **Tampered** (untimed, `verify_reject` rows): the first signature of
    each key with one bit flipped in the salt (byte 0, bit 0), in `s1`
    (HAWK: the first byte of the encoded `s1`, bit 0; E8: bit 14 of
    `s1[0]`) and, for E8, in `s0` (bit 14 of `s0[0]`), checked by every
    verifier unit. `verify_ok` = 1 if accepted.
  - **Warm-up**: W verifications per unit (`warmup_verify` rows).
  - **Timed schedule**: per signature `j = k M + m`, the units {HAWK
    `upstream`, E8 `fast`, E8 `ref`} x {`all_in`, `prepared`}, in an
    order rotated by j, one `verify` row each (`batch` = j's position).

The AVX2 context binary (`bench_compare_avx2-FS`) runs the same mode for
HAWK alone (variant `ref-avx2`; its `keygen_core` is the AVX2 build's
NTRU core). `--list` prints the expected row count per (degree, kind,
scheme, variant, verifier, accounting); the driver writes it as
`keyverify_list.csv` beside `keyverify.csv`. `--sizes` prints the sizes
CSV (`sizes.csv` in every run directory).

Driver: steps `keyverify` and `keyverify_avx2` in the `all` and
`headline` run sets (not `cross-ccd`), before `latency`; `--keygen-keys`
(smoke 20, full 1,000); the verification inputs use `--keys` and
`--sigs-per-key` (smoke 2 x 4, full 10 x 1,000). The preflight and the
`--cpus` checks are unchanged.

### Analysis

- **Verification**: per (flags, scheme, variant, verifier, logn,
  accounting), the signing estimator: per-run median, estimate = mean of
  the per-run medians, and the E8 / HAWK ratio with the hierarchical
  bootstrap CI (runs, then signatures). The pairs are E8 `fast` / HAWK
  `upstream` (fair), E8 `ref` / HAWK (the submission's verifier) and E8
  `fast` / HAWK AVX2 (context).
- **Key generation**: restarts make it heavy-tailed, and its cost to a
  user is its expectation. So the estimate is the mean of the per-run means
  over key indices, with the mean of the per-run medians beside it. The E8
  / HAWK ratio is a ratio of means, with a paired bootstrap CI (runs, then
  key indices within each run, the same indices for both schemes, which
  drew from the same seeds). Each scheme's tail is its mean minus the
  core's on the same indices. `diverged` is the share of indices where the
  schemes made different numbers of RNG calls (one restarted after the
  core). Key generation run spread is reported (`keyverify_summary.csv`),
  not checked.
- **Checks**: `keyverify_complete` (every keyverify CSV's row counts equal
  its `--list` file), `verify_failures` (every timed verification
  accepted; every keygen, `keygen_core`, `pub_decode` and warm-up row ok),
  `verify_reject` (every tampered signature rejected), and `run_spread`
  extended to the verification medians at the signing tolerance (3%). All
  three new checks SKIP without keyverify rows (the cross-CCD run set,
  older runs).
- **Tables**: 8 key generation (mean and median per key, ratio with
  paired CI, tail, diverged, draws, key bytes; HAWK AVX2 context rows),
  9 verification (median cycles for both accountings and both E8
  verifiers, ratios with CIs, prime counts, `pub_decode`, signature and
  public key bytes; HAWK AVX2 context rows), 10 sizes (every key,
  signature and `tmp` object, once per analysis).
- **Outputs**: `keyverify_summary.csv` (per unit: runs, count, estimate,
  median, quartiles, run spread, RNG calls per key, failures, warm-ups)
  and `keyverify_ratios.csv`.
- **Self-test**: synthetic keyverify runs; the checks pass on good runs
  and fail on an accepted tampered signature, a verification failure, a
  truncated `keyverify.csv` and a 20% slower run; the ratios lie inside
  their CIs and the bootstrap is deterministic.

### Reading Tables 8-10

- **Verification pairs**: E8 `fast` / HAWK `upstream` is the fair pair
  (HAWK's verifier techniques on both sides; each scheme with the minimum
  number of primes its own range argument proves sound, HAWK 2 and E8 3).
  E8 `ref` is the verifier behind the submission's 6.0-6.5x figure, a
  context row; so is HAWK AVX2.
- **all-in minus prepared** is the public-key decoding and hashing inside
  the timed call; `pub dec` times the decoding alone.
- **Key generation restarts**: HAWK's `hawk_keygen()` restarts the whole
  key generation, shared NTRU core included, when its compressed
  public-key encoding fails. E8's uncompressed public key always encodes,
  and its key generation limits reject no key in practice. The
  `diverged` column is the share of key indices where this made the two
  schemes' draws differ (about 0.5% / 8.5% / 1.6% of keys at n = 256 /
  512 / 1024 in trial runs), and each such key costs HAWK roughly twice
  as much. The ratio of means therefore includes a format effect, which
  can put it below 1. The `tail` column (scheme mean minus core mean)
  separates each scheme's own work after the core: E8's public form,
  limits and 3-prime invertibility check against HAWK's encoding.
