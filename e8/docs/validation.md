# Correctness and distribution validation of the sampler variants

For the fair comparison: every sampler variant of both schemes
signs correctly, and samples exactly the distribution its tables define.
One method, one set of tools and the same sample sizes per draw for HAWK
and HAWK-E8-CM.

```sh
make validate-quick   # small counts, about 1-2 minutes on 32 CPUs
make validate         # the full run below, about 30 minutes on 32 CPUs
```

Both targets (from the repository root or `e8/`) run `e8/tools/validate.py`,
which prints its expected duration, runs every part as single-threaded
processes in parallel (up to `os.cpu_count()` CPU slots; a 4-thread shard
takes 4), and writes, in `e8/results/validation/` (`make validate`) or
`e8/results/validation-quick/` (`make validate-quick`), both gitignored:

| file | contents |
|---|---|
| `correctness.csv` | one row per correctness shard and build (counts, attempts, signature digest, seconds) |
| `hist.csv` | the merged sampler histograms (kind, scheme, variant, logn, id, value, count) |
| `tests.csv` | every distribution test: statistic, df, cells, p, Holm-adjusted p |
| `controls.csv` | every negative control, with its noncentrality and eligibility |
| `ideal.csv` | the same samples against the ideal Gaussians (information only) |
| `signorm.csv` | signature norms: sample and exact mean and variance, z-scores, continuous approximation |
| `verifier.csv` | one row per verifier differential shard (candidates, accepts, mismatches, tampered and random signatures, bad keys) |
| `summary.csv` | one row per (scheme, variant, logn), no timings |

`make validate` fails unless every signature verified, every debug check
and digest matched, every distribution test passed and every eligible
control was rejected, and also if the fresh
`e8/results/validation/summary.csv` differs from the committed
`e8/docs/validation_summary.csv` (seeds are fixed, so the summary is
reproducible; to update it, copy it there).

## What the tests target

The references are the **exact implemented** distributions of the
committed tables (`precision_audit.load()`, Fractions with power-of-two
denominators; the generator's rounding rule is HAWK's, see
`precision_audit.md`), not the ideal Gaussians.  Their distance from the
ideal is the precision audit's result (`precision_audit.md`): at most 2^-29 in statistical distance per draw
(32-bit tables), far below what any feasible sample resolves (a test needs
on the order of `1/SD^2` draws to see a difference).  So the tests detect
implementation errors (a wrong table, comparison, value mapping, class or
component mapping, or stream layout), not the table approximation; the
`ideal` file confirms that the samples cannot tell the implemented and the
ideal distributions apart.

## Method

### Signature correctness (plan item 1)

`e8/tools/e8_correctness.c`, one process per shard:

- configurations: HAWK `ref` (upstream `hawk_sign_finish()` and
  `hawk_shim_sign_finish_decoded()`), `ref-T`, `hot64`, `hot32` (the
  variant signer, `hawkv_sign_finish()` / `hawkv_sign_finish_decoded()`
  after `hawkv_select()`), and E8 `ref`, `hot64`, `hot32`
  (`e8_sign_finish_inner()` with the variant), at n = 256, 512, 1024: 21
  configurations;
- real keys (`hawk_keygen()`, `e8_keygen()`); 1000 keys per configuration
  (shards of 10 keys, disjoint key indices) and 1000 signatures per key,
  i.e. 10^6 signatures per configuration and 2.1 * 10^7 in all;
- key modes alternate: even signatures use the encoded private key
  (all_in path), odd ones the decoded / prepared key;
- every signature is verified (`hawk_verify_finish()`,
  `e8_verify_finish()`) with the encoded and the decoded public key and,
  for E8, also with the reference verifier `e8_verify_finish_ref()`; all
  verdicts must agree (`verifier_mismatches` in `correctness.csv`, 0
  required). The first signature of each key is also
  verified against another message, which every verifier must reject (a
  control that the verifier can fail);
- attempts are counted from the signing RNG (two calls per attempt for
  both schemes) and, for E8, checked against the signer's own count;
- threads: every tenth shard of a variant with a split sampler (HAWK
  `ref-T`, `hot64`, `hot32`; every E8 variant) runs with T = 4 on the
  shared worker pool, i.e. 10% of those variants' signatures (an earlier
  pool race could only be exposed by threaded calls); upstream HAWK `ref`
  is serial;
- deterministic: keys, messages and signing RNGs are derived from the seed
  (documented in the tool's header); keys depend only on (seed, scheme,
  logn, key index), so the variants of a scheme sign with the same keys.

### Builds

- **No-debug build** (`bin/e8_correctness_nodebug`): the E8 objects
  compiled with `-DHAWK_E8_DEBUG_CHECKS=0`, the setting of the timed builds
  (`e8/README.md`, "For timing"), in their own directory `build/nodebug/`;
  HAWK, the HAWK variants and the shared modules do not use the flag and
  are the default objects.  All 21 configurations' 10^6 signatures come
  from this build.
- **Debug build** (`bin/e8_correctness`, default flags,
  `HAWK_E8_DEBUG_CHECKS=1`; plan item 3): every E8 shard of the no-debug
  run again, with the same seed, shard and thread count, so the same 10^6
  signatures per E8 configuration.  The debug build checks every sampled
  block (`e8_ca_debug_check_block()` in `e8_sampler_hot.c` and
  `e8_sampler_ref.c`: reconstruct x from the component offset and the
  product coordinates, solve back to z, compare z, the squared norm and the
  RM(1,3) syndrome, i.e. the coset, parity and block-norm checks), the
  prepared basis digests, and the reconstruction `w = B^-1 z` against the
  schoolbook computation for the first four attempts of every signing call
  (`e8_sign.c`); a failed check fails the signature (or asserts).  The
  debug run must have no failure, and every shard's signature digest must
  equal the no-debug shard's, so the timed build's signatures are exactly
  the ones the debug checks passed.  The sampling tool (below) is also a
  debug build, so the block checks also ran on every sampled block.

### Verifier differential

`e8/tools/e8_verify_diff.c`, 46 shards of 4 keys x 50 signatures per
degree (the same in `validate-quick` and `validate`; a small fixed run is
part of `make test-e8`). For every candidate, the new verifier
(`e8_verify_finish()`, 3 primes; encoded and decoded public key) and the
reference verifier (`e8_verify_finish_ref()`, 4 primes) must agree, and so
must their Q-norms (`e8_qnorm_completion()` against
`e8_qnorm_completion_ref()`). Per signature (honest, ref sampler) the
candidates are:

- the signature itself (must be accepted);
- the signature at the integer bounds N - 1 and N, where N is its Q-norm
  (must reject, then accept);
- the signature at the bounds of sigma_verify scaled by 0.97, 0.99, 1.01
  and 1.03;
- 9 one-bit flips (3 each in the salt, s0 and s1);
- 2 random signatures (s0, s1 uniform in [-2^b, 2^b), b uniform in
  0..15).

That makes 18 candidates per signature and about 10^5 tampered or random
ones per degree. Per key, three bad public keys (a q00 or q01 coefficient
just outside the key generation limits, a q00 that is not auto-adjoint)
must be rejected by `e8_decode_public_key()` and by `e8_verify_finish()`.
The range argument the verifier relies on is in `verifier_range.md`.

### Sampling (plan item 2)

`e8/tools/e8_distribution_sample.c`, histograms per job, every job on its
own stream (`cmn_rng`, domain `e8_distribution_sample v1`, labelled with
the job; seed 1):

- **1D tables**: 10^8 draws per table, every variant of both schemes:
  HAWK per parity (24 tables; `ref` through upstream `sig_gauss()` with all
  parity bits equal, the others through the variant's range sampler), E8
  per centre class (117 tables; `e8_hot*_sample_1d()`, and for `ref` the
  new hook `e8_ref_sample_1d_seq()`, which reads consecutive draws from one
  draw reader as a block stream does);
- **E8 blocks**: 10^6 blocks per tau, all 256 taus, every variant and n,
  consecutive blocks from one stream as one sampler worker reads them
  (`e8_hot*_sample_block()` with the trace; for `ref` the new hook
  `e8_ref_sample_blocks()`, the existing static `e8_ref_sample_block()`
  over one draw reader): component counts per tau, the squared norm
  `||P z||^2` per tau, and every product coordinate counted by its centre
  class;
- **signature norms**: 10^6 full-dimension samples per configuration (21)
  for one fixed random target per scheme and n, through the variant's
  sampler exactly as the signer calls it, serial (HAWK: the variant hook,
  i.e. upstream `sig_gauss()` for `ref`; E8: the variant's `sample_z()` in
  the bound per-worker mode).

The new E8 `ref` hooks only expose existing computation; the signing path
is unchanged (`make check-integer-only` and `test_e8_ref_ct` pass).

### Tests

`e8/tools/distribution_tests.py` (standard library; `python3 -I`):

- **references**: HAWK per parity (`ref-T` against `ref`'s table: same
  distribution, different mapping); E8 per class and per tau (component);
  E8 block norm per tau: the exact distribution of
  `sum_v (32 x_v - num32_v)^2 / 32 = sum_v 2 (4 x_v - k_v)^2` (centre
  `k_v / 4` of coordinate v's class), mixed over the components, by exact
  integer convolution; product coordinates by class against the class's
  table;
- **pooling**: values in increasing order (components by index) form
  adjacent cells, each closed once its expected count reaches 5, a
  trailing remainder joining the last cell; an observed value outside the
  reference support is an impossible outcome and fails the test;
- **statistics**: Pearson chi-square and the G-test on the pooled cells,
  `df = cells - 1`, p-values from the regularised incomplete gamma function
  (series and continued fraction, `math.lgamma`); signature norms: z-tests
  of the sample mean and variance against the exact implemented mean and
  variance (sums of the per-coordinate or per-block exact cumulants for
  the target's parities or taus);
- **multiple testing**: Holm-Bonferroni over the whole family (every
  chi-square, G and z test of both schemes) at family-wise level
  alpha = 10^-3;
- **negative controls** (their own Holm family, same alpha): the same
  samples against a wrong reference: the same table, tau or target at the
  neighbouring n (256 -> 512, 512 -> 1024, 1024 -> 512); and, for 1D
  tables and components, the reference with mass
  `m = 12 / sqrt(N (1/p_1 + 1/p_2))` moved from the most probable outcome to
  the second (a moved threshold; noncentrality about 144).  A control is
  eligible when its noncentrality (`N sum (p - q)^2 / q` over its cells; for
  signature norms the squared z of the true mean) is at least 100, and
  every eligible control must be rejected.  A permutation of the E8
  components is not used: at these `sigma_sign` the component masses are
  within 1/16 +- 3.5 * 10^-4, so a permutation changes the distribution too
  little to be seen; the component-to-coset mapping is established by
  verification and by the debug build's coset and parity checks.
- **ideal** (information): 1D, component and product-coordinate tests
  against the ideal Gaussians, same rules.

The same statistics, pooling rule, correction, controls and per-draw sample
sizes apply to both schemes.  E8 has more tests because it has more
distributions (13 classes, 256 component draws and 256 block norms per n).

## Results (full run, seed 1)

Run on the development machine (Ryzen 9 9950X3D, 32 logical CPUs, WSL),
`make validate`: correctness and debug runs 1184 s, sampling 132 s,
tests 65 s, 1381 s in all.  `make validate-quick` takes about 70 s.
Seeds: 1 for the correctness shards (keys, messages, signing RNGs) and for
every sampling job.  Builds: the signature counts, failures and attempts
below come from the no-debug build; the debug columns from the default
build; the histograms from the default (debug-check) build of
`e8_distribution_sample`.

**Overall.**  2.1 * 10^7 signatures (21 configurations x 10^6; 1.8 * 10^6
of them with 4 threads), zero signing failures, zero verification failures,
every control message rejected, E8 attempt counts equal to the signers'
own.  Debug build: 9 * 10^6 E8 signatures (every E8 shard again), zero
failures, every signature digest equal to the no-debug run's.  9774
distribution tests (282 on the 141 1D tables, 4608 on the 2304 component
distributions, 4608 on the 2304 block-norm distributions, 234 on the 117
product-coordinate classes, 42 signature-norm z-tests), none
rejected after Holm at 10^-3; smallest p 2.94 * 10^-4 (G-test, E8 `ref`
n = 512 components of tau 62), smallest Holm-adjusted p 1.  No impossible
outcome.  10035 negative controls, all eligible, all rejected.  Against the
ideal Gaussians (information): 0 of 5124 tests rejected (smallest p
2.94 * 10^-4, the same test), as the precision audit's distances predict.

| scheme | variant | n | signatures | threaded | sign / verify failures | attempts / sig | max attempts | debug signatures (failures, digest mismatches) | tests | rejected | smallest p | smallest Holm p | controls (eligible, rejected) |
|---|---|--:|--:|--:|:-:|--:|--:|--:|--:|--:|--:|--:|--:|
| hawk | ref | 256 | 1000000 | 0 | 0 / 0 | 1.1796 | 8 | — | 6 | 0 | 0.2063 | 1 | 9 (9, 9) |
| hawk | ref | 512 | 1000000 | 0 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.1808 | 1 | 9 (9, 9) |
| hawk | ref | 1024 | 1000000 | 0 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.2156 | 1 | 9 (9, 9) |
| hawk | ref-T | 256 | 1000000 | 100000 | 0 / 0 | 1.1795 | 8 | — | 6 | 0 | 0.05126 | 1 | 9 (9, 9) |
| hawk | ref-T | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.05536 | 1 | 9 (9, 9) |
| hawk | ref-T | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.2905 | 1 | 9 (9, 9) |
| hawk | hot64 | 256 | 1000000 | 100000 | 0 / 0 | 1.1792 | 8 | — | 6 | 0 | 0.2789 | 1 | 9 (9, 9) |
| hawk | hot64 | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.09332 | 1 | 9 (9, 9) |
| hawk | hot64 | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.2913 | 1 | 9 (9, 9) |
| hawk | hot32 | 256 | 1000000 | 100000 | 0 / 0 | 1.1792 | 8 | — | 6 | 0 | 0.1535 | 1 | 9 (9, 9) |
| hawk | hot32 | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.01201 | 1 | 9 (9, 9) |
| hawk | hot32 | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.3923 | 1 | 9 (9, 9) |
| e8 | ref | 256 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.0005517 | 1 | 1103 (1103, 1103) |
| e8 | ref | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.0002936 | 1 | 1103 (1103, 1103) |
| e8 | ref | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.0006082 | 1 | 1103 (1103, 1103) |
| e8 | hot64 | 256 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.002161 | 1 | 1103 (1103, 1103) |
| e8 | hot64 | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.003904 | 1 | 1103 (1103, 1103) |
| e8 | hot64 | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.001865 | 1 | 1103 (1103, 1103) |
| e8 | hot32 | 256 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.0007556 | 1 | 1103 (1103, 1103) |
| e8 | hot32 | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.002857 | 1 | 1103 (1103, 1103) |
| e8 | hot32 | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.006099 | 1 | 1103 (1103, 1103) |

E8 has 1078 tests and 1103 controls per configuration (13 classes, 256
component and 256 block-norm distributions, 13 product-coordinate
classes, the signature norm); HAWK 6 and 9 (two parities, the signature
norm).  The committed copy is `e8/docs/validation_summary.csv` (also the
controls' and ideal tests' counts and the signature-norm figures).

**Sample sizes and cells.**  1D tables: 10^8 draws each, 7-8 pooled cells
(E8) and 11-15 (HAWK); components: 10^6 blocks per tau, 16 cells; block
norms: 10^6 per tau, 17-32 cells; product coordinates: all 2.05 * 10^9
coordinates of the 2.56 * 10^8 blocks per variant and n, by class (2.0 *
10^6 to 3.6 * 10^8 per class, as the classes occur), 6-8 cells; signature
norms: 10^6 samples per configuration.

**Signature norms** (one fixed target per scheme and n, 10^6 samples):

SN| scheme | variant | n | signatures | threaded | sign / verify failures | attempts / sig | max attempts | debug signatures (failures, digest mismatches) | tests | rejected | smallest p | smallest Holm p | controls (eligible, rejected) |
|---|---|--:|--:|--:|:-:|--:|--:|--:|--:|--:|--:|--:|--:|
| hawk | ref | 256 | 1000000 | 0 | 0 / 0 | 1.1796 | 8 | — | 6 | 0 | 0.2063 | 1 | 9 (9, 9) |
| hawk | ref | 512 | 1000000 | 0 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.1808 | 1 | 9 (9, 9) |
| hawk | ref | 1024 | 1000000 | 0 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.2156 | 1 | 9 (9, 9) |
| hawk | ref-T | 256 | 1000000 | 100000 | 0 / 0 | 1.1795 | 8 | — | 6 | 0 | 0.05126 | 1 | 9 (9, 9) |
| hawk | ref-T | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.05536 | 1 | 9 (9, 9) |
| hawk | ref-T | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.2905 | 1 | 9 (9, 9) |
| hawk | hot64 | 256 | 1000000 | 100000 | 0 / 0 | 1.1792 | 8 | — | 6 | 0 | 0.2789 | 1 | 9 (9, 9) |
| hawk | hot64 | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.09332 | 1 | 9 (9, 9) |
| hawk | hot64 | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.2913 | 1 | 9 (9, 9) |
| hawk | hot32 | 256 | 1000000 | 100000 | 0 / 0 | 1.1792 | 8 | — | 6 | 0 | 0.1535 | 1 | 9 (9, 9) |
| hawk | hot32 | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.01201 | 1 | 9 (9, 9) |
| hawk | hot32 | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | — | 6 | 0 | 0.3923 | 1 | 9 (9, 9) |
| e8 | ref | 256 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.0005517 | 1 | 1103 (1103, 1103) |
| e8 | ref | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.0002936 | 1 | 1103 (1103, 1103) |
| e8 | ref | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.0006082 | 1 | 1103 (1103, 1103) |
| e8 | hot64 | 256 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.002161 | 1 | 1103 (1103, 1103) |
| e8 | hot64 | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.003904 | 1 | 1103 (1103, 1103) |
| e8 | hot64 | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.001865 | 1 | 1103 (1103, 1103) |
| e8 | hot32 | 256 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.0007556 | 1 | 1103 (1103, 1103) |
| e8 | hot32 | 512 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.002857 | 1 | 1103 (1103, 1103) |
| e8 | hot32 | 1024 | 1000000 | 100000 | 0 / 0 | 1.0000 | 2 | 1000000 (0, 0) | 1078 | 0 | 0.006099 | 1 | 1103 (1103, 1103) |

The exact implemented mean and variance agree with the continuous
approximation (HAWK `E||x||^2 = 2n (2 sigma_sign)^2`, variance
`2n * 32 sigma_sign^4`; E8 `E||P z||^2 = 2n sigma_sign^2`, variance
`4n sigma_sign^4`) to the printed digits: at these widths the discrete
Gaussians are far above smoothing.  For the same reason the E8 component
masses are within 1/16 +- 3.5 * 10^-4.

**Negative controls.**

| control | kind | scheme | count | smallest noncentrality | rejected |
|---|---|---|--:|--:|--:|
| neighbouring n | 1D | HAWK | 48 | 5.1 * 10^4 | 48 |
| neighbouring n | 1D | E8 | 234 | 2.0 * 10^4 | 234 |
| neighbouring n | product coordinates | E8 | 234 | 425 | 234 |
| neighbouring n | block norm | E8 | 4608 | 1700 | 4608 |
| neighbouring n | signature norm | HAWK | 12 | 5.6 * 10^5 | 12 |
| neighbouring n | signature norm | E8 | 9 | 1.2 * 10^5 | 9 |
| moved threshold | 1D | HAWK | 48 | 144 | 48 |
| moved threshold | 1D | E8 | 234 | 144 | 234 |
| moved threshold | component | E8 | 4608 | 144 | 4608 |

The moved thresholds are a relative change of the most probable outcome's
mass of at most 1.6 * 10^-3 (1D, 10^8 draws) and 3.4 * 10^-2 (components,
10^6 draws), so the 1D tests resolve threshold errors of about 2^-9 to
2^-11 of the mode's mass.

**Mutation check (one-off, not part of the targets).**  To confirm that
the tests catch real implementation errors and not only wrong references,
five single-line bugs were planted, one at a time, in a scratch copy of
`e8/` and run at n = 1024 (1D tables 10^7 draws, blocks 10^4 per tau,
signature norms 10^4, 200 signatures):

| mutant | distribution tests | signatures |
|---|---|---|
| E8 `hot64`: the hot-table scan skips its last value | 46 of 1078 rejected (1D, product coordinates) | all verify |
| E8 `ref`: the 78-bit comparison drops the low-part borrow | 29 rejected (1D, product coordinates, signature norm) | all verify |
| E8 `hot64`: coordinate v sampled with coordinate v+1's class | blocks fail the debug block check | all 200 fail verification |
| HAWK `hot64`: the tail returns the previous tail value | 6 of 6 rejected (1D, signature norm) | all verify |
| HAWK `hot64`: parity bit read from the next coordinate | none (1D tables use one parity; norms smooth) | all 200 fail verification |

Three of the five bugs leave every signature valid and are caught only by
the distribution tests; two are caught only by verification or the debug
checks.  Both parts are needed.

**A tool error found and fixed during the run.**  The first full run
rejected all 42 signature-norm z-tests, with the same z for every variant
of both schemes at each n (-45537 at n = 256).  Every signature-norm part
(10 parts of 10^5 samples) had written the target's parity / tau counts, so
the merged reference mean was 10 times the true one: `-9 mu / sqrt(10
kappa_2 / N)` is -45537 for both schemes, since `mu^2 / kappa_2 = 256` at
n = 256.  The target rows are now written by part 0 only, and the
analysis checks that a target has exactly 2n coordinates (HAWK) or n/4
blocks (E8).  No sampler or table was changed; the results above are from
a complete rerun with the fixed tool, and the first run's other 9732 tests
and its signatures had passed as well.

## Findings for the timing runs

- Every variant of both schemes, serial and threaded, signs correctly with
  both key forms; none is unusable.
- Attempts per signature (no-debug run, 10^6 signatures): HAWK 1.1792 to
  1.1796 at n = 256 (every variant; at most 8 attempts), 1.000001 to
  1.000007 at n = 512 and 1024 (at most 2); E8 1.000008 to 1.000039 at
  every n (at most 2).  The harness's check of HAWK n = 256 at about 1.17: the
  measured value is 1.1796 +- 0.0005 (one standard error), with the same
  keys for every HAWK variant.
- The debug build gives byte-identical signatures, so it may be used for
  correctness runs; timings must use the no-debug build.
- The validation reruns in about 23 minutes here; rerun it (and update the
  committed summary) after any change to a sampler, a table or the signer.
