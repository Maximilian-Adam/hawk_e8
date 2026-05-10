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
- `src/e8_vrfy.c`: uncompressed signature codec and direct E8 norm verifier.
- `src/e8_sampler.c`: standalone floating-point bounded E8 block sampler.
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

The verifier reconstructs:

```text
w = h - 2s
```

and checks the direct expanded norm:

```text
<w, Q_E8 w> <= 8n sigma_verify_E8^2
```

This is deliberately separate from ordinary HAWK verification and does not use
the ordinary determinant-one completion-of-squares formula.

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

The current sampler enumerates bounded candidates for each block, computes
`||P z_block||^2`, assigns floating-point weights, and samples from the
resulting truncated distribution. It returns both `z0,z1` and:

```text
norm2 = ||P_n z||^2
```

This sampler is experimental, floating-point, bounded/truncated,
data-dependent, and non-constant-time. It is not suitable for deployment.

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
  - These call the standalone floating-point bounded sampler.
  - The trace variant exists for tests and histogram logging.

Neither path is connected to ordinary HAWK signing.

## Current Prototype Parameters

The verifier uses the report's starting `sigma_verify_E8` values:

```text
n=256:  sigma_verify_E8 = 1.06
n=512:  sigma_verify_E8 = 1.42
n=1024: sigma_verify_E8 = 1.57
```

The current sampler-backed tests and histogram defaults use:

```text
n=256:  sigma_sign = 1.25, sigma_verify = 1.06, sampler_bound = 2
n=512:  sigma_sign = 1.28, sigma_verify = 1.42, sampler_bound = 2
n=1024: sigma_sign = 1.30, sigma_verify = 1.57, sampler_bound = 2
```

`max_attempts` defaults to `1000` in the sampler-backed tests and histogram
driver. These are prototype integration and calibration values only. They are
not final parameter claims.

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
test_e8_verify        uncompressed verifier and direct norm checks
test_e8_sign          dummy signing algebra and encoding
test_e8_sampler       standalone block sampler support and norm checks
test_e8_sign_sampler  sampler-backed uncompressed sign/verify
```

Generate quick CSV calibration logs:

```sh
make -C Reference_Implementation e8-histograms
```

Outputs:

```text
Reference_Implementation/data/e8_hist_public.csv
Reference_Implementation/data/e8_hist_signatures.csv
```

Generate long-mode logs with a bound sweep:

```sh
make -C Reference_Implementation e8-histograms-long
```

Long mode sweeps `sampler_bound` over `{2,4,6,8}` unless `E8_HIST_BOUND` is
set.

## Known Limitations

- Floating-point sampler.
- Bounded/truncated sampler support.
- Data-dependent, non-constant-time sampling and signing paths.
- No AVX2 E8 implementation.
- No compressed E8 public key or signature format.
- No final parameter calibration.
- No qROM or security proof rewrite.
- No automorphism-aware security analysis.
- No side-channel hardening.
- No production E8 key format. Tests use expanded `f,g,F,G` from
  `Hawk_keygen`.
- The direct expanded public form is used for verification; compact
  reconstruction from `qtilde00,qtilde01` is not implemented.

