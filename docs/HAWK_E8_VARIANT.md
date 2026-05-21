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
- `src/e8_vrfy.c`: uncompressed signature codec, sign-based sym-break, and E8 completion-norm verifier
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
completion-of-squares formula. The current reference code also cross-checks
accepted signatures against the expanded direct norm while this path is still
experimental.

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
large finite tail cutoff for the current research prototype. All sampler paths
are experimental, floating-point, data-dependent, and non-constant-time. They
are not suitable for deployment.

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
E8_HIST_BOUND=<positive integer>
E8_HIST_MAX_ATTEMPTS=<positive integer>
```

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

Generate long-mode logs with a bound sweep:

```sh
make -C Reference_Implementation e8-histograms-long
```

Long mode still sweeps the retained `sampler_bound` CSV column over
`{2,4,6,8}` unless `E8_HIST_BOUND` is set. With the default CM sampler this is
metadata for compatibility with earlier bounded-sampler logs; it does not
truncate the CM sampler.

## Known Limitations

- Floating-point sampler.
- The CM sampler currently uses floating-point finite-tail one-dimensional
  conditional CDFs.
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
- The verifier uses the E8 completion-of-squares norm with an expanded direct
  norm cross-check on accepted signatures; compact public-key reconstruction
  from `qtilde00,qtilde01` is not implemented.
