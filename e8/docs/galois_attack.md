# The Galois key recovery attack on HAWK-E8-CM: small-degree experiments

`tools/galois_attack.py` (`make galois-attack`) checks the statements of
the paper's Sec. 5.4 (`sec:attack`) at small degree, from public keys, in
exact integer arithmetic (Python standard library only: no Sage, fpylll or
NumPy). The attack is Straznickas and Weis, "HAWK-n Key Recovery Reduces
to SVP in Dimension n/2+1" (2026), cited below as [SW]; the paper's bib
key is `hawkkeyrecovery`.

## What the paper proves

For `Q_E8 = B^* S_n B`, `B` in `SL2(R_n)`:

- `lem:cocycle`: since `tau(Y) = Y` (`Y = X^(n/4)`, `n/4` even),
  `tau(S_n) = S_n`, so `V_tau = B^-1 tau(B)` satisfies [SW]'s constraints
  (C1) and (C2) for `Q_E8`: it lies in the public cocycle lattice
  `Lambda(Q_E8)`, and `Y -> B Y tau(B)^-1` maps that lattice isometrically
  onto `Lambda(S_n)`, so the class is key independent.
- `prop:cocycleclass` (n >= 16):
  `Lambda(Q_E8) ~ sqrt(n/4) (Z + sqrt2 Z^(n/4-1) + L12^(n/16))` under
  `Q^tau = Tr_F det`, where `L12 = P sl2(O8) P^-1` under the Frobenius
  coefficient norm. `det L12 = 16`, so the rank, the determinant
  `(n/4)^n 2^(n/2-1)` and the minimum `n/4` are those of HAWK's cocycle
  lattice `sqrt(n/4) (Z^(n/2+1) + sqrt2 Z^(n/2-1))`. `min L12 >= 2` (an E8
  root-averaging argument), so `+-V_tau` is the unique shortest pair (HAWK
  has `n/2 + 1` pairs). By exact computation `L12 ~ D6 + D6`.
- `lem:descent`: the kernel lattice `M_(+-V_tau)` is `B^-1 O'^2` or
  `B^-1 (X O')^2` (`O' = Z[X^2]`), isometric under
  `T(x, x') = Tr(x^* Q_E8 x')` to `sqrt(n) (2E8)^(n/8)`; any det-1 basis
  `U` of `M_(V_tau)` gives `U^* Q_E8 U = A^* S_n A`, `A` in `SL2(O')`: a
  HAWK-E8-CM key of degree `n/2`.

Consequences: [SW]'s provable Thm 5.1 (SVP in dimension `n/2 + 1`) needs
the class `Z^p + sqrt2 Z^q` and does not apply as stated; [SW]'s
heuristic estimate (App. C) depends only on rank, determinant and target
length, so it is unchanged. The tau-tower step ([SW] Lemma D.4) needs no
reduction of the E8 descent lattice.

## What the script checks

Keys: `f, g` centred binomial (`eta = 2`), `F, G` from an exact
`NTRUSolve` with `q = 1`, i.e. `HawkKeyGen`'s structure (HAWK has no
parameter sets below `n = 256`). Per key, and the run fails if any
prediction fails:

| Check | Prediction | Paper |
|---|---|---|
| `L12` | integral, det 16, no norm-1 vector, 120 roots in two orthogonal rank-6 systems of 60 (so `D6 + D6`) | prop. (1)-(3) |
| `cocycle_rank`, `cocycle_det` | rank `n`, det of the Gram of `(4/n) Q^tau` equal to `2^(n/2-1)` | prop. (1) |
| `norm1_pairs`, `norm1_is_vtau` | exactly one pair of norm `n/4`, and it is `+-V_tau` | prop. (2) |
| `norm2_vectors`, `root_systems` | `8n - 2` vectors of norm `n/2`, root systems `(n/4 - 1) A1 + (n/8) D6`; with `+-V_tau` they have full rank and the product of their determinants equals the lattice's: the class certificate | prop. (3), eq. (cocycleclass) |
| `hawk_norm1_pairs` (`--hawk`, first key per n) | `n/2 + 1` pairs for the HAWK form `B^* B` of the same basis | [SW] Prop. 4.5 |
| `descent_det`, `descent_root_systems` | det 1 for `T / (4n)`; at `n <= 32`, `n/8` orthogonal E8 root systems (240 roots, rank 8) | lem. descent (2) |
| `recovered` | the tau-tower returns `B'` in `GL2(R_n)` with `B'^* S_n B' = Q_E8` | Sec. 5.4 |

`lll_first_is_vtau` (LLL, `delta = 0.99`, puts `+-V_tau` first) and
`recovered_equals_B` (whether `B' = +-B`) are reported, not checked.

The cocycle lattice is the integer kernel of (C2) in the unknowns `a`
and the odd coefficients of `b, c` (with `d = tau(a)`, (C1) holds by
construction). The tower: from the parity-test survivor `V`, the
generators `(I + V)/2`, `(I - V)/(2X)` of `M_V` over `O'`; a Bezout step
`1 = sum_j c_j mu(x, g_j)` over `O'`, solved as an integer linear system,
gives `y` with `det[x y] = 1`; the basis is then size reduced under the
`Q_E8` form (without that the next public form grows and the next level
slows down by orders of magnitude). The parity test cannot tell `V_tau`
from `-V_tau`: for `-V_tau` the kernel is `B^-1 (X O')^2` and
`(X^-1 x, X y)` is a det-1 basis of `B^-1 O'^2`, so both signs are tried
(the wrong one fails one level down). The recursion ends at degree 4,
where `(O8^2, A^* S A) ~ 2E8` is solved from its 240 minimal vectors.

## Results (2026-11-22, 32-core WSL machine, one core per run)

`make galois-attack` (n = 8, 16, 32, five keys each, seeds 1-5,
`--hawk`): every prediction holds, 12 s in total (recovery times
below; the rest is the certificates).

| n | keys | cocycle det (scaled) | norm-1 pairs | norm-2 vectors | root systems | HAWK norm-1 pairs | descent | LLL first = `+-V_tau` | recovery |
|---|---|---|---|---|---|---|---|---|---|
| 8 | 5 | 8 = 2^3 | 1 | 62 | A1 + D6 | 5 | (not run, n < 16) | 4 / 5 | 5 / 5, < 0.1 s |
| 16 | 5 | 128 = 2^7 | 1 | 126 | 3 A1 + 2 D6 | 9 | det 1, 2 E8 | 5 / 5 | 5 / 5, < 0.1 s |
| 32 | 5 | 32768 = 2^15 | 1 | 254 | 7 A1 + 4 D6 | 17 | det 1, 4 E8 | 3 / 5 | 5 / 5, 0.5-0.7 s |
| 64 | 2 | 2147483648 = 2^31 | 1 | 510 | 15 A1 + 8 D6 | 33 | det 1 | 2 / 2 | 2 / 2, 72-78 s |

The n = 64 row is `GALOIS_ARGS="--n 64 --keys 2 --hawk"` (seeds 1-2,
8 minutes in total, most of it the certificates; recovery is dominated by
the top-level cocycle lattice, about 70 s).

`L12`: det 16, 120 roots, two systems of 60 roots of rank 6, no norm-1
vector.

The experiments test the structure, not the cost: HAWK's parameter sets
(n = 256 and up) are far beyond pure-Python exact arithmetic, and an
`n = 128` run did not finish its integer kernel in half an hour.
