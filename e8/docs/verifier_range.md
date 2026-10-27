# Range argument of the HAWK-E8-CM verifier (3 primes)

The E8 verifier (`e8_verify_finish()`, `src/e8_vrfy.c`) decides
`||w||^2_Q <= bound` from residues of the half-trace modulo
`E8_VRFY_PRIMES` = 3 NTT primes, as HAWK's verifier does with 2 primes
(`src/hawk_vrfy.c`, comment before its two-prime loop). This note proves
that 3 primes are sound for every degree with the key generation limits of
`src/e8_api.c`. It also shows why 2 primes cannot be. The numbers come from `tools/e8_keygen_limits.c`
(`bin/e8_keygen_limits`) and the short computations quoted below.

## What the verifier computes

For a signature `(salt, s0, s1)` and `h = H(hm || salt)`, the verifier forms
`w = h - 2 s` (`w0 = h0 - 2 s0`, `w1 = h1 - 2 s1`) and needs

    ||w||^2_Q = [ adj(w0) q00 w0 + adj(w0) q01 w1
                  + adj(w1) q10 w0 + adj(w1) q11 w1 ]_0

(the constant coefficient; `n ||w||^2_Q = Tr(w^* Q w)` and
`Tr(a) = n a_0` in `R_n = Z[X]/(X^n + 1)`). By the completion of squares
(`q00 q11 - q01 q10 = Delta`, `d = w1 / q00`, `e = w0 + q01 d`), modulo a
prime `p` at which `q00` is invertible,

    V = (n/2) ||w||^2_Q = sum_{u < n/2} q00[u] e[u] e[n-1-u]
                                       + Delta[u] d[u] w1[n-1-u]   (mod p)

in the NTT domain. That is, the residue of the integer `V` modulo `p` (for
a key from `e8_keygen()`, whose form satisfies the identity above exactly).
The verifier accepts if and only if every residue is the same value `r`,
`(n/2) | r` and `r / (n/2) <= bound`.

**Soundness condition.** If `0 <= V < P = p0 p1 p2` for every input the
verifier evaluates, then "all residues equal `r`" means `V = r + kP` with
`V < P`, so `V = r`. The verdict is therefore exactly
`(n/2) | V and V/(n/2) <= bound`, the mathematical verdict. (`V >= 0`
because `Q = B^* S_n B` is positive definite for a key from
`e8_keygen()`.) Conversely, if some `V >= P` is possible, a large `w` can
alias to a small residue and be accepted.

The early reject after the first prime is sound for any `V`. It rejects
when `r0 = V mod p0 > (n/2) bound`. The accept condition needs
`V = r <= (n/2) bound`, which is below every prime (at most 2^23.93, see
below), so `V mod p0 = V`. A rejected `r0` therefore means the true value
is out of bounds.

## The bound on V

Let `|x|` be the maximum absolute coefficient. For polynomials `a`, `b`
and `q`,

    |[adj(a) q b]_0| = |sum_i a_i (q b)_i|
                     <= sum_{i, j} |a_i| |b_j| |q_{(i - j) mod n}|
                     <= n A B |q_0| + n (n - 1) A B L

with `A = |a|`, `B = |b|` and `L = max_{k != 0} |q_k|` (the `n` pairs with
`i = j` meet the constant term). With `|w0|, |w1| <= W` and
`q10 = adj(q01)`, which has the same coefficients up to sign,

    ||w||^2_Q <= W^2 [ n L00_0 + n (n-1) L00 + 2 n^2 L01
                       + n L11_0 + n (n-1) L11 ]

    V = (n/2) ||w||^2_Q.

- **W**: the verifier decodes any int16 `s0`, `s1`
  (`e8_decode_sig_uncompressed()`), so
  `w = h - 2 s` lies in `[-65534, 65537]` and `W = 2^16 + 1`. The argument
  needs no signature limit at all.
- **L**: the key generation limits. `e8_keygen()` discards a key with a
  coefficient outside them (as HAWK's `make_q001()`), and
  `e8_decode_public_key()` rejects a public key whose `q00` or `q01` is
  outside them. `q11` is not in the public key (as in HAWK), so its limit
  is enforced at key generation only. Every limit is strict:
  `|x| < 2^bits`, so `L = 2^bits - 1`.

## Measured maxima and the chosen limits

`bin/e8_keygen_limits --keys 10000` (and `--keys 100000`), keys from
`Hawk_keygen()` as `e8_keygen()` calls it, form from
`e8_compute_keygen_qform()`. The table gives the maximum |coefficient| (bit
length) with the limit in bits; `q00[0]` and `q11[0]` are listed apart, as
HAWK excludes them from its limits.

| n | keys | q00[0] | q00[u>0] | q01 | q11[0] | q11[u>0] |
|---:|---:|---|---|---|---|---|
| 256 | 10^4 | 5,496 (13) | 1,980 (11) | 8,504 (14) | 145,656 (18) | 67,224 (17) |
| 256 | 10^5 | 5,576 (13) | 2,088 (12) | 9,100 (14) | 174,704 (18) | 99,560 (17) |
| | limit (bits) | 14 | 12 | 15 | 19 | 18 |
| 512 | 10^4 | 19,728 (15) | 6,588 (13) | 31,416 (15) | 991,264 (20) | 392,608 (19) |
| 512 | 10^5 | 20,296 (15) | 6,916 (13) | 35,336 (16) | 1,074,584 (21) | 441,020 (19) |
| | limit (bits) | 16 | 14 | 16 | 21 | 20 |
| 1024 | 10^4 | 74,840 (17) | 22,056 (15) | 125,748 (17) | 6,957,912 (23) | 2,563,552 (22) |
| 1024 | 10^5 | 77,168 (17) | 23,348 (15) | 129,684 (17) | 7,268,664 (23) | 2,721,892 (22) |
| | limit (bits) | 18 | 16 | 18 | 24 | 23 |

The limits are one bit above the bit length of the maxima over 10^4 keys,
so at least twice every observed value. Over the 10^5 keys, the largest
q00[u>0] at n = 256 (2,088 < 4,096), q01 at n = 512 (35,336 < 65,536)
and q11[0] at n = 512 (1,074,584 < 2,097,152) reach the next bit length but
stay within the limits. **Rejected keys: 0 of 10^4 and 0 of 10^5 per
degree.** Every q00 of those keys is invertible modulo all 4 primes. Paper
Table 3 (ten keys: 13/13/17, 15/15/20, 17/17/23 bits for q00/q01/q11)
agrees.

## V per degree against the product of the primes

With the limits above and `W = 2^16 + 1` (computed exactly; the bracket is
`S = n L00_0 + n (n-1) L00 + 2 n^2 L01 + n L11_0 + n (n-1) L11`):

| n | S | V bound | V at the observed maxima | p0 p1 p2 | margin |
|---:|---:|---:|---:|---:|---:|
| 256 | 21,813,264,384 (2^34.34) | 1.199e22 (2^73.34) | 2^71.87 | 9.903e27 (2^93.00) | 2^19.66 |
| 512 | 314,093,600,768 (2^38.19) | 3.454e23 (2^78.19) | 2^76.99 | 2^93.00 | 2^14.81 |
| 1024 | 9,423,355,379,712 (2^43.10) | 2.072e25 (2^84.10) | 2^82.52 | 2^93.00 | 2^8.90 |

with `p0 p1 p2 = 2147473409 * 2147389441 * 2147387393 =
9902594767948415389369430017`. **The 3-prime condition `V < p0 p1 p2`
holds for every degree, for every int16 signature and every key within
the limits.** The early-reject threshold `(n/2) bound` = 1,080,192 /
4,152,320 / 15,948,800 (2^20.04 / 2^21.99 / 2^23.93) is below every prime
(all above 2^30), and `e8_verify_uncompressed_bound()` refuses bounds whose
threshold is not below 2^30.

The reference verifier (`e8_vrfy_ref.c`) uses all 4 primes (product
2^124). For keys within the limits, its verdict therefore equals the 3-prime
verdict on every input (both are the mathematical verdict). The
differential tests check this (`tools/e8_verify_diff.c`, `make test-e8`,
`validate-quick`, `validate`; docs/validation.md).

## Why 2 primes cannot work

`p0 p1 = 4611461723314874369 = 2^62.00` (HAWK's two primes). Three ways to
fit below it fail:

1. **No signature limit (as above).** V reaches 2^73-2^84, far above
   2^62. Fitting 2^62 with the key limits would need
   `|w| <= W` with W = 1285 / 239 / 30 for n = 256 / 512 / 1024. Honest
   signatures are far larger: max |w0| = 2,010 / 5,945 / 15,646 and max
   |w1| = 401 / 883 / 1,665 over 1,000 signatures per degree (10 keys x 100;
   rms |w0| 408 / 1,157 / 3,123).
2. **HAWK-style signature limits** (HAWK decodes s1 within `bits_lims1`
   bits and reconstructs s0 within `bits_lims0` bits). Give E8 separate
   limits on s0 and s1 one bit above the honest maxima (s0: 11 / 13 / 14
   bits, s1: 9 / 10 / 11 bits, so |w0| <= 4,097 / 16,385 / 32,769 and
   |w1| <= 1,025 / 2,049 / 4,097). Splitting the bound into its w0-w0,
   w0-w1 and w1-w1 parts gives V <= 2^62.18 / 2^69.59 / 2^77.00. That is
   above 2^62 for every degree; n = 256 misses by a fraction of a bit.
   Only limits at exactly the honest maxima (no margin) would fit at
   n = 256 (2^59.70), and they would reject honest signatures. Such limits
   are also a new decoding rule plus a signer-side rejection, which changes
   the scheme.
3. **A plain-norm pre-check**: `||w||^2 <= B2` bounds
   `||w||^2_Q` by `lambda_max(Q) B2`. That lands at about 2^61, depends on
   the key, and needs a new signer rejection, which again changes the
   scheme.

The cause is the `S_n` scaling of the E8-CM form (paper Sec. 4.5): its
coefficients are 2^3 to 2^7 times HAWK's for the same basis (HAWK's limits
are 9/9/10 bits for q00, 11/12/14 for q01 and 13-17 for q11). So E8 needs
a third prime where HAWK needs two. That third prime is a cost of the
lattice and stays in the comparison. The fourth prime of the reference
verifier was implementation. Wider (62-bit) primes would be a technique
open to both schemes, so neither gets them.

## Reproducing

```sh
cd e8
make bin/e8_keygen_limits
./bin/e8_keygen_limits --keys 10000 --jobs 16    # the table above (7 s on 16 cores)
./bin/e8_keygen_limits --keys 100000 --jobs 30   # 10^5 keys per degree (54 s)
make test-e8 validate-quick                      # differential tests
```
