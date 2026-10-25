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
`qtilde11` values for tests and verification helpers.  The public key format
(see "Public API And Key Formats") stores `qtilde00` and `qtilde01`
uncompressed; compressed E8 public keys are not implemented.

## Code Layout

Core experimental sources:

- `e8/src/hawk_e8.h`: public E8 API (key generation, signing with an
  encoded or prepared private key, verification), mirroring `hawk.h`.
- `e8/src/e8_api.c`: implementation of that API: key generation and key
  formats, key preparation, signing entry points, verification.
- `e8/src/hawk_e8_inner.h`: gated internal E8 declarations.
- `e8/src/e8_math.c`: `P_n`, `S_n`, `Delta_n`, and `Q_E8` helpers.
- `e8/src/e8_f2.c`: bit-packed F2 negacyclic arithmetic used for the signing
  target coset.
- `e8/src/e8_ntt.c`: shared internal NTT, inverse NTT, modular multiplication,
  and CRT helpers for verifier/signing arithmetic.
- `e8/src/e8_sampler.c`: coset-matched Construction-A CM E8 sampler.
- `e8/src/e8_sign.c`: dummy E8 signers and the sampler-backed uncompressed
  signing core shared by the internal `e8_sign_sampler_*` entry points and
  the public API.
- `e8/src/e8_vrfy.c`: uncompressed signature codec, sign symmetry break, and
  E8 completion-norm verifier.

The experimental E8 path targets GCC or Clang and is not strict
`-std=c99 -pedantic` code: exact CRT/reference accumulations use `__int128`,
and sampler parallelism uses pthreads and GNU `__sync` builtins.  Compilation
fails with a clear diagnostic when 128-bit integer support is unavailable.
These extensions are confined to the gated E8 path; ordinary HAWK retains its
existing portability profile.

All E8 code lives under `e8/`, separate from upstream HAWK: sources in
`e8/src/`, tests in `e8/tests/`, diagnostics in `e8/tools/`, and comparison
benchmarks in `e8/compare/`.  The shared measurement modules used by every
benchmark variant live in `e8/common/`: the SHAKE256 stream RNG
(`cmn_rng`), the spin/yield worker pool (`cmn_pool`), timing (`cmn_timing`:
fenced TSC, `CLOCK_MONOTONIC_RAW`, `CLOCK_THREAD_CPUTIME_ID`, and
`perf_event_open` core cycles) and the CSV writer (`cmn_csv`).  Dependencies
run one way: `e8/src/` may use `e8/common/`, `e8/compare/` may use both, and
`e8/common/` uses neither (only upstream `sha3.h` and `hawk.h`), so the
scheme never depends on the benchmarks.  `e8/Makefile` compiles the E8 code against the
generated, unmodified HAWK reference sources in `Reference_Implementation/`.
Benchmarks reach static HAWK internals only through `e8/compare/hawk_shim.c`,
and `make check-upstream` verifies that the upstream HAWK files are unchanged.

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

Serial mode is the reference path.  The full sampler can also use the shared
pthread spin/yield worker pool (`e8/common/cmn_pool.c`, moved unchanged from
this file so that HAWK parallel variants can use the same code) over
independent block ranges; the calling thread runs worker 0.  Parallelism is
controlled by `e8_sampler_set_thread_count()`,
`HAWK_E8_SAMPLER_THREADS`, or the compile-time `HAWK_E8_SAMPLER_THREADS`
macro.  The compile-time default is serial.  The selected benchmark/signing
diagnostic configuration uses 8, 8, and 16 sampler threads for n=256, 512, and
1024, respectively.  A serial call runs worker 0 directly and never touches
the pool; a threaded call dispatches to the pool, which only grows
(`cmn_pool.h`), so harnesses size it with `cmn_pool_ensure()` outside timed
regions.

The sampler streams are shared-module streams (`e8/common/cmn_rng.c`), read
through a 128-byte buffer as little-endian 64-bit words.  The RNG mode is one
of:

```text
per worker (default; the public API always uses it):
    SHAKE256(prefix || "HAWK-E8-CM worker sampler v2" || seed40
             || u8 logn || u8 worker_id || u64le(range_start)
             || u64le(range_end) || u64le(sigma_sign bits))
    prefix = hm || K || enc32le(2*attempt + 1)     (absorbed once per attempt)
per block (legacy, regression-only):
    SHAKE256("HAWK-E8-CM block sampler v1" || seed32 || u8 logn || u8 tau
             || u64le(block_index) || u64le(sigma_sign bits))
```

The per-worker streams are bound to the message, key and attempt exactly as
upstream HAWK binds its `sig_gauss()` streams: `hm = SHAKE256(message ||
hpub)`, `K` is the key seed for an encoded key and `f || g` for a prepared key
(the same key material as the salt derivation), and the counter is HAWK's odd
sampler counter.  The prefix is absorbed once per attempt
(`e8_sampler_prefix_init()`) and copied into each stream
(`cmn_rng_init_prefixed()`), so the binding cost does not grow with the
thread count; its permutations are counted once per attempt.  The seed is 40
bytes from the signing RNG, as for HAWK, so the signing RNG supplies
`salt_len + 40` bytes per attempt and is called exactly twice per attempt.  A
serial per-worker call uses exactly one stream (worker 0, range `[0, n/4)`),
so serial and one-thread outputs are identical by construction; with `T`
workers the output depends on `T`.  Sampler-only callers without a message
(`e8_sample_z_construction_a_cm()`) use an empty prefix.

The per-block mode is kept only so that the fixed-seed regression vectors of
the caller-supplied-salt entry points (`e8_sign_sampler_*_uncompressed()`)
stay valid: its streams are unbound, its seed is 32 bytes, and its output does
not depend on the thread count.  It is never used by the public API or in
benchmark rows.

Each block reads one word for the component and one per coordinate (72
bytes), plus one more word for each coordinate that falls to the tail table.
The streams' delivered, squeezed and refill counts and Keccak permutations are
summed per call into `e8_sampler_profile.rng` and cumulatively into
`e8_sampler_rng_counters_get()`.

The per-stage sampler profile (the `cycles_*` / `wall_ns_*` fields of
`e8_sampler_profile`) is off by default (`HAWK_E8_PROFILE_SAMPLER=0`), because
its timers run on every block inside the timed call; only
`profile-sampler-bench` and the rejection summary build it in.  Likewise,
`e8_sign_sampler_core()` takes its per-attempt sampler timestamps only when a
trace is requested.

Cache lookup and publication are protected by a pthread mutex, so concurrent
cache warm-up cannot corrupt the cache.  Full sampler and signing calls are
not reentrant across application threads because the worker pool, thread/RNG
controls, and profiling state are process-global; applications must serialize
those calls.  This does not affect the sampler's internal block parallelism.

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

The target-coset computation `t = B h mod 2` keeps the public input/output
layout as one byte per coefficient, but internally packs each polynomial into
64-bit words.  Per-key 4-bit comb tables for `f,g,F,G` reduce each F2
negacyclic product to a branchless window scan over the changing challenge
operand.  Since `-1 == 1` over F2, negacyclic wraparound is an XOR into the
low coefficients.

The inverse-basis reconstruction `w = B^{-1} z` uses the shared internal NTT
over one 31-bit prime with a proven centered reconstruction bound.  The
caller-owned prepared basis hoists `NTT(f)`, `NTT(g)`, `NTT(F)`, and `NTT(G)`
out of repeated signing calls; each attempt transforms only `z0,z1`.
Debug-check builds retain the independent schoolbook reconstruction as a
safety rail, while the shared CRT helpers remain available to verifier tests.

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

## Public API And Key Formats

`e8/src/hawk_e8.h` gives the experimental scheme the same API boundary as
ordinary HAWK (`src/hawk.h`), so that both can be timed through equivalent
calls.  With the default `HAWK_PREFIX`, the public names are `hawk_e8_*`.

| HAWK (`hawk.h`, shim) | E8 (`hawk_e8.h`) |
| --- | --- |
| `hawk_keygen` | `e8_keygen` |
| `hawk_sign_start` | `e8_sign_start` (`= shake_init(sc, 256)`) |
| `hawk_sign_finish` (encoded key) | `e8_sign_finish` (encoded key) |
| `hawk_decode_private_key` | `e8_prepare_private_key` |
| `hawk_shim_sign_finish_decoded` (decoded key) | `e8_sign_finish_prepared` (prepared key) |
| `hawk_verify_finish` | `e8_verify_finish` |

`hawk_shim_sign_finish_decoded()` (`e8/compare/hawk_shim.c`) exposes HAWK's
static `sign_finish_inner()` with the normal SHAKE sampler and a decoded
private key.  The public `hawk_sign_finish_alt()` also accepts a decoded key,
but it switches to a different sampler and salt rule, so it is not HAWK's
prepared-key mode for comparison purposes.

Formats (experimental, uncompressed; `seed_len = 8 + n/32`,
`hpub_len = n/16`, little-endian integers):

```text
encoded private key   kgseed (seed_len) || F (n x int8) || G (n x int8)
                      || hpub (hpub_len)
public key            q00 (n x int32) || q01 (n x int32)
hpub                  SHAKE256(public key), first hpub_len bytes
prepared private key  f, g, F, G, hpub, NTT basis, F2 basis
                      (implementation-defined layout, aligned internally)
signature             salt || s0 (n x int16) || s1 (n x int16)
```

| n | private key | prepared key | public key | signature |
| ---: | ---: | ---: | ---: | ---: |
| 256 | 544 | 40,960 | 2,048 | 1,038 |
| 512 | 1,080 | 40,960 | 4,096 | 2,072 |
| 1024 | 2,152 | 40,960 | 8,192 | 4,136 |

The signer needs the full `F` and `G` (not only `F mod 2` and `G mod 2`, as in
HAWK) for `w = B^-1 z`.  `f` and `g` are regenerated from `kgseed` with HAWK's
`Hawk_regen_fg()`.  `e8_keygen()` calls HAWK's internal `Hawk_keygen()`, so
`(f, g, F, G)` follow HAWK's key distribution exactly; the NTRU solver only
accepts `F` and `G` with coefficients in `[-127, +127]`, which keygen asserts.
Keygen additionally discards a basis whose `qtilde00` has a zero NTT
coefficient modulo one of the verifier's primes (such a key could never
verify; this has probability about `n/2^31` per prime).  The public key
stores the two entries the completion-norm verifier uses; `qtilde10` is the
adjoint of `qtilde01` and `qtilde11` is not needed.

The prepared key holds the per-key NTT and F2 basis tables, whose fixed
maximum-size layout makes it 40,960 bytes for every degree; it is valid only at
the address where it was prepared.  Signing with an encoded key prepares the
key in the caller's temporary buffer (`E8_TMPSIZE_SIGN(logn)` bytes) on every
call; this is E8's true all-in cost.

Signing semantics mirror `hawk_sign_finish()`:

```text
hm = SHAKE256(message || hpub)                                  once per call
for attempt = 0, 2, 4, ...:
    salt = SHAKE256(hm || K || enc32le(attempt) || rng(salt_len))
    h    = SHAKE256(hm || salt)                                 2n bits
    t    = B h mod 2;  z <- E8 sampler on 2R^2 + t              rng(32)
    reject if ||P_n z||^2 > bound;  w = B^-1 z;  symmetry break
    s    = (h - w) / 2;  reject if s does not fit int16;  encode
```

`K` is `kgseed` for an encoded key and `f || g` (2n bytes) for a prepared key,
the same rule as HAWK's encoded/decoded split, and the attempt counter
advances by 2 as in HAWK.  The RNG is called exactly twice per attempt in both
schemes (salt, then sampler seed: 32 bytes for E8, 40 bytes for HAWK); the E8
sampler derives its per-block or per-worker SHAKE streams from that seed, also
with the worker pool enabled.  Attempts can therefore be counted identically
for both schemes as `rng_calls / 2`.  The public API uses the prototype
`sigma_sign` and default `sigma_verify` values below and caps attempts
internally at 65,536 (HAWK loops until success).

One difference remains: HAWK's sampler SHAKE instances also absorb
`hm || K || enc32le(attempt + 1)` before the RNG seed, so its samples stay
key- and message-bound even with a poor RNG; the E8 sampler is seeded from the
RNG only.

The older internal `e8_sign_sampler_*` functions keep their behaviour: they
take raw `f, g, F, G` (and optionally prepared bases) and a caller-supplied
salt that is reused for every attempt.  Both paths share
`e8_sign_sampler_core()`, and the fixed-seed signature regression in
`test_e8_sign_sampler` is unchanged.

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

Build all E8 tests, tools and benchmarks:

```sh
make -C e8
```

Run the ordinary reference tests:

```sh
Reference_Implementation/bin/test_self
Reference_Implementation/bin/test_codec
Reference_Implementation/bin/test_sampler
```

Run all E8 correctness tests:

```sh
make -C e8 test-e8
```

Current E8 tests:

```text
test_e8_math          P_n/S_n/Delta_n helpers
test_e8_arith         packed F2 and NTT/CRT differential arithmetic tests
test_e8_public        expanded Q_E8 public form algebra
test_e8_verify        uncompressed verifier and norm equivalence
test_e8_sign          dummy E8 signing algebra and encoding
test_e8_sampler       block/full sampler support, cosets, and norms
test_e8_sign_sampler  sampler-backed uncompressed sign/verify, dynamic-range
                      diagnostic, and fixed-seed signature regression
test_e8_api           public API: keygen/sign (encoded and prepared)/verify
                      round trips, tampering, HAWK salt construction, RNG
                      calls per attempt, and the HAWK prepared-key shim
test_cmn_common       shared modules: RNG known answer, buffering invariance
                      and counters; pool coverage, reductions, resizing and
                      accounting; clock monotonicity and perf unavailability;
                      CSV writer
```

## Diagnostics And CSV Outputs

Histogram diagnostics:

```sh
make -C e8 e8-histograms
```
supports:

```text
E8_HIST_KEYS=<positive integer>
E8_HIST_TRIALS=<positive integer>
```


writes:

```text
e8/e8_hist_public.csv
e8/e8_hist_signatures.csv
```

Sampler timing:

```sh
make -C e8 sampler-bench
```

or for a proper comparison with checks disabled:
```sh
make -C e8 clean
make -C e8 sampler-bench CFLAGS='-O2 -DHAWK_E8_DEBUG_CHECKS=0'
```

writes `e8/e8_sampler_bench.csv` with ordinary HAWK
sampler rows and selected E8 sampler rows.  In-path sampler profiling is off
by default, so the `profile_*` columns are empty; `make -C e8
profile-sampler-bench` builds the same benchmark against a sampler compiled
with `HAWK_E8_PROFILE_SAMPLER=1` and writes `e8/e8_sampler_profile_bench.csv`
with those columns filled (its timings include the profile timers).  Defined
modes are available after building the benchmark binary:

```sh
make -C e8 bin/e8_sampler_bench
e8/bin/e8_sampler_bench --selected-configs --trials 50 --warmups 1
e8/bin/e8_sampler_bench --isolated-matrix --trials 50 --warmups 1
e8/bin/e8_sampler_bench --single-hawk-sampler --logn 10 --trials 50
e8/bin/e8_sampler_bench --single-config --logn 10 --threads 16 --worker-mode spin --rng-mode per_worker --trials 50 --warmups 1
```

Signature timing:

```sh
make -C e8 sign-bench
make -C e8 profile-sign-bench
```

`sign-bench` writes `e8/e8_sign_bench.csv`: HAWK and E8 signed through
their own `sign_finish` calls under the same two accounting modes (`all_in`,
`prepared`), one CSV row per signature in the shared schema
(`e8/common/cmn_csv.h`), every signature verified.  Both schemes draw from
the shared SHAKE256 signing RNG and are timed with the shared timing and pool
measurement; columns are defined in `e8/docs/perf_comparison_final.md`.
`profile-sign-bench` builds the same benchmark with `HAWK_E8_PROFILE_SIGN=1`
and prints per-stage E8 signing timings (including key preparation and salt
derivation) to the terminal.  For optimization timing, build with
`CFLAGS='-O2 -DHAWK_E8_DEBUG_CHECKS=0'`; debug-check builds deliberately retain schoolbook
cross-checks for the reconstructed signer coefficients.  The final symmetric
HAWK/E8 accounting, methods, and reproduction commands are recorded in
`e8/docs/perf_comparison_final.md`.

Rejection and norm summaries:

```sh
make -C e8 e8-rejection-summary
```

writes `e8/e8_rejection_summary.csv`.  The target
supports:

```text
E8_REJECTION_KEYS=<positive integer>
E8_REJECTION_TRIALS=<positive integer>
E8_REJECTION_LOGN=8|9|10
```

Sigma sweeps are available through:

```sh
make -C e8 e8-sigma-verify-sweep
make -C e8 e8-sigma-sign-sweep
```

These write `e8/e8_sigma_verify_sweep.csv` and
`e8/e8_sigma_sign_sweep.csv`.  The verify sweep uses the
rounded up smoothing `sigma_sign` values and tests `sigma_verify`.  The sign
sweep starts at the full signing space smoothing threshold with `eps_bits=32`,
rounds the tested `sigma_sign` up to 3 decimal places, and steps upward by
`0.05`.

To run more trials:

```sh
E8_SIGMA_VERIFY_SWEEP_TRIALS=100 make -C e8 e8-sigma-verify-sweep
E8_SIGMA_SIGN_SWEEP_SIGN_TRIALS=100 make -C e8 e8-sigma-sign-sweep
```

Validation summaries:

```sh
make -C e8 e8-validation-summary
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
- The E8 key formats are experimental and uncompressed, not production
  formats.
- No side-channel hardening.
- No full qROM-style unforgeability proof or complete HAWK security reduction for the modified public-form class.
- Only the HAWK-style publicly computable isometry/orbit structure is analysed; a complete automorphism-aware cryptanalysis of the E8 preconditioned public-form class remains to be done.
- The E8 verifier uses the completion-of-squares norm; tests compare it with
  the expanded direct norm.  Compact public-key reconstruction from compressed
  E8 public data is not implemented.
