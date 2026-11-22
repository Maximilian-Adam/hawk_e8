# HAWK-E8-CM

This directory holds HAWK-E8-CM, a research variant of the HAWK signature
scheme in which the hidden lattice is a direct sum of E8 lattices instead of
the integer lattice Z^2n. It is the code behind the paper "Instantiating
HAWK with a Remarkable Lattice: E8 Beyond the Hypercube".

Everything specific to E8 lives here. The rest of the repository is upstream
HAWK and stays byte-identical to the upstream merge commit; `make
check-upstream` (from the repository root) confirms it.

The design is described in [`docs/HAWK_E8_VARIANT.md`](docs/HAWK_E8_VARIANT.md).

## Status

This is experimental code, not for deployment. Two results from the paper
matter before you use it for anything:

- **It is slower than HAWK.** In a comparison where only the lattice differs
  (same sampler technique, precision, randomness, threading and build), E8
  signs 1.8 to 2.5 times slower and verifies 1.2 to 1.4 times slower. The
  measurements are in [`docs/measurements/`](docs/measurements/README.md).
- **It does not resist the recent Galois key recovery attack on HAWK.** The
  attack carries over at the same heuristic cost. The analysis is in the
  paper's Sec. 5.4, and [`docs/galois_attack.md`](docs/galois_attack.md)
  covers the small-degree experiments that check it.

The signing path uses integer arithmetic only. The `hot64` and `hot32`
samplers are not constant-time, and the `ref` sampler's constant-time
structure is checked by loop counts and code review, not by a tool such as
ctgrind.

## Building and testing

Run these from the repository root (the targets are forwarded to this
directory), or use `make -C e8 <target>`:

```sh
make e8               # build all E8 tests, tools and benchmarks
make test-e8          # run the E8 test suite
make check-upstream   # check that the upstream HAWK files are unchanged
```

E8 code is compiled with `-DHAWK_ENABLE_E8_EXPERIMENTAL=1` against the HAWK
reference sources that `../build.py` generates in `Reference_Implementation/`.
It needs GCC or Clang (it uses `__int128` and pthreads).

## Common tasks

| Task | Command | Takes | Details |
| --- | --- | --- | --- |
| Check the sampler tables match their generator | `make tables` | 1 s | [`HAWK_E8_VARIANT.md`](docs/HAWK_E8_VARIANT.md) |
| Check signing and verification use no floating point | `make check-integer-only` | 3 s | [`HAWK_E8_VARIANT.md`](docs/HAWK_E8_VARIANT.md) |
| Audit sampler precision (statistical distance, Rényi divergence) | `make precision-audit` | about 2 min | [`precision_audit.md`](docs/precision_audit.md) |
| Quick correctness and distribution validation | `make validate-quick` | 1-2 min | [`validation.md`](docs/validation.md) |
| Full validation | `make validate` | 30-40 min | [`validation.md`](docs/validation.md) |
| Smoke-test the comparison harness | `make bench-compare-smoke` | under 1 min | [`harness.md`](docs/harness.md) |
| One full comparison run | `make bench-compare` | 6-8 min | [`harness.md`](docs/harness.md) |
| Rebuild the paper's tables from the recorded measurements | `make paper-tables` | 1 s | [`harness.md`](docs/harness.md), "Paper tables" |
| Run the Galois attack experiments (n = 8, 16, 32) | `make galois-attack` | about 15 s | [`galois_attack.md`](docs/galois_attack.md) |

`make precision-audit` and `make validate` fail if their fresh summaries
differ from the committed copies in `docs/` (`precision_summary.csv`,
`validation_summary.csv`). `make tables` fails if a committed table differs
from what the generator produces.

All generated output goes under `e8/results/`, which is gitignored and
removed by `make clean`. Each kind of output gets its own subdirectory:

| Directory | Written by |
| --- | --- |
| `results/bench/` | `sign-bench`, `sampler-bench`, `variant-sampler-bench`, `profile-sampler-bench` |
| `results/diagnostics/` | `e8-histograms`, `e8-rejection-summary`, `e8-validation-summary`, the two sigma sweeps |
| `results/precision/` | `precision-audit` |
| `results/validation/`, `results/validation-quick/` | `validate`, `validate-quick` |
| `results/distribution-tests/` | `tools/distribution_tests.py` run on its own |
| `results/compare/` | the comparison harness: one directory per run, `compare/<flag set>/<run_id>/`, and the analysis in `compare/analysis/<name>/` |
| `results/paper_tables/` | `paper-tables` |
| `results/galois_attack/` | `galois-attack` |

## Timing: use the benchmark builds

The default build keeps the E8 debug checks on (`HAWK_E8_DEBUG_CHECKS=1`),
which re-check every signature with slower reference code. **Do not time the
default binaries.** That includes `make sign-bench` and `make sampler-bench`,
which run the default build.

For timing, use the per-flag-set binaries from `make bench-builds` (also part
of the default `make`). Each flag set compiles every object into its own
`build/bench-<flag set>/` directory with the debug checks off:

- `O2`: upstream HAWK's flags (`-O2`);
- `O3native`: `-O3 -march=native -fno-strict-aliasing`.

The binaries are `bin/bench_compare-<flag set>`,
`bin/bench_compare_avx2-<flag set>` (HAWK's AVX2 code, for context),
`bin/e8_sign_bench-<flag set>`, `bin/e8_sampler_bench-<flag set>` and the
profile benches. `bench_compare` and `e8_sign_bench` print their flags with
`--build-info`.

Per-stage profiling is off unless you build a `profile-*` target
(`HAWK_E8_PROFILE_SAMPLER` and `HAWK_E8_PROFILE_SIGN` default to 0). The
profile builds and the rejection summary include those timers in their
timings, so their figures are not headline numbers.

The measurements behind the paper were taken on bare-metal Linux following
[`docs/measurement_runbook.md`](docs/measurement_runbook.md). That covers
machine setup, CPU lists, the preflight checks, the exact commands of both
run sets, run time, acceptance and what to copy back. The results are
recorded in [`docs/measurements/`](docs/measurements/README.md). WSL runs are
smoke tests only.

## Rules for changing the code

- Do not edit upstream HAWK files: `src/`, `extra/`, `intermediate/`,
  `build.py`, `Reference_Implementation/`, `Optimized_Implementation/`.
- Reach static HAWK internals only through `compare/hawk_shim.c`; never
  `#include "hawk_sign.c"` anywhere else.
- Compile HAWK objects with upstream's reference flags and E8 objects with
  `-DHAWK_ENABLE_E8_EXPERIMENTAL=1`.
- Benchmark-only HAWK variants live in `compare/hawk_variants/`. Files
  derived from upstream `hawk_sign.c` say so in their header ("derived from
  upstream hawk_sign.c, modified for benchmarking"), and the upstream source
  is only ever patched into a copy under `build/`.
- Sampler tables come only from `tables/gen_tables.py`. To change a table,
  edit the generator, regenerate, and let `make tables` confirm the result.
- Keep dependencies one-way: `src/` may use `common/` and `tables/`;
  `compare/`, `tests/` and `tools/` may use all three; `common/` uses only
  upstream HAWK's `sha3.h` and `hawk.h`. The scheme never depends on the
  benchmarks.

## Documentation

| Document | What it covers |
| --- | --- |
| [`HAWK_E8_VARIANT.md`](docs/HAWK_E8_VARIANT.md) | The design: lattice, public form, sampler and its variants, randomness, signing, verification, key formats, tests and diagnostics |
| [`harness.md`](docs/harness.md) | The comparison harness: schedule, modes, builds, analysis, the paper's tables |
| [`perf_comparison_final.md`](docs/perf_comparison_final.md) | Timing accounting and the per-signature CSV schema shared by the benchmarks |
| [`measurement_runbook.md`](docs/measurement_runbook.md) | How to run the bare-metal measurements |
| [`measurements/`](docs/measurements/README.md) | The recorded bare-metal results |
| [`precision_audit.md`](docs/precision_audit.md) | Distance of every sampler variant from the ideal Gaussian |
| [`validation.md`](docs/validation.md) | Correctness and distribution validation |
| [`verifier_range.md`](docs/verifier_range.md) | Why the E8 verifier needs three NTT primes |
| [`keygen_verify_data.md`](docs/keygen_verify_data.md) | Data behind the key generation and verification tables (limits, primes, sizes) |
| [`galois_attack.md`](docs/galois_attack.md) | Small-degree experiments for the Galois key recovery attack |

## Directory reference

```text
e8/
  Makefile     builds against ../Reference_Implementation (made by ../build.py)

  src/         the scheme
    hawk_e8.h              public API, mirroring HAWK's hawk.h
    hawk_e8_inner.h        internal declarations
    e8_api.c               key generation, key formats, signing, verification
    e8_math.c              P_n, S_n, Delta_n and Q_E8
    e8_f2.c                bit-packed arithmetic mod 2 (the signing target)
    e8_ntt.c               NTT, CRT and the key generation form
    e8_sampler.c           shared part of the E8 sampler (internal interface
                           in e8_sampler_inner.h)
    e8_sampler_ref.c       sampler variant ref (constant-time full scans)
    e8_sampler_hot.c       variants hot64 and hot32 (compiled once per width)
    e8_sampler_variants.c  the variant registry
    e8_sampler_runtime.c   runtime table builder, for tests and tools only
    e8_sign.c              signing core
    e8_sign_legacy.c       older caller-supplied-salt signing entry points
                           (tests and tools)
    e8_codec.c             uncompressed signature encoding
    e8_vrfy.c              verifier (3 primes, integer-only)
    e8_vrfy_ref.c          the submission's verifier, kept for differential
                           tests

  tables/      gen_tables.py, the offline table generator (Python standard
               library), and the integer tables it generates:
    e8_tables_geom.h               E8 block geometry (all variants)
    e8_tables_{ref,hot64,hot32}.h  E8 sampler tables, per degree
    hawk_tables_{ref,hot64,hot32}.h  HAWK sampler tables
    e8_tables_ntt.h                NTT twiddles and the Delta and S_n
                                   constants of the verifier and key
                                   generation

  common/      measurement modules used identically by HAWK and E8
    cmn_rng      SHAKE256 streams and the benchmarks' signing RNG
    cmn_pool     spin/yield worker pool with per-worker CPU and cycle
                 accounting
    cmn_timing   fenced TSC, monotonic and thread CPU clocks, perf cycles
    cmn_csv      CSV writer and the per-signature schema

  tests/       test_e8_*.c, test_hawk_variants.c, test_cmn_common.c

  tools/       diagnostics and checks
    e8_histograms.c, e8_rejection_summary.c, e8_validation_summary.c
    e8_correctness.c          sign and verify with real keys
    e8_distribution_sample.c  sampler histograms
    distribution_tests.py     goodness-of-fit tests against the exact
                              implemented distributions
    validate.py               drives the validation
    precision_audit.py        the precision audit
    e8_keygen_limits.c        public form coefficient ranges over many keys
    e8_verify_diff.c          the verifier against the reference verifier
    galois_attack.py          the Galois attack experiments

  compare/     HAWK vs E8 benchmarks
    bench_compare.c          the comparison harness (latency, throughput
                             and key generation / verification modes)
    bench_sign_core.c/.h     signing core shared by bench_compare and
                             e8_sign_bench
    run_compare.sh           runs the harness and the analysis
    env_capture.sh           records a run's environment
    preflight.sh             checks the machine before a run
    ccd_cpus.sh              CPU lists for the run sets
    analysis/                analyze.py, compare_data.py, compare_tables.py
                             (statistics, bootstrap ratios, acceptance,
                             tables) and paper_tables.py
    e8_sign_bench.c          signing benchmark, every sampler variant
    e8_sampler_bench.c       sampler-only timing
    hawk_sign_profile_bench.c  HAWK's per-stage signing profile
    hawk_shim.c/.h           the only bridge to static HAWK internals
    hawk_sign_profile.patch  stage profiling, applied to a copy of hawk_sign.c
    hawk_variants/           benchmark-only HAWK sampler variants (ref-T,
                             hot64, hot32) and the patch that routes
                             upstream's sampler call through a hook

  docs/        the documents above, the committed summaries
               (precision_summary.csv, validation_summary.csv), the
               recorded measurements, and plots and a stage profile
               from the earlier prototype (e8_sampler_bench_*.png,
               perf.txt), kept for history

  results/     generated output (gitignored)
```
