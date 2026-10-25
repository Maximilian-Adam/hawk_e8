# Precision audit of the sampler variants

Step 5 of the fair-comparison plan: one tool and one set of measures give
the distance of every sampler variant of both schemes from its ideal
discrete Gaussian, per draw, per unit (HAWK coordinate, E8 block) and per
signature.  The audit reads the committed tables; it changes no table,
generator rule or sampler.

```sh
make precision-audit          # from the repository root or e8/ (about 2 minutes)
```

`e8/tools/precision_audit.py` (Python standard library) runs its self-tests,
audits every variant and writes, in `e8/`:

- `precision_audit.csv`: one row per distribution (scheme, variant, logn,
  kind `1d` / `component` / `block`, id = parity, class or tau);
- `precision_audit_dists.csv`: every implemented distribution exactly
  (numerator and `log2` of the power-of-two denominator) beside the ideal,
  for Step 6's goodness-of-fit tests;
- `precision_summary.csv`: one row per (scheme, variant, logn); the target
  fails if it differs from the committed copy `e8/docs/precision_summary.csv`
  (to update it after a table change, copy it there).

## Implemented distributions

Every implemented probability is an exact rational with a power-of-two
denominator, read from the committed headers in `e8/tables/`:

| table | implemented distribution |
|---|---|
| hot row (`hot64`, `hot32`, both schemes; `B` = 64, 32) | `P(v_i) = (q_i - q_{i-1}) / 2^B`, `q_{-1} = -1`; `P(tail) = (2^B - 1 - q_last) / 2^B`; `P(w_j) = P(tail) (r_j - r_{j-1}) / 2^B`, `r_{-1} = -1`; values outside the row: 0 |
| E8 hot component CDF | `P(c \| tau) = (cdf_c - cdf_{c-1}) / 2^B`, `cdf_{-1} = -1` |
| HAWK `ref`, `ref-T` | `P(r >= k + 1) = T[2k + p] / 2^78`, value `+-(2r + p)` with a uniform sign bit (0 reached by both signs) |
| E8 `ref` 1D | value `lo_value[res] + j` with `(T[j-1] - T[j]) / 2^78`, `T[-1] = 2^78`, `T = 0` past the table; class `k/4` uses residue `k mod 4` shifted by `floor(k/4)` |
| E8 `ref` component | slot `s` with `(S[s-1] - S[s]) / 2^78`, `S[-1] = 2^78`, `S[15] = 0`; component = field `s` of the tau's slot permutation |

HAWK `ref` and `ref-T` share one distribution per coordinate (only the
random-to-sample mapping differs); the summary gives `ref-T` the `ref` row.
The legacy runtime-built `hot64` tables (tests and tools only) are not
audited.

## Ideal distributions

The ideal distributions come from `e8/tables/gen_tables.py` (`DGauss`,
`hawk_dists()`, `e8_dists()`), with the same parameters and support rule,
evaluated at 220 decimal digits:

- HAWK, parity `p`: `x = 2y + p`, `y ~ D_Z(-p/2, sigma_sign)`, i.e.
  `D_{2Z+p, 2 sigma_sign}` of the spec, `sigma_sign` = 1.010, 1.278, 1.299;
- E8 1D, class `j`: `D_Z(num32_j / 32, sigma_sign / sqrt(32))`,
  `sigma_sign` = 3.592, 3.631, 3.669;
- E8 component given tau: mass of component `c` = product of the eight 1D
  normalisers of its classes, normalised over the 16 components;
- E8 block given tau: ideal component times the eight ideal 1D draws.  This
  is the exact coset Gaussian: (component, eight product coordinates) is a
  bijection onto the block, and `32 ||P base_z||^2 = sum num32^2` for every
  (tau, component) (checked by the self-test), so the block weight
  `exp(-||P z||^2 / (2 sigma_sign^2))` is the product of the eight 1D
  weights.  The implemented block factorises the same way.

**Support truncation.**  `DGauss` keeps the integers within `20 s + 32` of
the centre and renormalises.  The audit bounds the true mass outside each
truncation by
`sum_{side} exp(-d^2 / 2s^2) / (1 - exp(-d / s^2)) / Z_trunc`, `d` the
distance from the centre to the first excluded integer (`trunc_bound_log2`).
Over every distribution the bound is at most 2^-1488 (E8 blocks: 16 times
the largest 1D bound).  The true ideal differs from the truncated one by at
most that in SD, and `R_a(P || Q_true) = R_a(P || Q_trunc) / (1 - eps)` for
`P` inside the truncation, so the change in any `R_a - 1` is below
2^-1487.  Every reported non-zero value is at least 2^-147 (SD at least
2^-78.4), so the truncation is negligible next to all of them.  Differences
below `10^-200` are rounding of the 220-digit ideal and are reported as
exact zeros.  This happens only for the 16 taus per logn whose E8 `ref`
component distribution is uniform (each component exactly 1/16, which the
78-bit slot table represents exactly).

## Measures

All measures are computed for both schemes by the same code.  Every
distribution is reported in `precision_audit.csv`, with a `log2` form.

- **Statistical distance** `SD = 1/2 sum |P_impl - P_ideal|`, exact per
  draw (each 1D table, each component distribution).  For an E8 block given
  tau, the audit uses the bound
  `SD(component) + max_c sum_v SD(class(tau, c, v))`, not the exact product
  SD.  Per signature it uses the union bound `units * max SD(unit)`.
- **Renyi divergence, as hawk-spec.pdf defines and uses it** (HAWK v1.1,
  February 5, 2025; PDF page indices):
  - definition, section 2, eq. (5), PDF page 7:
    `R_a(P || Q) = (sum_{x in Supp(P)} P(x)^a / Q(x)^(a-1))^(1/(a-1))`
    for `Supp(P) ⊆ Supp(Q)`, `a in (1, inf)`; data processing,
    multiplicativity and probability preservation (PDF pages 7-8);
  - direction, section 6.5, PDF page 61: `P_1` is the table-based sampler,
    `P_2` the exact Gaussian, so the audit reports
    `R_a(P_impl || P_ideal)`.  `Supp(P_impl) ⊆ Supp(P_ideal)` holds for
    every distribution (self-test).  The reverse direction is infinite for
    every coordinate, block and signature, because every 1D table truncates
    the ideal support.  It is not reported further.  The E8 component draws
    have full support;
  - eq. (58), PDF page 61:
    `R_a(P_1 || P_2) <= (max_b R_a(D'_b || D_{2Z+b, 2 sigma_sign}))^(2n q_s)`;
  - eq. (57), PDF page 61: `P_2(E) >= P_1(E) / (R_a(P_1 || P_2) 2^(lambda / (a - 1)))`
    for `P_1(E) >= 2^-lambda`; the **loss factor** is
    `R_a(P_1 || P_2) 2^(lambda / (a - 1))`;
  - criterion, PDF page 61: per coefficient `R_513 < 1 + 2^-78` for
    HAWK-512 and HAWK-1024 "implies at most one bit of security being
    lost" by eq. (6);
  - condition, eq. (6) (from [Pre17, (4)]), PDF page 8: a `lambda`-bit
    secure search game sampling `Q` at most `q_s` times stays
    `(lambda - 1)`-bit secure with `P` if, for an order `a >= 2 lambda + 1`,
    `R_a(P || Q) <= 1 + 1/(4 q_s)`.  The audit evaluates it per signature at
    `a = 2 lambda + 1` (the smallest admissible order; `R_a` is
    non-decreasing in `a`);
  - parameters, Table 4, PDF page 16: `lambda` = 64, 128, 256 (bit
    security) and `q_s` = 2^32, 2^64, 2^64 for n = 256, 512, 1024.  These are
    also the aux script's values (`lam = 2^(logn - 2)`) and Theorem 4's
    (PDF page 62: `lambda in {128, 256}`, `q_s <= 2^64`).  The audit uses
    the same values for E8 at the same n.

  Reported orders: `a = 513` (the spec's), 4181 and 37286 (the spec's
  optimal orders, `precision_audit.csv`), `2 lambda + 1` (eq. (6)), infinity
  (`max P_impl / P_ideal`), and the integer order in `[2, 2^20]` that
  minimises the loss factor.  The search is a ternary search on integers,
  like the aux script's continuous one over the same interval.
- **Units and signatures.**  One signature (one accepted attempt) uses 2n
  HAWK coordinates, with parities given by `t`, or n/4 E8 blocks, with taus
  given by `t`.  RD is multiplicative over independent draws.  The block RD
  is exact:
  `sum_block P^a Q^(1-a) = sum_c P_c^a Q_c^(1-a) prod_v S_a(class(tau, c, v))`,
  with `S_a` the 1D sums.  **Worst case** (the summary's default, as in
  eq. (58)): every unit at the worst parity or tau, `R_sig = R_unit^units`.
  **Uniform target**: half of each parity, or every tau equally often
  (geometric mean of the 256 block divergences), `sig_rd513_m1_uniform_log2`.
  Per transcript: `R_sig^(q_s)`, the `R_a(P_1 || P_2)` of eq. (57).
- Also per draw: `max |P_impl / P_ideal - 1|` over the implemented support,
  the ideal mass outside it, and `P(tail)` of the hot tables.

## Reproduction of the spec's HAWK `ref` figures

| figure (hawk-spec.pdf, section 6.5, PDF page 61) | spec | audit |
|---|---|---|
| HAWK-512 per-coefficient `R_513 - 1`, worst parity | `< 2^-78` | `2^-79.19` |
| HAWK-1024 per-coefficient `R_513 - 1`, worst parity | `< 2^-78` | `2^-78.57` |
| HAWK-512 loss factor at `a = 37286` (`lambda = 128`, `q_s = 2^64`) | 1.07 | 1.0602 |
| HAWK-1024 loss factor at `a = 4181` (`lambda = 256`, `q_s = 2^64`) | 1.17 | 1.1674 |
| order minimising the loss factor, HAWK-512 / HAWK-1024 | 37286 / 4181 | 37286 / 4181 |

The audit reproduces the spec's figures.  The minimising orders are exactly
the spec's, which confirms `lambda` = 128 and 256 (`2^(logn - 2)`, as in
the aux script `generate_C_tables.py`).  The spec's 1.07 and 1.17 are the
audit's 1.0602 and 1.1674 rounded up to two decimals.  For HAWK-256 (not in
the spec's text) the per-coefficient `R_513 - 1` is `2^-78.30`, which
also meets the spec's criterion.  The aux script also checks
`R_{2 lambda + 1} < 1 + 2^-(3 + logn) / q_s` per coefficient (orders 129,
257 and 513).  This is the per-signature form of eq. (6), which the audit
reports as `cond6_ok`.

## Results

Values are `log2` (for example, -76.6 means 2^-76.6); `R` is `R_513`.
"draw" is the worst single draw (any 1D table or component distribution).
"sig" is one signature, worst target unless marked "unif.".  (6) is eq. (6)
at `a = 2 lambda + 1` per signature.  `a_opt` and "loss" are the order
minimising the loss factor `R_a(unit)^(units q_s) 2^(lambda / (a - 1))` and
that factor.  HAWK `ref-T` equals `ref`.  The full column set is in
`precision_summary.csv` (column definitions below).

| scheme | variant | B | n | draw SD | draw R−1 | draw R∞−1 | sig SD | sig R−1 | sig R−1 unif. | sig R∞−1 | (6) | a_opt | loss |
|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|:-:|--:|--:|
| hawk | ref | 78 | 256 | -76.6 | -78.3 | -15.5 | -67.6 | -69.3 | -70.0 | -6.5 | yes | 794959 | 1.0001 |
| hawk | hot64 | 64 | 256 | -63.6 | -2.1 | -1.5 | -54.6 | 154.0 | 77.0 | 225.7 | no | 73 | 1.9085 |
| hawk | hot32 | 32 | 256 | -31.8 | -4.5 | -3.3 | -22.8 | 31.9 | 16.0 | 72.9 | no | 32 | 18.8413 |
| e8 | ref | 78 | 256 | -76.4 | -3.7 | -2.4 | -67.3 | 55.0 | 23.9 | 131.5 | yes | 145 | 1.3795 |
| e8 | hot64 | 64 | 256 | -61.8 | -84.4 | -13.1 | -54.1 | -75.4 | -76.2 | -4.0 | yes | 218091 | 1.0002 |
| e8 | hot32 | 32 | 256 | -29.7 | -36.6 | -20.8 | -21.8 | -27.6 | -28.5 | -11.8 | no | 33 | 16.8477 |
| hawk | ref | 78 | 512 | -76.8 | -79.2 | -19.1 | -66.8 | -69.2 | -69.2 | -9.1 | yes | 37286 | 1.0602 |
| hawk | hot64 | 64 | 512 | -63.4 | -1.0 | -0.6 | -53.4 | 600.7 | 300.4 | 740.7 | no | 5 | 3.847e+22 |
| hawk | hot32 | 32 | 512 | -31.4 | -4.9 | -3.5 | -21.4 | 49.4 | 24.7 | 124.1 | no | 2 | 2.594e+44329723437 |
| e8 | ref | 78 | 512 | -76.2 | -23.7 | -3.6 | -66.2 | -14.7 | -15.7 | 119.7 | no | 55 | 8.3128 |
| e8 | hot64 | 64 | 512 | -62.0 | -84.2 | -11.5 | -53.0 | -74.2 | -74.9 | -1.3 | yes | 13036 | 1.0091 |
| e8 | hot32 | 32 | 512 | -29.7 | -1.6 | -1.2 | -20.7 | 419.2 | 182.1 | 526.5 | no | 2 | 5.411e+485083819 |
| hawk | ref | 78 | 1024 | -76.2 | -78.6 | -12.0 | -65.2 | -67.6 | -68.4 | -0.7 | yes | 4181 | 1.1674 |
| hawk | hot64 | 64 | 1024 | -63.9 | -67.3 | -7.2 | -52.9 | -56.3 | -57.1 | 19.7 | no | 99 | 2.237e+15 |
| hawk | hot32 | 32 | 1024 | -31.2 | -4.7 | -3.5 | -20.2 | 108.1 | 54.1 | 253.0 | no | 2 | 9.515e+42284837185 |
| e8 | ref | 78 | 1024 | -76.2 | -77.8 | -7.8 | -65.4 | -67.7 | -67.7 | 12.9 | yes | 826 | 1.4047 |
| e8 | hot64 | 64 | 1024 | -61.7 | -83.6 | -16.5 | -51.7 | -72.6 | -73.1 | -5.4 | yes | 93177 | 1.0059 |
| e8 | hot32 | 32 | 1024 | -29.8 | -37.8 | -6.1 | -19.9 | -26.8 | -27.6 | 44.1 | no | 2 | 1.350e+272507406 |

A positive `log2(R - 1)` means `R` itself is about that power of two.  A
loss factor above about 2 means the Renyi argument of eq. (57) gives no
useful bound over `q_s` signatures.

### Reading

**SD tracks the threshold width and is comparable between the schemes.**
At equal width the per-draw SD is within 2.2 bits between the schemes:
2^-76.2 to 2^-76.8 for both at 78 bits; 2^-63.4 to 2^-63.9 (HAWK) against
2^-61.7 to 2^-62.0 (E8) at 64 bits; 2^-31.2 to 2^-31.8 against 2^-29.7 to
2^-29.8 at 32 bits.  The per-signature SD bound is within 1.5 bits
(for example 2^-52.9 against 2^-51.7 at 64 bits, n = 1024).  E8's slightly
larger values come from its component CDFs (16 thresholds per draw, the
worst E8 draws by SD) and from 9n/4 draws per signature against HAWK's 2n.
HAWK's larger hot-table tail mass (`P(tail)` 0.2-2.7% against 3e-7 to 1.4e-5
for E8, reproducing Step 4) does not show in the SD: tail thresholds are
`B` bits relative to the tail.  Every SD is below `(entries + 1) 2^-B`
(maximum ratio 0.29).

**The order-513 Renyi divergence is decided by single extreme-tail
entries, not by the width.**  `P_impl^a / P_ideal^(a-1)` amplifies a
relative error at an entry of probability `p` by `(P/Q)^512`.  So one entry
of probability near 2^-70 with `P/Q = 1.36` dominates a distribution whose
SD is 2^-63.  These entries come from two documented generator rules, so no
table behaves differently from its documentation:

- **Hot tables, last tail entry.**  The tail is truncated after the first
  threshold equal to `2^B - 1`.  That entry takes the rest of the unit
  interval, i.e. its own mass plus everything beyond, rounded up to the
  `2^-B` grid of the tail.  When its own mass is about `2^-B P(tail)`, its
  probability is inflated by a factor of order 1:

  | table | value | `P_impl` | `P_ideal` | `P/Q` | `log2 P (P/Q)^512` |
  |---|--:|--:|--:|--:|--:|
  | HAWK hot64 n=256, parity 0 | -20 | 2^-71.61 | 2^-72.05 | 1.357 | 154.0 |
  | HAWK hot64 n=512, parity 1 | -25 | 2^-69.96 | 2^-70.69 | 1.651 | 300.4 |
  | HAWK hot32 n=256, parity 1 | -15 | 2^-40.97 | 2^-41.12 | 1.104 | 31.9 |
  | E8 hot32 n=512, class 1 (centre 1/2) | -5 | 2^-53.13 | 2^-53.65 | 1.428 | 210.1 |

  HAWK hot64 n=1024 happens to end at an entry with `P/Q = 1.0067`
  (`R - 1 = 2^-67.3` per coordinate), and every E8 hot64 class ends at an
  entry with `P/Q - 1 <= 2^-11.5`.  E8's narrower 1D distributions (`s` =
  0.64 against HAWK's 1.01-1.30 for `y`) make consecutive tail masses differ
  by large factors, so its truncation point rarely lands on an entry of mass
  about `2^-B P(tail)`.  This is a property of where the truncation lands,
  not of the lattice.  The forced maximum also gives the truncated values
  probability 0, which is what makes the reverse direction infinite.
- **E8 `ref`, mass below `lo` placed on `lo`.**  `lo` is the largest value
  with `floor(P(X < lo) 2^78) = 0`, and the table gives it
  `(2^78 - T[0]) / 2^78`.  That is its own mass plus the mass below it,
  rounded up to the 2^-78 grid.  For the residue-1/2 classes (classes 1, 3,
  4), `lo = -6` has
  `P_impl = 2^-76.00` against `P_ideal = 2^-76.26` (`P/Q = 1.195`, n = 256),
  `1.084` (n = 512) and `1.0044` (n = 1024), giving per-draw `R - 1` =
  2^-3.7, 2^-23.7 and 2^-77.8.  At the other end, `hi` carries
  `floor(P(X >= hi) 2^78) / 2^78`, the floored mass at or beyond it, as the
  extreme entry of HAWK's one-sided table does.  Floored ends err by less
  than one 2^-78 unit including the mass beyond, and HAWK `ref`'s largest
  `P/Q - 1` is 2^-12.0.

Consequences at equal width:

- **78 bits, `ref` against `ref`** (the headline pair).  The SDs are the
  same.  HAWK `ref` meets the spec's criteria at every n.  E8 `ref` meets
  eq. (6) at n = 256 and 1024 and the 2^-78 per-draw criterion only at
  n = 1024 (2^-77.8, against HAWK's 2^-78.6).  Its loss factors are 1.38,
  8.31 and 1.40, against HAWK's 1.0001, 1.06 and 1.17.  E8 `ref`'s
  component slot tables are not the cause: their `R - 1` is at most
  2^-142, and the uniform-mass taus are exact.  The cause is the `lo`
  placement of the residue-1/2 1D table.
- **64 bits, `hot64`.**  The SDs are comparable (HAWK 1.4-2.2 bits better
  per draw, at most 1.2 bits per signature).  By RD, E8 `hot64` meets every criterion (per-draw
  `R - 1` 2^-83.6 to 2^-84.4, loss 1.0002 to 1.009, better than HAWK
  `ref`).  HAWK `hot64` fails at every n because of its last tail entries.
- **32 bits, `hot32`.**  The SDs are comparable (HAWK 1.4-2.1 bits better
  per draw, at most 1.0 bit per signature).  E8 `hot32` fails at n = 512 on
  its class-1 last tail entry; at n = 256 and 1024 it is within 2^-36.6 and
  2^-37.8 per draw, against HAWK's 2^-4.5 and 2^-4.7.  Neither scheme meets the spec's RD criteria at any n.  The
  per-transcript loss is unbounded except at n = 256 (`q_s` = 2^32), where
  both are near 17-19.

So neither scheme's tables are materially worse by SD at the same width.
By the spec's RD, the generated tables' end-point rounding decides the
outcome, and it favours E8 at 64 bits and HAWK at 78 bits.
A generator rule that puts the floored mass at or beyond the extreme entry
on it, as HAWK's one-sided `ref` table does, instead of rounding the
remainder up, would remove the large outliers.  That is a table change,
outside this step.

### Other observations

- **Exact first-entry offset.**  With `target <= threshold` and
  `q_{-1} = -1`, the first entry of every hot CDF gets one extra unit
  (`q_0 + 1` of `2^B`).  For the E8 component CDFs of the uniform-mass taus
  (`q_0 = 2^B / 16` exactly) this is a relative error of exactly 2^-60
  (hot64) or 2^-28 (hot32), the `component` rows' `R∞ - 1`.
- **Zero-probability entries.**  HAWK `ref`'s parity-1 table ends with a 0
  threshold (`lo` entry `0x0`), so its last value has probability 0
  (`zero_entries` = 1).  No hot table and no E8 `ref` table has one.
- **Exact symmetries.**  E8 classes with the same centre residue have the
  same row up to the integer shift.  Residues 1/4 and 3/4 mirror each
  other, so classes 0, 2 and 10 (residue 0), classes 1, 3 and 4 (1/2), and
  classes 5-9, 11 and 12 (1/4, 3/4) have identical measures.  The 16
  equal-mass E8 `ref` components per logn are exact.

## Values for the revised paper §6.3

The submitted §6.3 bounded the E8 distance heuristically
(`eps_DG ≲ eps_tail + eps_arith + eps_cdf ≈ 2^-32` at n = 1024, from
`double` table construction) and used SD where HAWK uses RD (Review 1).
Both premises are gone: tables are generated offline in exact arithmetic
and the runtime is integer-only (Step 4), and both schemes are now measured
by the same tool under both measures.  The paper text is Step 11.  The
values to cite:

- truncation: true ideal mass outside the generator's support `<= 2^-1488`
  per distribution (n = 1024: E8 1D `s = 0.649`, radius 44.97);
- per-signature SD bound (union bound, worst target), n = 256 / 512 / 1024:
  HAWK `ref` 2^-67.6 / 2^-66.8 / 2^-65.2, E8 `ref` 2^-67.3 / 2^-66.2 /
  2^-65.4; HAWK `hot64` 2^-54.6 / 2^-53.4 / 2^-52.9, E8 `hot64` 2^-54.1 /
  2^-53.0 / 2^-51.7; HAWK `hot32` 2^-22.8 / 2^-21.4 / 2^-20.2, E8 `hot32`
  2^-21.8 / 2^-20.7 / 2^-19.9;
- per-draw `R_513 - 1` against the spec's 2^-78 and the loss factor of
  eq. (57) at the optimal order: the Results table above;
- the spec's HAWK figures reproduced (2^-79.19 and 2^-78.57; 1.0602 at
  `a = 37286`, 1.1674 at `a = 4181`).

## Files and columns

`precision_summary.csv` (one row per scheme, variant, logn; Step 7 joins it
onto its timing rows by `(scheme, variant, logn)`):

| column | definition |
|---|---|
| `scheme`, `variant`, `threshold_bits`, `technique`, `logn`, `n` | as in the timing CSVs |
| `unit`, `units_per_sig` | `coordinate` and 2n (HAWK), `block` and n/4 (E8) |
| `lambda`, `qs_log2` | spec Table 4 (PDF page 16) |
| `draw_sd_max_log2` | worst per-draw SD (any 1D table or component distribution) |
| `draw_rd513_m1_max_log2`, `draw_rdinf_m1_max_log2` | worst per-draw `log2(R_a - 1)`, `a` = 513 and infinity |
| `draw_max_rel_err` | worst `|P_impl / P_ideal - 1|` over the implemented support |
| `draw_ideal_out_mass_max_log2` | worst ideal mass outside the implemented support |
| `draw_p_tail_max` | worst hot-table tail probability (empty for `ref`) |
| `unit_sd_bound_log2`, `unit_rd513_m1_log2`, `unit_rdinf_m1_log2` | per coordinate (= per draw) or per block given tau, worst tau |
| `sig_sd_bound_log2` | `units * max SD(unit)` |
| `sig_rd513_m1_log2`, `sig_rd513_m1_uniform_log2`, `sig_rdinf_m1_log2` | per signature, worst and uniform target |
| `cond6_order`, `sig_cond6_m1_log2`, `cond6_ok` | eq. (6) at `a = 2 lambda + 1`: `R_a(sig) - 1 <= 1/(4 q_s)` |
| `loss_a513_log2` | `log2` of the loss factor at `a = 513` |
| `a_opt`, `loss_opt_log2`, `loss_opt` | order minimising the loss factor, and the factor |

`log2` values are printed to 3 decimals, `-inf` for an exact zero.

`precision_audit.csv` has the same measures per distribution:
`kind` = `1d` (id = HAWK parity or E8 class), `component` (id = tau) or
`block` (id = tau, E8 only), `rd<a>_m1_log2` for every `--orders` order,
`support`, `zero_entries`, `p_tail`, `sd`, `max_rel_err`,
`ideal_out_mass_log2` and `trunc_bound_log2`.

`precision_audit_dists.csv`: one row per (distribution, value) over the
union of both supports: `p_impl = p_impl_num / 2^p_impl_den_log2` exactly
(0 outside the implemented support), and `p_ideal` to 7 significant digits.
E8 `component` rows have value = component index; blocks are the product
(see above).

## Self-tests (`--self-test`, run by the target)

- SD and RD formulas on toy distributions: identical (SD 0, `R_a = 1`), a
  two-point example (`SD = 1/4`, `R_2 = 4/3`, `R_3 = sqrt(20/9)`,
  `R_inf = 2`), infinity on a support mismatch, and the block factorisation
  against brute force;
- the geometry header equals the generator's, and the block norm identity
  holds;
- every implemented distribution sums to exactly 1 and lies in the ideal
  support; every 1D table truncates it;
- HAWK `ref` from `hawk_tables_ref.h` equals upstream's tables in
  `src/hawk_sign.c`;
- the exact mean and variance of all 135 1D tables equal the moments that
  `test_e8_tables --moments` and `test_hawk_variants --moments` compute in
  C (the values their distribution checks use), to relative 1e-15;
- sanity (not proofs): every SD `<= (entries + 1) 2^-B`; hot `P(tail)` in
  [3.2e-7, 1.4e-5] (E8) and [2.0e-3, 2.7e-2] (HAWK).
