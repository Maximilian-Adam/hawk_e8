# Experimental HAWK-E8-CM Variant

This document describes the current experimental E8 path in this repository.
It is research code only.  It is not standard HAWK with a replacement sampler,
and it is not production cryptographic code.

The ordinary HAWK implementation, public API, compressed signature format, and
original tests remain unchanged.  E8 code is enabled only for E8-specific
reference targets with `HAWK_ENABLE_E8_EXPERIMENTAL=1`.

## Implemented Shape

The prototype works over:

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

with associated public-form weight:

```text
S_n = P_n^* P_n =
    [ 8 + 4(Y - Y^3)      4Y^2 ]
    [ -4Y^2               8    ]

Delta_n = det(S_n) = 48 + 32Y - 32Y^3
```

For a HAWK secret basis

```text
B = [ f  F ]
    [ g  G ],       fG - gF = 1
```

the expanded experimental public form is:

```text
Q_E8 = B^* S_n B
```

The implementation stores expanded `qtilde00`, `qtilde01`, `qtilde10`, and
`qtilde11` values for tests and verification helpers.  Compressed E8 public
keys are not implemented.

## Code Layout

Core experimental sources:

- `src/hawk_e8_inner.h`: gated internal E8 declarations.
- `src/e8_math.c`: `P_n`, `S_n`, `Delta_n`, and `Q_E8` helpers.
- `src/e8_sampler.c`: coset-matched Construction-A CM E8 sampler.
- `src/e8_sign.c`: dummy E8 signers and sampler-backed uncompressed signer.
- `src/e8_vrfy.c`: uncompressed signature codec, sign symmetry break, and
  E8 completion-norm verifier.

The corresponding files under `Reference_Implementation/` are the reference
test/build copies.

## Sampler Contract

The sampler decomposes a full `n`-coefficient instance into `n/4` independent
8-dimensional blocks.  For block `r`, the label is:

```text
tau_r =
(
    t0[r], t0[r+k], t0[r+2k], t0[r+3k],
    t1[r], t1[r+k], t1[r+2k], t1[r+3k]
) mod 2
```

Each block sample returns `z_block` such that:

```text
z_block in 2Z^8 + tau_r
x_block = P z_block
```

and the block weight is proportional to:

```text
exp(-||P z_block||^2 / (2 sigma_sign^2))
```

The full sampler returns `z0,z1` and:

```text
norm2 = ||P_n z||^2
```

The sampler is a direct coset-matched Construction-A CM sampler.  It decomposes

```text
C_tau = 2M + P tau = P(2Z^8 + tau)
```

into 16 RM(1,3)-labelled product-lattice components, selects a component by
its Gaussian mass, samples eight shifted one-dimensional integer Gaussian
coordinates, reconstructs the E8-side point, and maps back to `z_block` with a
checked inverse.  Debug builds check parity, coset membership, syndrome, and
norm consistency.

The implementation uses floating-point finite-tail tables and runtime caches
keyed by the exact `sigma_sign` bit pattern.  Cached probability tables,
component masses, prefixes, totals, normalisers, and tail CDFs use `double`;
hot component and six-value CDF thresholds use 64-bit integer thresholds.
The cache also stores one-dimensional centre classes, ordered tail fallback
data, base blocks, base norms, and affine reconstruction deltas.

Serial mode is the reference path.  The full sampler can also use a small
pthread spin/yield worker pool over independent block ranges.  Parallelism is
controlled by `e8_sampler_set_thread_count()`,
`HAWK_E8_SAMPLER_THREADS`, or the compile-time `HAWK_E8_SAMPLER_THREADS`
macro.  RNG mode is either per-block SHAKE streams or per-worker SHAKE streams.
The compile-time default is serial.  The selected benchmark/signing diagnostic
configuration uses 8, 8, and 16 sampler threads for n=256, 512, and 1024,
respectively.

All sampler paths are floating-point, data-dependent, and non-constant-time.

## Signing And Verification

The sampler-backed signer samples in the internal E8 coordinate coset:

```text
t = B h mod 2
z in 2R^2 + s(t)
x_E = P_n z
w = B^{-1} z
s = (h - w) / 2
```

with:

```text
B^{-1} = [ G  -F ]
         [ -g  f ]
```

Before encoding, the signer applies a simple sign symmetry break: compare `w`
and `-w`, scan `w1` from coefficient 0 upward and then `w0`, and keep the
representative whose first non-zero coefficient is positive.  The all-zero
vector is accepted.  This is not full E8 orbit canonicalisation.

The verifier decodes an uncompressed signature, reconstructs:

```text
w = h - 2s
```

checks the same sign representative rule, then uses the E8
completion-of-squares norm:

```text
n ||w||^2_QE8 = Tr(q00 e e^* + Delta_n d w1^*)
d = w1 / q00
e = w0 + q01 d
```

Tests compare this completion norm with the expanded direct `Q_E8` norm.

## Signature Format

Only an uncompressed experimental E8 signature is implemented:

```text
salt || s0 || s1
```

where `s0` and `s1` are little-endian signed 16-bit coefficient arrays of
length `n`.

Current salt lengths:

```text
n=256:  14 bytes
n=512:  24 bytes
n=1024: 40 bytes
```

The current uncompressed signature size is:

```text
salt_len + 4*n
```

No compressed E8 signature format is implemented.

## Prototype Parameters

Current sampler-backed tests, histogram diagnostics, rejection summaries, and
stability diagnostics use:

```text
n=256:  sigma_sign = 3.592, sigma_verify = 2.03
n=512:  sigma_sign = 3.631, sigma_verify = 1.99
n=1024: sigma_sign = 3.669, sigma_verify = 1.95
```

`max_attempts` defaults to `1000` in the sampler-backed signing tests and CSV
diagnostics.  These are prototype integration values, after some degree of tuning, but are not final parameter
claims.

The verifier threshold helper is:

```text
bound = floor(8.0 * n * sigma_verify^2)
```

The C API uses the `sigma` convention:

```text
exp(-norm2 / (2 sigma^2))
```

For lattice-theory `rho_s` notation:

```text
s = sqrt(2*pi) * sigma
```

## Build And Test

Build the reference implementation:

```sh
make -C Reference_Implementation
```

Run the ordinary reference tests:

```sh
Reference_Implementation/bin/test_self
Reference_Implementation/bin/test_codec
Reference_Implementation/bin/test_sampler
```

Run all E8 correctness tests:

```sh
make -C Reference_Implementation test-e8
```

Current E8 tests:

```text
test_e8_math          P_n/S_n/Delta_n helpers
test_e8_public        expanded Q_E8 public form algebra
test_e8_verify        uncompressed verifier and norm equivalence
test_e8_sign          dummy E8 signing algebra and encoding
test_e8_sampler       block/full sampler support, cosets, and norms
test_e8_sign_sampler  sampler-backed uncompressed sign/verify
```

## Diagnostics And CSV Outputs

Histogram diagnostics:

```sh
make -C Reference_Implementation e8-histograms
```
supports:

```text
E8_HIST_KEYS=<positive integer>
E8_HIST_TRIALS=<positive integer>
```


writes:

```text
Reference_Implementation/e8_hist_public.csv
Reference_Implementation/e8_hist_signatures.csv
```

Sampler timing:

```sh
make -C Reference_Implementation sampler-bench
```

or for a proper comparison with checks disabled:
```sh
make -C Reference_Implementation clean
make -C Reference_Implementation sampler-bench E8_CFLAGS='-O2 -DHAWK_ENABLE_E8_EXPERIMENTAL=1 -DHAWK_E8_DEBUG_CHECKS=0 -DHAWK_E8_PROFILE_SAMPLER=0'
```

writes `Reference_Implementation/e8_sampler_bench.csv` with ordinary HAWK
sampler rows and selected E8 sampler rows.  Defined modes are available after
building the benchmark binary:

```sh
make -C Reference_Implementation bin/e8_sampler_bench
Reference_Implementation/bin/e8_sampler_bench --selected-configs --trials 50 --warmups 1
Reference_Implementation/bin/e8_sampler_bench --isolated-matrix --trials 50 --warmups 1
Reference_Implementation/bin/e8_sampler_bench --single-hawk-sampler --logn 10 --trials 50
Reference_Implementation/bin/e8_sampler_bench --single-config --logn 10 --threads 16 --worker-mode spin --rng-mode per_worker --trials 50 --warmups 1
```

Signature timing:

```sh
make -C Reference_Implementation sign-bench
make -C Reference_Implementation profile-sign-bench
```

`sign-bench` writes `Reference_Implementation/e8_sign_bench.csv`.
`profile-sign-bench` builds the dedicated signature benchmark with
`HAWK_E8_PROFILE_SIGN=1` and prints per-stage E8 signing timings to the
terminal.

Rejection and norm summaries:

```sh
make -C Reference_Implementation e8-rejection-summary
```

writes `Reference_Implementation/e8_rejection_summary.csv`.  The target
supports:

```text
E8_REJECTION_KEYS=<positive integer>
E8_REJECTION_TRIALS=<positive integer>
E8_REJECTION_LOGN=8|9|10
```

Sigma sweeps are available through:

```sh
make -C Reference_Implementation e8-sigma-verify-sweep
make -C Reference_Implementation e8-sigma-sign-sweep
```

These write `Reference_Implementation/e8_sigma_verify_sweep.csv` and
`Reference_Implementation/e8_sigma_sign_sweep.csv`.  The verify sweep uses the
rounded up smoothing `sigma_sign` values and tests `sigma_verify`.  The sign
sweep starts at the full signing space smoothing threshold with `eps_bits=32`,
rounds the tested `sigma_sign` up to 3 decimal places, and steps upward by
`0.05`.

To run more trials:

```sh
E8_SIGMA_VERIFY_SWEEP_TRIALS=100 make -C Reference_Implementation e8-sigma-verify-sweep
E8_SIGMA_SIGN_SWEEP_SIGN_TRIALS=100 make -C Reference_Implementation e8-sigma-sign-sweep
```

Validation summaries:

```sh
make -C Reference_Implementation e8-validation-summary
```

writes:

```text
e8_validation_summary.csv     manifest of generated validation CSVs
e8_validation_shell.csv       low-shell sanity histogram versus finite reference
e8_validation_coset.csv       all-256 tau parity/coset/norm summary
e8_validation_global.csv      1000-trial global norm factorisation check
e8_validation_stability.csv   HAWK vs serial E8 wall-clock stability summary
```

The validation summary is a diagnostic sanity check.  It is not a formal
statistical proof of sampler distribution.

## Limitations

- Experimental floating-point sampler.
- Data-dependent, non-constant-time sampling and signing paths.
- No AVX2 E8 implementation.
- No compressed E8 public key or signature format.
- No production E8 key format.
- No side-channel hardening.
- No full qROM-style unforgeability proof or complete HAWK security reduction for the modified public-form class.
- Only the HAWK-style publicly computable isometry/orbit structure is analysed; a complete automorphism-aware cryptanalysis of the E8 preconditioned public-form class remains to be done.
- The E8 verifier uses the completion-of-squares norm; tests compare it with
  the expanded direct norm.  Compact public-key reconstruction from compressed
  E8 public data is not implemented.
