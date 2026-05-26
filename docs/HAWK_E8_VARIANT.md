# Experimental E8-Preconditioned HAWK-Like Variant

This document describes the current experimental E8-preconditioned variant in
this repository. It is research code only. It is not standard HAWK with a new
sampler, and it is not a production cryptographic implementation.

The original HAWK implementation remains unchanged. The E8 code is built only
when `HAWK_ENABLE_E8_EXPERIMENTAL=1` is defined by the E8-specific reference
Makefile targets.

## Implemented Shape

The current prototype uses:

```text
R = Z[X] / (X^n + 1), n in {256,512,1024}
k = n / 4
Y = X^k
```

The E8 preconditioner is:

```text
P_n =
    [ 2(1 + Y)      1 - Y + Y^2 + Y^3 ]
    [ 0             1 + Y + Y^2 + Y^3 ]
```

The associated public-form weight is:

```text
S_n = P_n^* P_n =
    [ 8 + 4(Y - Y^3)      4Y^2 ]
    [ -4Y^2               8    ]
```

and:

```text
Delta_n = det(S_n) = 48 + 32Y - 32Y^3
```

The HAWK-style secret basis convention is:

```text
B = [ f  F ]
    [ g  G ]

fG - gF = 1
```

The E8 public form is computed as:

```text
Q_E8 = B^* S_n B
```

The implementation stores and tests the expanded experimental form
`qtilde00`, `qtilde01`, `qtilde10`, and `qtilde11`. Public-key compression is
not implemented.

## Code Layout

Core experimental files:

- `src/hawk_e8_inner.h`: gated internal E8 declarations.
- `src/e8_math.c`: `P_n`, `S_n`, `Delta_n`, and `Q_E8` helpers.
- `src/e8_vrfy.c`: uncompressed signature codec, sign-based sym-break, and
  E8 completion-norm verifier.
- `src/e8_sampler.c`: standalone E8 block samplers, including the
  coset-matched Construction-A CM sampler and the bounded baseline.
- `src/e8_sign.c`: dummy E8 signers and sampler-backed uncompressed signer.

The corresponding files under `Reference_Implementation/` are kept in sync for
the reference test target.

The ordinary HAWK files, public API, compressed signature format, and original
tests are not changed by the E8 path.

## Coset Interface

The E8 signer samples in the internal `P_n`-coordinate coset:

```text
t = B h mod 2
C_t = P_n(2R^2 + s(t))
```

The sampler outputs `z` in:

```text
z in 2R^2 + s(t)
```

with weight proportional to:

```text
exp(-||P_n z||^2 / (2 sigma_sign^2))
```

The signature algebra then uses:

```text
x_E = P_n z
w = B^{-1} z
s = (h - w) / 2
```

where:

```text
B^{-1} = [ G  -F ]
         [ -g  f ]
```

Before `s` is encoded, the uncompressed E8 signer applies the same simple
sign-based symmetry break style used by HAWK signatures: among `w` and `-w`,
scan the coefficients of `w1` from index `0` upward, then `w0` as fallback,
and keep the representative whose first non-zero coefficient is positive.  The
all-zero vector is accepted.  Since `-w = w mod 2`, this does not change the
integrality condition for `s = (h - w) / 2`.

The verifier reconstructs:

```text
w = h - 2s
```

It first enforces the same sign-based symmetry break, then checks the
completion-of-squares norm:

```text
n ||w||^2_QE8 = Tr(q00 e e^* + Delta_n d w1^*)
d = w1 / q00
e = w0 + q01 d
```

This is deliberately separate from ordinary HAWK verification. The E8 path uses
the `Delta_n`-weighted formula above, not the ordinary determinant-one
completion-of-squares formula. The verifier does not run the expanded direct
`Q_E8` norm check; that slower equivalence check is kept in tests while this
path is still experimental.

This is not full public-orbit canonicalisation for the E8 form.  The
implementation does not apply rotations, `omega_E8_Q`, or a full `G_E8,Q`
orbit rule here; it only chooses a deterministic sign representative at the
signature level.

## Signature Format

Only an uncompressed experimental signature is implemented:

```text
salt || s0 || s1
```

where `s0` and `s1` are little-endian signed 16-bit coefficient arrays of
length `n`.

Current salt lengths are:

```text
n=256:  14 bytes
n=512:  24 bytes
n=1024: 40 bytes
```

Thus the current uncompressed signature size is:

```text
salt_len + 4*n
```

No compressed E8 signature format is implemented.

## Sampler Architecture

The standalone sampler in `e8_sampler.c` decomposes the lattice into `n/4`
independent 8-dimensional blocks. For block `r`, it extracts:

```text
tau_r =
(
    t0[r], t0[r+k], t0[r+2k], t0[r+3k],
    t1[r], t1[r+k], t1[r+2k], t1[r+3k]
) mod 2
```

Each block sample satisfies:

```text
z_block in 2Z^8 + tau_r
```

The default sampler-backed signer now calls:

```text
e8_sample_z_construction_a_cm
```

This sampler treats `tau_r` as an internal `P`-coordinate label. It returns
the HAWK-E8-CM coordinate vector `z_block`, not merely an abstract E8-side
vector. The measured E8-side vector is:

```text
x_block = P z_block
```

and the target weight is:

```text
exp(-||P z_block||^2 / (2 sigma_sign^2)).
```

The implementation is now a direct coset-matched Construction-A sampler.  It
decomposes the exact HAWK-E8-CM coset

```text
C_tau = 2M + P tau_r = P(2Z^8 + tau_r)
```

into 16 RM(1,3)-labelled product-lattice components before sampling.  The
Gaussian mass of each component is computed with the E8-side norm
`||x_block||^2`; one component is selected by that mass, eight shifted
one-dimensional integer Gaussian coordinates are sampled inside the selected
product lattice, and the resulting E8-side point is converted back with a
checked `P^{-1}` map.  Thus RM(1,3) code data drives component selection and
product-lattice sampling; it is not post-sampling bookkeeping.

The sampler returns both `z0,z1` and:

```text
norm2 = ||P_n z||^2
```

The old bounded enumerator remains available only as an explicitly named
experimental baseline:

```text
e8_sample_block_bounded_float
e8_sample_z_bounded_float
```

The compatibility names `e8_sample_block_float` and `e8_sample_z_float` still
refer to that bounded baseline for older tests and diagnostics.

The CM sampler uses floating-point one-dimensional conditional CDFs with a
large finite tail cutoff for the current research prototype.  Its default path
uses a runtime cache keyed by the exact `sigma_sign` bit pattern, the derived
`sigma_coord = sigma_sign / sqrt(32)` bit pattern, and the exact
Construction-A one-dimensional centre numerator over denominator `32`.  The
cache stores:

```text
lo, hi, total mass, and cumulative weights for each shifted 1D table
per-tau masses and prefix sums for the 16 RM(1,3) components
32-bit per-tau component CDFs for fixed-integer component selection
dense centre-class rows for the eight product-coordinate draws
ordered tail candidates for rare 1D fallback draws
base z-blocks, base norms, and coordinate delta vectors for affine reconstruction
```

Each finite shifted centre also has a tiny hot sampler for the current
prototype: the six largest-mass integer values for that centre, plus a 32-bit
integer CDF. The finite set of shifted centres is represented by dense
`uint8_t` centre classes, so each `(tau, component, coord)` slot maps directly
to a compact hot row. The CM hot path samples this row first. Only the rare
remaining tail falls back to an ordered compact tail CDF for that centre. The
hot and tail candidate order is descending mass, which for these small sigmas is
nearest-to-centre order. The common path avoids the long `lo..hi` CDF scan and
avoids loading the general table metadata. This is a performance approximation
to the current experimental sampler, not a claim of exact discrete-Gaussian
sampling.

Component selection is also table-driven in the hot path: the cached double
component masses are retained for diagnostics, while sampling uses a compact
`uint32_t component_cdf[256][16]` table.  After the eight product coordinates
are sampled, the non-debug hot path reconstructs `z_block` directly from the
cached affine form

```text
z_block = base_z[tau][component]
        + sum_i coord[i] * delta_z[i]
```

and computes `norm2` from the cached base norm and the orthogonal
product-coordinate quadratic formula.  Debug builds still reconstruct the
E8-side block and check the inverse map, coset, syndrome, and norm consistency.
The full-ring sampler also batches RNG callback output into a small local byte
stream and consumes `uint32_t`/`uint64_t` words from it for component and
coordinate draws; this changes deterministic test traces but not the sampler
support contract.

`e8_sampler_warm_cache(sigma_sign)` explicitly precomputes the tables for one
`sigma_sign`; the full CM sampler also warms lazily on first use.  Warm-up does
not consume sampler RNG output.  The cache is an optimisation only, not a
constant-time hardening mechanism.

The internal macro `HAWK_E8_DEBUG_CHECKS` defaults to `1`.  In that mode the
CM sampler keeps the reconstruction, parity, syndrome, and norm consistency
checks used by the test builds.  Benchmark or release builds may compile with
`HAWK_E8_DEBUG_CHECKS=0` to skip those extra prototype checks.

Keep the default at `1` for development and correctness tests.  For
sampler-speed comparisons against ordinary HAWK, rebuild the E8 benchmark with
`-DHAWK_E8_DEBUG_CHECKS=0`; these checks are prototype assertions rather than
part of the sampler contract.  In local measurements this changed the
sampler-only gap from roughly `1.8x` slower than HAWK to roughly `1.4x`, so
benchmark notes should record whether debug checks were enabled.

All sampler paths are experimental, floating-point, data-dependent, and
non-constant-time. They are not suitable for deployment.

## Signer Separation

There are two intentionally separate E8 signing paths:

- Dummy signers in `e8_sign.c`
  - `e8_sign_dummy_uncompressed`
  - `e8_sign_dummy_offset_uncompressed`
  - These exercise the algebra and uncompressed encoding only.
  - They do not sample from the target E8 distribution.

- Sampler-backed signer in `e8_sign.c`
  - `e8_sign_sampler_uncompressed`
  - `e8_sign_sampler_trace_uncompressed`
  - These call the standalone coset-matched Construction-A CM sampler.
  - The trace variant exists for tests and histogram logging.

Neither path is connected to ordinary HAWK signing.

## Construction-A CM Adaptation Note

The Sage prototype samples an abstract Construction-A E8 coset by selecting
RM(1,3) code data and then sampling product-lattice coordinates. The C
implementation does not expose that abstract E8 vector as the HAWK sample.
Instead, for each HAWK-E8-CM block it keeps the exact block contract:

```text
C_tau = 2M + P tau = P(2Z^8 + tau).
```

For each block, write `z_block = tau + 2y`.  The lift `y mod 2` is decomposed
into the 16 cosets of the Construction-A lattice `RM(1,3)+2Z^8`.  For a chosen
representative `r`, the corresponding E8-side component is

```text
P tau + 2P r + 2P(RM(1,3)+2Z^8).
```

In the implemented coordinate bridge, the kernel
`2P(RM(1,3)+2Z^8)` has an explicit orthogonal product basis with squared
length `32` in each coordinate.  This is the product lattice sampled by the
eight one-dimensional shifted discrete Gaussian samplers.  After reconstruction
the code checks `x_block = P z_block`, `z_block = tau mod 2`, and
`norm2 = ||x_block||^2 = ||P z_block||^2`.

## Current Prototype Parameters

The verifier uses these starting `sigma_verify_E8` values:

```text
n=256:  sigma_verify_E8 = 1.06
n=512:  sigma_verify_E8 = 1.42
n=1024: sigma_verify_E8 = 1.57
```

Verifier thresholds are computed through the shared helper
`e8_verify_bound_from_sigma`:

```text
bound = floor(8.0 * n * sigma_verify_E8 * sigma_verify_E8)
```

`e8_verify_uncompressed` uses the default fixed values above.
`e8_verify_uncompressed_with_sigma` is available for experimental tests that
pass an explicit `sigma_verify`, so the signer restart threshold and verifier
threshold can be kept identical.

The current sampler-backed tests and histogram defaults use:

```text
n=256:  sigma_sign = 1.25, sigma_verify = 1.06, sampler_bound = 2
n=512:  sigma_sign = 1.28, sigma_verify = 1.42, sampler_bound = 2
n=1024: sigma_sign = 1.30, sigma_verify = 1.57, sampler_bound = 2
```

`max_attempts` defaults to `1000` in the sampler-backed tests and histogram
driver. These are prototype integration and calibration values only. They are
not final parameter claims.

`sampler_bound` is retained in the existing test/histogram interfaces for the
bounded baseline and for backward-compatible CSV columns. The default
sampler-backed signer uses the CM sampler above and does not use this value to
truncate the block support.

The C API uses the `sigma` convention:

```text
exp(-norm2 / (2 sigma^2)).
```

When comparing with Sage code written in the lattice-theory `rho_s`
convention, use:

```text
s = sqrt(2*pi) * sigma.
```

The histogram target supports:

```text
E8_HIST_KEYS=<positive integer>
E8_HIST_TRIALS=<positive integer>
E8_HIST_BOUND=<positive integer>
E8_HIST_MAX_ATTEMPTS=<positive integer>
```

In short mode, `E8_HIST_KEYS` and `E8_HIST_TRIALS` apply uniformly to every
listed `logn`; the defaults are 3 keys and 5 trials per key per `logn`, so the
short correctness CSVs do not silently use fewer keys at larger dimensions.

The sampler-backed signing test supports:

```text
E8_SIGN_TEST_BOUND=<positive integer>
```

If unset, defaults are unchanged. Invalid bound values fail with a clear error.

## Build And Test Commands

Build the original reference implementation:

```sh
make -C Reference_Implementation
```

Run the original reference test binaries:

```sh
Reference_Implementation/bin/test_self
Reference_Implementation/bin/test_codec
Reference_Implementation/bin/test_sampler
```

Build and run all current E8 tests:

```sh
make -C Reference_Implementation test-e8
```

Current E8 test coverage is split across:

```text
test_e8_math          P_n/S_n consistency and Delta_n
test_e8_public        Q_E8 public form algebra
test_e8_verify        uncompressed verifier, completion norm, and direct norm checks
test_e8_sign          dummy signing algebra and encoding
test_e8_sampler       standalone block sampler support, CM cosets, and norms
test_e8_sign_sampler  sampler-backed uncompressed sign/verify
```

Generate quick CSV calibration logs:

```sh
make -C Reference_Implementation e8-histograms
```

Outputs:

```text
Reference_Implementation/e8_hist_public.csv
Reference_Implementation/e8_hist_signatures.csv
```

The signature CSV keeps the sampler/norm fields used for calibration
(`pnorm`, `qnorm`, `norm_equal`, attempts, rejected attempts, and coefficient
ranges) and appends correctness-result and timing fields:

```text
test_type, expected_verify, verify_success, norm_margin, verify_bound
coset_check_success, piM_matches_t, ambient_mod2_matches_t
target_t_weight, piM_t_weight, ambient_t_weight
coset_error_count, ambient_error_count
cycles_keygen, cycles_public_form, cycles_sample_total, cycles_sample_last
cycles_sign_total, cycles_verify
wall_ns_keygen, wall_ns_public_form, wall_ns_sample_total
wall_ns_sample_last, wall_ns_sign_total, wall_ns_verify
```

The coset diagnostics distinguish the HAWK-E8-CM quotient check
`pi_M(P_n z) = z mod 2 = t` from the non-canonical ambient diagnostic
`P_n z mod 2`.  The internal quotient check is the mathematical signing
interface; ambient coefficientwise reduction is reported for debugging only.

For each generated sampled signature it writes one `valid_signature` row and
six negative verification rows: `tamper_message`, `tamper_s0`, `tamper_s1`,
`tamper_salt`, `tamper_hpub`, and `tamper_public_form`.
Cycle counts use x86-64 timestamp counters when available and are zero on
unsupported platforms; wall-clock timings use `CLOCK_MONOTONIC_RAW` where
available.  Tamper rows reuse the corresponding valid signature's
keygen/public-form/sign/sample timings and measure verifier time separately.

Generate sampler-isolated timing rows:

```sh
make -C Reference_Implementation sampler-bench
```

For sampler-speed comparisons, disable E8 debug consistency checks explicitly:

```sh
make -C Reference_Implementation clean
make -C Reference_Implementation sampler-bench E8_CFLAGS='-Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always -DHAWK_ENABLE_E8_EXPERIMENTAL=1 -DHAWK_E8_DEBUG_CHECKS=0'
```

For a longer run, set `E8_SAMPLER_BENCH_TRIALS`; for example, this runs 1000
warm trials per supported `logn`:

```sh
E8_SAMPLER_BENCH_TRIALS=1000 make -C Reference_Implementation sampler-bench E8_CFLAGS='-Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always -DHAWK_ENABLE_E8_EXPERIMENTAL=1 -DHAWK_E8_DEBUG_CHECKS=0'
```

This benchmark emits sampler-scope and signature-scope rows.  The current row
types are:

```text
hawk_sampler
e8_sampler_cached_cold_full
e8_sampler_cached_warm_block
hawk_sign
e8_sign_sampler_cached
```

`hawk_sampler` times the ordinary HAWK signing sampler.  The ordinary sampler
produces a full `2n` scalar batch internally, so `cycles_total` is the full
sampler call and `cycles_per_unit` is amortized to one eight-scalar unit.

`e8_sampler_cached_cold_full` times one full
`e8_sample_z_construction_a_cm` call including lazy cache warm-up for that
`sigma_sign`.  This row is a startup-cost diagnostic, not the intended
steady-state sampler comparison.  `e8_sampler_cached_warm_block` also times the
full `e8_sample_z_construction_a_cm` call, but with the cache already warm; its
name is historical, and its per-unit columns are amortized over the `n/4` E8
blocks.  Thus the E8 sampler row's `cycles_total` is now a full-dimension
sampler measurement, not a single-block measurement.

`hawk_sign` times ordinary compressed HAWK signing through
`hawk_sign_finish`, with key generation and message setup outside the timed
region.  `e8_sign_sampler_cached` times the experimental uncompressed
HAWK-E8-CM signing path through `e8_sign_sampler_trace_timed_uncompressed`,
again with setup outside the timed region and with the E8 sampler cache warm.
These two `signature` rows are the closest signature-level comparison, though
they still compare ordinary compressed HAWK with the experimental uncompressed
E8 path.

For signature-only runs over `logn = 8, 9, 10`:

```sh
make -C Reference_Implementation sign-bench
make -C Reference_Implementation profile-sign-bench
```

The first command writes `Reference_Implementation/e8_sign_bench.csv` with the
normal `hawk_sign` and `e8_sign_sampler_cached` rows.  The second builds the same
benchmark with `HAWK_E8_PROFILE_SIGN=1` and prints a per-stage E8 signing
summary to the terminal without writing a CSV file.  The summary reports
hash/challenge generation, mod-2 target/coset computation, E8 sampling,
reconstruction, norm/rejection logic, encoding, and attempts/rejections.

`E8_SAMPLER_BENCH_TRIALS` controls the number of warm HAWK/E8 trials per
`logn`, and the target writes
`Reference_Implementation/e8_sampler_bench.csv`.

Generate aggregate rejection and norm statistics for valid signing only:

```sh
make -C Reference_Implementation e8-rejection-summary
```

This writes `Reference_Implementation/e8_rejection_summary.csv` with one row
per `(logn, key_index)`.  It does not emit tamper rows and does not write one
row per signature.  The harness is a diagnostic experiment for rejection,
norm, coset, and timing summaries; it is not a security claim or final
parameter calibration.

The rejection summary target supports:

```text
E8_REJECTION_KEYS=<positive integer>
E8_REJECTION_TRIALS=<positive integer>
E8_REJECTION_LOGN=8|9|10
```

For example:

```sh
E8_REJECTION_LOGN=10 E8_REJECTION_TRIALS=10000 E8_REJECTION_KEYS=5 make -C Reference_Implementation e8-rejection-summary
```

## Known Limitations

- Floating-point sampler.
- The CM sampler currently builds floating-point finite-tail one-dimensional
  conditional CDFs and component masses at runtime, then uses fixed 32-bit
  component CDFs plus centre-class-indexed tiny 32-bit hot CDFs with a
  floating-point ordered-tail fallback.
- First use of a new `sigma_sign` pays the cache-build cost.  Static
  pregenerated tables could remove that startup wait, but they are not
  implemented in this repository.
- The previous bounded/truncated sampler remains only as a baseline.
- Data-dependent, non-constant-time sampling and signing paths.
- No AVX2 E8 implementation.
- No compressed E8 public key or signature format.
- No final parameter calibration.
- No qROM or security proof rewrite.
- No automorphism-aware security analysis.
- No side-channel hardening.
- No production E8 key format. Tests use expanded `f,g,F,G` from
  `Hawk_keygen`.
- The verifier uses the E8 completion-of-squares norm. Tests still compare it
  with the expanded direct norm; compact public-key reconstruction from
  `qtilde00,qtilde01` is not implemented.
