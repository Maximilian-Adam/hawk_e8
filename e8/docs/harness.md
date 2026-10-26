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
| driver | `compare/run_compare.sh` | one run directory per run and flag set, then the analysis |
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
`COMPARE_ARGS` (passed to the driver) select more.  Step 9, for example, on
one CCD with SMT off:

```sh
make bench-compare COMPARE_FLAGSETS="O2 O3native" COMPARE_RUNS=5 \
  COMPARE_ARGS="--cpus 0,1,2,3,4,5,6,7 --procs 8 --threads 1,2,4,8 --name step9"
```

Driver options: `--smoke`, `--flagsets`, `--runs R` (runs and flag sets
alternate: r1/O2, r1/O3native, r2/O2, ...), `--keys`, `--sigs-per-key`,
`--procs`, `--cpus`, `--threads`, `--logn`, `--warmup`,
`--sampler-trials`, `--stage-trials`, `--bootstrap`, `--name` (the analysis
directory; default the first run's `run_id`), `--no-analysis`.  Defaults:
smoke 2 keys x 4 signatures, `P = 4`, 50 sampler trials, 20 stage trials,
200 bootstrap resamples; full 10 x 1,000, `P = 4`, 2,000, 1,000, 2,000.

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
- the sampler and stage benches run under `taskset -c LIST` (stages:
  `LIST[0]`).

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
    env.txt                 env_capture.sh
    commands.txt            every command, start time, duration, exit status
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
`unavailable`; it never fails.  On WSL: governor, scaling driver, frequency
and boost are unavailable, microcode reads `0xffffffff`, SMT is on.

## Analysis

`python3 -I compare/analysis/analyze.py [--out-dir DIR] [--bootstrap B]
[--seed S] [--run-tolerance F] [--core-tsc-tolerance F] [--precision CSV]
[--validation CSV] [--no-fail] PATH...` (standard library, no bytecode
written).  `PATH`: run directories, or single CSVs in the schema (an
`e8_sign_bench` output works; missing Step 7 columns default to `mode =
latency`).  `--self-test` checks the bootstrap against brute-force
resampling, determinism, and that the checks pass on good synthetic runs
and fail on a verification failure, a missing row and a 20% slower run.

- **Keys** of the statistics: (flags, mode, scheme, variant, threads,
  logn, accounting); runs are combined, flag sets never.
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
    key signed with, flags equal to `env.txt`'s;
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
    `threshold_bits`.
- **Tables** (one set per flag set; `tables.md` for review, `tables.tex`
  with booktabs): 1 headline `ref` vs `ref` (one core, both accountings,
  cycles / TSC / us, attempts, signature and random bytes, precision
  columns, ratio and CI); 2 `hot64` and `hot32` pairs; 3 sampler only
  (cycles and bytes per 8 coordinates, `T = 1`); 4 parallel latency
  (HAWK `ref-T` / `hot64` / `hot32` vs E8, wall-clock TSC and total core
  cycles per `T`); 5 throughput per core; 6 stage breakdown (`ref`, one
  thread, all-in; includes the profile timers); 7 HAWK AVX2 context,
  labelled as not a fair pair.
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
