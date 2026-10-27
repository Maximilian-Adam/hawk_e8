# Run book: the measurements on bare metal

How to run the timing measurements on the AMD Ryzen 9 9950X3D (Arch Linux): machine
setup, the CPU lists, the preflight, the two run sets, the acceptance
checks, what to copy back and what to do when a check fails.  The harness
itself is described in [`harness.md`](harness.md); the accounting and the
CSV schema in [`perf_comparison_final.md`](perf_comparison_final.md).
Everything below was rehearsed at small and full volume; the key
generation and verification steps also on this machine, where
`ccd_cpus.sh` found the V-cache CCD at cores 0-7.

## 1. Machine setup

```sh
sudo cpupower frequency-set -g performance                   # every CPU
echo 0 | sudo tee /sys/devices/system/cpu/cpufreq/boost        # fixed clock
echo off | sudo tee /sys/devices/system/cpu/smt/control        # SMT off
echo 1 | sudo tee /proc/sys/kernel/perf_event_paranoid         # user-space core cycles
```

- If `cpufreq/boost` does not exist (amd-pstate in active mode), use
  `echo 0 | sudo tee /sys/devices/system/cpu/amd_pstate/cpb_boost` or set
  the governor to `performance` and boost off through `cpupower set
  --turbo-boost 0`; `env_capture.sh` reads all three places.
- Minimal background load: no desktop session (`systemctl isolate
  multi-user.target` if practical), no other jobs, network idle.
- Optional: `isolcpus=` / `nohz_full=` for the benchmark cores (kernel
  command line, reboot); `env.txt` records both.
- Repository: clone or copy this repository at the commit to measure,
  `make` from the root (no warnings), `make test-e8`, `make check-upstream`.
  The tree must be clean (`git status --untracked-files=no` empty): the
  preflight refuses a dirty tree, because `env.txt` records only the
  commit.
- Compiler: record `c99 --version`.  Recheck the `O3native` flag set on
  this compiler (`harness.md`, "Builds": upstream `hawk_sign.c` needs
  `-fno-strict-aliasing` at `-O3 -march=native`): `make
  bench-compare-smoke` must report 0 verification failures for both flag
  sets.
- Do not run `make clean` or `make -C e8 clean` once runs exist: it deletes
  `e8/results/`.

## 2. The CPU lists

With SMT off, the 9950X3D has 16 online CPUs, 8 per CCD; one CCD has the
3D V-cache (96 MiB L3 against 32 MiB).  Find it:

```sh
lscpu -e                                         # CPU, CORE, L3 columns
grep . /sys/devices/system/cpu/cpu*/cache/index3/{id,size}
sh e8/compare/ccd_cpus.sh show                   # both CCDs, the lists
```

`compare/ccd_cpus.sh` groups the online CPUs by L3 domain (one CPU per
core), takes the domain with the larger L3 as the V-cache CCD and prints:

- `headline`: the V-cache CCD's 8 cores, for example `0,1,2,3,4,5,6,7`;
- `cross-ccd`: those 8 first, then the other CCD's 8, for example
  `0,1,...,7,8,...,15`.  Worker `w` then sits on the same CPU in both run
  sets, and the cross-CCD rows with `T <= 8` run on the V-cache CCD.

It exits 1 unless it finds exactly two L3 domains of different sizes with
8 cores each; check `lscpu -e` by hand then.

## 3. Preflight

`compare/run_compare.sh` runs `compare/preflight.sh` before every run and
refuses a non-smoke run (exit 3, before its run directory exists) unless
every condition is ok:

| condition | ok when |
| --- | --- |
| `boost` | boost is off |
| `governor` | `performance` on every pinned CPU |
| `smt` | `smt/control` is `off` (or `forceoff`, `notsupported`) and `smt/active` is 0 |
| `cores` | no two pinned CPUs are SMT siblings |
| `perf` | `perf_event_paranoid <= 2` and the binary's core-cycle counter works |
| `freq` | the pinned CPUs' `scaling_cur_freq`, read while each runs a busy loop, within 1% (`--freq-tolerance`) |
| `git` | the tree is clean (tracked files) |

Run it by hand first:

```sh
cd e8
sh compare/preflight.sh --cpus "$(sh compare/ccd_cpus.sh cross-ccd)" \
  --binary bin/bench_compare-O2 --repo ..
```

`--force` (driver) overrides; the override and every failed condition are
recorded in the run's `commands.txt` and `env.txt`, and the analysis's
`preflight` check then FAILs.  Do not force a measurement run; fix the machine
state instead.  If `freq` fails although boost is off and the governor is
`performance`, compare `cpupower monitor` on the listed CPUs: an idle-state
artefact passes on a second try, a real clock difference between the CCDs
does not.

## 4. The commands

Smoke first (every configuration of both flag sets at small counts,
unpinned; not a measurement run set; the preflight is reported, not enforced):

```sh
make bench-compare-smoke
```

Then the two run sets, from the repository root.  Headline (tables
1-10): key generation and verification (`keyverify`, `keyverify_avx2`:
one thread pinned to the first CPU of the list, 1,000 timed key
generations per scheme and degree, 10 x 1,000 verified signatures), then T
in 1, 2, 4, 8 and throughput with P = 8 on the V-cache CCD; both flag
sets; 5 runs each (runs and flag sets alternate: r1/O2, r1/O3native,
r2/O2, ...):

```sh
make bench-compare COMPARE_FLAGSETS="O2 O3native" COMPARE_RUNS=5 \
  COMPARE_ARGS="--run-set headline --cpus $(sh e8/compare/ccd_cpus.sh headline) --name headline"
```

Cross-CCD (secondary, Table 4b): latency only, T in 1, 2, 4, 8, 16 on both
CCDs, the V-cache CCD first:

```sh
make bench-compare COMPARE_FLAGSETS="O2 O3native" COMPARE_RUNS=5 \
  COMPARE_ARGS="--run-set cross-ccd --cpus $(sh e8/compare/ccd_cpus.sh cross-ccd) --name cross-ccd"
```

The run sets set the thread list (`1,2,4,8` / `1,2,4,8,16`) and `--procs`
(`|--cpus|`, headline only); the driver checks before any step that the
CPU list holds the largest T and P on distinct CPUs.  Each run writes
`e8/results/compare/FS/RUN_ID/` (`env.txt` and `commands.txt` carry
`run_set`); the analysis goes to `e8/results/compare/analysis/NAME/`.  The
analysis refuses inputs that mix run sets, so each run set has its own
`--name`.  To re-run the analysis alone (for example after copying runs
elsewhere):

```sh
cd e8
python3 -I compare/analysis/analyze.py --out-dir results/compare/analysis/headline \
  results/compare/O2/RUN_ID ... results/compare/O3native/RUN_ID ...
```

## 5. Expected run time

Measured on WSL (one full-volume run of each run set, O2, pinned): per run
and flag set, headline 8.8 min (latency 6.2, throughput 1.9, AVX2, sampler
and stages 0.7) and cross-CCD 7.9 min.  WSL's cores ran boosted (core /
TSC about 1.3), the measurement runs at the base clock, so expect about
1.3x.  The key generation and verification steps took 67 + 16 s per run
and flag set on this machine with boost on, so about 1.8 min with boost
off:

| run set | runs x flag sets | per run (expected) | total (expected) |
| --- | --- | --- | --- |
| smoke | 1 x 2 | seconds | under a minute |
| headline | 5 x 2 | about 13.3 min | about 2.2 h |
| cross-CCD | 5 x 2 | about 10.3 min | about 1.7 h |

Plan for 4.5 hours.  The analysis of each run set takes about 2 minutes and
under 1 GB of memory.  Start the runs inside `tmux` or `screen` (or with
`nohup`), so that a dropped session does not stop them, and do not touch
the repository while they run: the driver is a shell script read as it
runs, and the preflight checks a clean tree only at the start of each
run.  The first run of each flag set finishes after about 12 minutes:
check its `commands.txt` (every step `exit 0`) before leaving it.
Its first steps, `keyverify` and `keyverify_avx2`, finish after about
2 minutes.

## 6. Acceptance

The driver exits 0 only if every step succeeded and the analysis's checks
passed; `acceptance.txt` (and the top of `tables.md`) lists them:

| check | must be |
| --- | --- |
| `verify` | PASS: 0 signing or verification failures, warm-ups included |
| `complete` | PASS: every step ran and exited 0, every CSV present, complete and well-formed |
| `attempts` | PASS: attempts per signature as in the validation (HAWK n = 256 about 1.1796, the rest about 1.0000) |
| `run_spread` | PASS: per-run medians within 3% (`--run-tolerance`) for every key |
| `core_vs_tsc` | PASS: with boost off, the serial core / TSC ratio constant within 2% in each run |
| `joins` | PASS: precision and validation rows for every key |
| `preflight` | PASS: every run passed the preflight |
| `keyverify_complete` | PASS: every `keyverify*.csv` has the row counts of its `--list` file |
| `verify_failures` | PASS: every timed verification accepted; every key generation, `pub_decode` and warm-up row ok |
| `verify_reject` | PASS: every tampered signature rejected by every verifier in both accounting modes |

`run_spread` covers the verification medians as well (same 3%); key
generation's per-run means are reported in `keyverify_summary.csv`, not
checked (they are heavy-tailed).

Then read `tables.md` as a reviewer would: the ratio CIs (`ratios.csv`,
`runs` = 5), the two flag sets apart, the HAWK AVX2 context row labelled,
and the cross-CCD Table 4b against the headline Table 4 at `T <= 8`.
For Tables 8-10 (`keyverify_ratios.csv`, `keyverify_summary.csv`), check
the `diverged` column and the O3native E8 `fast` all-in rows (all-in
minus prepared was twice O2's at n = 1024 in a trial run); `harness.md`,
"Key generation and verification", explains both.

## 7. What to copy back

Per run set, into this repository's `e8/results/compare/` (gitignored), so
that the analysis can be re-run here:

```sh
cd e8/results/compare
tar czf RUNSET.tar.gz analysis/RUNSET \
  $(grep -l '^run_set: RUNSET' */*/env.txt | xargs -n1 dirname)
```

The tarball holds the run directories with every CSV (700 MB per
headline run, 28 MB of it keyverify, and 250 MB per cross-CCD run, about 8.6x smaller gzipped:
about 0.8 and 0.3 GB for the two run sets) and the analysis.  The analysis reads
`NAME.csv.gz` like `NAME.csv`, so the CSVs may also be kept gzipped
(`gzip */*/*.csv`).  Commit only what the comparison tables need
(the final `tables.md` / `tables.tex`, `summary.csv`, `ratios.csv`,
`keyverify_summary.csv`, `keyverify_ratios.csv`, `acceptance.txt` and the
`env.txt` files) under `e8/docs/`.

## 8. When a check fails

- `verify` FAIL: a broken build or a real bug; do not use the run.  Rerun
  the smoke run for the flag set; check `latency.txt` / `throughput.txt`
  for the failing configuration; for `O3native`, recheck the strict
  aliasing finding (`harness.md`, "Builds") with this compiler.
- `complete` FAIL: a step failed or was killed (`commands.txt` records
  every step's exit status; `NAME.txt` its stderr), or a CSV is missing,
  empty or truncated.  Delete the incomplete run directory (`rm -r
  e8/results/compare/FS/RUN_ID`) and repeat that run (`COMPARE_RUNS=1`,
  the same arguments); then re-run the analysis over all runs of the run
  set (section 4).
- `run_spread` FAIL: some key's run medians differ by more than 3%.  Look at
  `runs.csv` for the outlying run (a background job, a thermal event, a
  different clock in `env.txt`), fix the cause, replace that run.  Do not
  raise the tolerance to make it pass.
- `core_vs_tsc` FAIL: core cycles and TSC are not proportional, so the clock
  moved (boost, a governor change, a power limit).  Check `env.txt`
  (`boost`, `governor`, `cur_freq_khz`) and repeat the run.
- `attempts` FAIL: the signers do not reject as in the validation; stop and
  investigate (a sampler or table difference, not noise).
- `joins` FAIL: `e8/docs/precision_summary.csv` or `validation_summary.csv`
  lacks a row (a new variant or a changed name).
- `preflight` FAIL: a run was forced; repeat it with the machine set up.
- `verify_failures` or `verify_reject` FAIL: a broken build or a real
  bug, as for `verify`; do not use the run. `keyverify.txt` names any
  tampered signature that was accepted; `make test-e8` and `make
  validate-quick` run the verifier's differential tests.
- `keyverify_complete` FAIL: the keyverify step was killed or failed
  (`commands.txt`, `keyverify.txt`); repeat that run as for `complete`.
- Driver exit 2: a usage or configuration error (the message says which);
  exit 3: the preflight refused the run (each failed condition printed).
