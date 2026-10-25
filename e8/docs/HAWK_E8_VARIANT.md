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
- `e8/src/e8_sampler.c`: shared part of the coset-matched Construction-A CM
  E8 sampler (block geometry, controls, stream derivations, worker
  dispatch); `e8_sampler_inner.h` is its internal interface.
- `e8/src/e8_sampler_hot.c` (compiled once per threshold width, variants
  `hot64` and `hot32`), `e8/src/e8_sampler_ref.c` (variant `ref`) and
  `e8/src/e8_sampler_variants.c` (the variant registry): the sampler
  variants on the generated tables.
- `e8/src/e8_sampler_runtime.c`: test/tool-only runtime `hot64` table
  builder (floating point) for arbitrary `sigma_sign`, and the legacy
  double-sigma sampler entry points.
- `e8/src/e8_sign.c`: dummy E8 signers and the integer-only sampler-backed
  uncompressed signing core shared by the public API and
  `e8/src/e8_sign_legacy.c` (the caller-supplied-salt, double-sigma
  `e8_sign_sampler_*` entry points of tests and tools).
- `e8/src/e8_codec.c`: uncompressed signature codec.
- `e8/src/e8_vrfy.c`: sign symmetry break and E8 completion-norm verifier.
- `e8/tables/`: the offline table generator `gen_tables.py` and the
  generated integer tables of every sampler variant of both schemes.

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

The sampler runs on integer tables generated offline for the three fixed
`sigma_sign` values (see "Sampler Variants"); the signing path is
integer-only.  The double-sigma entry points used by tests and tools build
`hot64` tables of the same format at runtime, in `double`, for any
`sigma_sign`.

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

The `hot64` and `hot32` variants are data-dependent and not constant-time;
the `ref` variant uses constant-time full scans (see "Sampler Variants").

## Sampler Variants

Step 4 of the fair-comparison plan gives HAWK and E8 the same sampler
variants, so that every headline row pair differs only in the lattice.  All
variants of both schemes are linked into one binary and listed by
`e8_sign_bench --list`:

```text
scheme  variant  threshold_bits  technique     notes
hawk    ref      78              ct_full_scan  upstream sig_gauss(), unchanged
hawk    ref-T    78              ct_full_scan  upstream table and scan, worker streams
hawk    hot64    64              vt_hot_table  generated tables
hawk    hot32    32              vt_hot_table  generated tables
e8      ref      78              ct_full_scan  generated tables
e8      hot64    64              vt_hot_table  generated tables (public API default)
e8      hot32    32              vt_hot_table  generated tables
```

Every variant except upstream HAWK `ref` runs with `T` workers (`T = 1` is its
serial form, which never touches the pool); the thread count is reported
beside the variant.

E8 registry: `e8_sampler_variants[]` (`hawk_e8_inner.h`), each with a name,
threshold width, technique, `sample_z` (the full-dimension sampler, one
indirect call per attempt from the signing core) and its generated tables per
logn.  `e8_sign_finish_inner()` takes the variant (NULL: `hot64`).  Each
variant is its own translation unit (`e8_sampler_hot.c` compiled with
`E8_HOT_BITS=64` and `=32`, `e8_sampler_ref.c`) with its own symbols; the
per-block loop is direct within it.  HAWK registry: `hawk_sampler_variants[]`
(`e8/compare/hawk_variants/hawk_variants.h`).

### Hot-table variants (`hot64`, `hot32`)

Per E8 block: one `B`-bit word selects the component against the tau's
component CDF; each of the eight coordinates reads one `B`-bit word against
its centre class's hot table of the `K = 6` highest-weight values and, on a
miss, a second `B`-bit word against the class's integer tail table.  Every
comparison is `target <= threshold`, at both widths.  (The 32-bit sampler of
commit `b7bf0fc` compared `target < threshold`, with thresholds
`(uint64_t)(prefix / total * 2^32)` clamped to `UINT32_MAX` and a `double`
tail; using one rule at every width keeps the precision audit uniform.)
HAWK's `hot64` and `hot32` use the same skeleton per coordinate on its two
distributions (`x in 2Z + p`, `x = 2y + p`, `y ~ D(-p/2, sigma_sign)`, by the
parity `p` of `t`), with the same `K`, rule, integer tail and read widths.
With `K = 6`, HAWK's coordinates fall to the tail with probability 0.51% /
0.20% (n = 256, parity 0 / 1), 2.48% / 1.60% (512) and 2.71% / 1.79% (1024),
against about 1e-5 for E8 (HAWK's distributions are wider in lattice units:
`sigma_sign` 1.01-1.30 against E8's 0.64).

### Constant-time variants (`ref`)

HAWK `ref` is upstream `sig_gauss()`: 78-bit one-sided table, sign bit,
constant-time full scan, four interleaved SHAKE256 instances.  HAWK `ref-T`
uses the same table and per-coordinate code on the worker streams below,
reading each range's draws sequentially (groups of five words per four
coordinates); its per-coordinate distribution equals upstream's, its mapping
does not.

E8 `ref` (`e8_sampler_ref.c`) uses 78-bit thresholds and HAWK's draw format:
every draw is `hi (15 bits) || lo (63 bits)`, a group of five little-endian
words giving four draws (words 0-3: low parts, bit 63 unused; word 4: the
four high parts in its 16-bit lanes).  A block uses nine draws (component,
then eight coordinates); groups continue across the blocks of a stream and a
stream's last partial group is read whole, so a stream of `b` blocks reads
`ceil(9b/4) * 40` bytes (22.5n bytes per attempt serially; up to 30 more per
worker).  Comparisons are HAWK's borrow form (`cc = (lo - Tlo) >> 63`, then
`(hi - Thi - cc) >> 31`), over whole tables with no early exit:

- component: the secret tau selects a row class (6 distinct component mass
  orders) and a slot-to-component permutation by a masked scan over all 256
  taus; the row class's 15 survival thresholds by a masked scan over the 6
  classes; the slot is the count of thresholds above `u`, the component the
  slot's 4-bit field of the permutation;
- coordinates: the base block `tau + 2 rep(component)`, its offset `P z`,
  squared norm and the centres `c = k/4` are computed arithmetically; the
  residue `k mod 4` selects one of four two-sided survival tables (13
  entries) by a masked scan, and the value is `floor(k/4) + lo + #{j : u <
  T[j]}`.

`test_e8_ref_ct` links a build whose loops count their iterations and checks
that every block, for every tau and random input, runs the same counts (256
tau scans, 90 row-class entries, 119 comparisons, 416 residue entries).
Valgrind/ctgrind is not available here, so the absence of other
secret-dependent branches and memory indices rests on code review.

### RNG layout per variant

Both schemes draw every sampler stream like upstream HAWK: bound to the
message, key and attempt, from a 40-byte seed read from the signing RNG
(`option B` of the plan).  The prefix is HAWK's
`hm || K || enc32le(2*attempt + 1)` (`K` = key seed for an encoded key, `f || g`
for a prepared/decoded key), absorbed once per attempt and copied into each
stream.  The signing RNG is called exactly twice per attempt (salt, seed) and
supplies `salt_len + 40` bytes per attempt for every variant of both schemes.

```text
hawk ref          SHAKE256(prefix || seed40 || u8 j), j = 0..3   (upstream)
hawk ref-T/hot*   SHAKE256(prefix || "HAWK bench sampler worker v1" || seed40
                           || u8 logn || u8 w || u64le(start) || u64le(end))
                  ranges over the 2n coordinates
e8 ref/hot*       SHAKE256(prefix || "HAWK-E8-CM worker sampler v2" || seed40
                           || u8 logn || u8 w || u64le(start) || u64le(end)
                           || u64le(sigma_sign bits))
                  ranges over the n/4 blocks
```

Worker `w` of `T` covers `[N w / T, N (w + 1) / T)`.  `test_hawk_variants`
checks that the prefix the HAWK signer gives its sampler equals E8's
`e8_sampler_prefix_init()` for the same message, key and attempt, and
`test_e8_variants` / `test_hawk_variants` check every variant against these
documented streams for several `T`.  Sampler bytes per attempt (n = 256, 512,
1024; top-level `salt_len + 40` not included):

```text
hawk ref, ref-T   20n                 5120 / 10240 / 20480
hawk hot64        16n + 8 per tail    4096 / 8192 / 16384 + tails
hawk hot32         8n + 4 per tail    2048 / 4096 / 8192 + tails
e8 ref            22.5n (serial)      5760 / 11520 / 23040
e8 hot64          18n + 8 per tail    4608 / 9216 / 18432 + tails
e8 hot32           9n + 4 per tail    2304 / 4608 / 9216 + tails
```

### Integer-only runtime and generated tables

`e8/tables/gen_tables.py` (Python standard library, `decimal` at 160 digits
from exact rational exponents) generates every table of both schemes with
one discrete-Gaussian routine: E8 for `sigma_sign` = 3.592, 3.631, 3.669 (all
13 centre classes `k/4`, the component CDFs of all 256 taus, `base_z`,
`base_norm2`, `coord_class`, and the `ref` residue and slot tables), HAWK for
upstream's `sigma_sign` = 1.010, 1.278, 1.299 (both parities; the `ref` table
reproduces upstream's 78-bit table exactly, which the generator and
`test_hawk_variants` check).  Thresholds are exact floors of
`prefix / total * 2^B`; values within `10^-130` of an integer are exact
symmetries (for example `P(X > 0) = 1/2` at centre 1/2) and are taken as that
integer.  The rules are in the generator's docstring.  `make tables`
regenerates the headers and fails if the committed ones differ.

The signing path (`e8_sign.c`, `e8_api.c`, `e8_codec.c`, `e8_f2.c`,
`e8_ntt.c`, `e8_math.c`, `e8_sampler.c`, the variant TUs, `cmn_rng`,
`cmn_pool`, `cmn_timing`, and the HAWK variant signer and samplers) contains
no floating point: `make check-integer-only` compiles each of these
translation units with `-mgeneral-regs-only` (GCC rejects any floating-point
use) and scans the timed objects, plus the upstream objects the E8 signer
calls (`sha3.o`, `ng_hawk.o`), for SSE/AVX/x87 floating-point instructions.
The timed objects themselves are compiled with the ordinary flags, identical
for both schemes.  The norm bounds of the public API are integer constants
(8439, 16220, 31150; `e8_norm_bound_default()`), equal to
`e8_verify_bound_from_sigma()` with the default `sigma_verify`.

The former runtime `double` builder survives only for tests and the tools
that sweep `sigma_sign` (`e8_sampler_runtime.c`).  Its double weights resolve
about 53 bits, so the low bits of nearly every 64-bit threshold differ from
the exact generated ones (at most 5,840 units of 2^-64; `test_e8_tables`
counts them); hot values agree, sampling on either table set gives the same
outputs in the tests, and none of the fixed-seed vectors contains a tail
draw, so the integer tail left them unchanged.

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
                      calls per attempt, the API's bound streams, and the
                      HAWK prepared-key shim
test_e8_tables        generated against runtime-built hot64 tables, sampling
                      equivalence, integer norm bounds, 1D distribution
                      moments of every ref/hot64/hot32 table
test_e8_variants      every E8 variant: sign/verify, documented streams for
                      T = 1, 2, 4, 8, determinism, binding
test_e8_ref_ct        structural constant-time loop counts of E8 ref
test_hawk_variants    HAWK variants: generated = upstream 78-bit table,
                      variant signer = upstream signing, prefix = E8 prefix,
                      sign/verify, documented streams, binding, moments
test_cmn_common       shared modules: RNG known answer, buffering invariance,
                      prefixed streams and counters; pool coverage,
                      reductions, grow-only dispatch, resizing and
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
e8/bin/e8_sampler_bench --variant-matrix [--logn N] [--threads N] --trials 2000
```

`--variant-matrix` (also `make -C e8 variant-sampler-bench`, writing
`e8/e8_variant_sampler_bench.csv`) times every sampler variant of both
schemes, one full-dimension call per trial through the signers' own entry
points with bound streams (a fixed prefix of encoded-key length), at
`T = 1, 8, 16` by default, and reports medians, cycles per 8 coordinates
(`cycles / (n/4)` for both schemes) and random bytes per call.  The other
modes are the earlier E8-only configurations.

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
Every sampler variant of both schemes runs by default, with one thread and
with the selected per-logn thread count; `--variant NAME` (repeatable) and
`--threads LIST` (for example `1,8,16`, or `sel`) select blocks, and
`--list` prints the variants.
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

- The `hot64` and `hot32` samplers are data-dependent and not constant-time;
  the `ref` sampler's constant-time property rests on its structure (loop
  counts tested) and code review, not on a tool such as ctgrind.  The rest
  of the signing path is not hardened.
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
