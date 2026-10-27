# Key generation and verification: data for the tables and the paper

What the measured key generation and verification tables (Tables 8-10)
and the paper's revised Sec. 8.4 rely on, beyond the timings themselves:
the E8 key generation limits, the verifier's prime counts, the
correctness evidence, the object sizes, and how the submission's figures
relate to these. The timings come from the bare-metal runs
(`bench_compare --mode keyverify`; accounting in
`perf_comparison_final.md`, "Key generation and verification"; how to
read the tables in `harness.md`, "Key generation and verification").

## Submission figures and what replaces them

The submission (Sec. 8.4) reported E8 key generation at 1.09-1.18x HAWK
and verification at 6.0-6.5x HAWK. Both came from a benchmark that is no
longer available, so they are replaced, not reproduced:

- **Verification**: that figure is the verifier now kept as E8 `ref`
  (`e8_verify_finish_ref()`): 4 NTT primes, twiddles and the NTT of
  `Delta` rebuilt on every call, one Fermat inversion per coefficient, a
  `double` bound. Timed against HAWK in the same harness, it is again
  6.0-6.6x HAWK (trial runs), so the old figure is accounted for. The
  fair figure is E8 `fast` (`e8_verify_finish()`), which uses HAWK's
  verifier techniques and the minimum sound number of primes (below).
  Table 9 shows both, with `ref` as context.
- **Key generation**: both schemes now run on paired seeds, with a
  `keygen_core` row for the shared NTRU core and E8's form computed with
  HAWK's technique. E8's own tail after the core is the public form, its
  limits and the 3-prime invertibility check. HAWK restarts the whole
  key generation when its compressed public-key encoding fails, and E8
  never does, so the ratio of means includes a key-format effect (the
  `diverged` column of Table 8).

## Verifier primes

| scheme | primes | why |
| --- | ---: | --- |
| HAWK | 2 | upstream's range argument (`src/hawk_vrfy.c`) |
| E8 `fast` | 3 | the minimum sound for the E8-CM form (below) |
| E8 `ref` | 4 | the submission's verifier; more than needed |

Bound on the half-trace `V = (n/2) ||w||^2_Q` for any int16 signature
and any key within the limits, against the products of the primes (proof
and derivation: `verifier_range.md`):

| n | V bound | 3 primes | 2 primes |
| ---: | ---: | ---: | ---: |
| 256 | 2^73.34 | 2^93.00 | 2^62.00 |
| 512 | 2^78.19 | 2^93.00 | 2^62.00 |
| 1024 | 2^84.10 | 2^93.00 | 2^62.00 |

Two primes cannot be sound for E8 at any n. Fitting 2^62 needs |w| at
most 1285 / 239 / 30, but honest signatures reach max |w0| = 2,010 /
5,945 / 15,646. Even HAWK-style limits on s0 and s1 one bit above the
honest maxima give V <= 2^62.18 / 2^69.59 / 2^77.00. The cause is the
`S_n` scaling of the E8-CM public form (paper Sec. 4.5): its
coefficients are 2^3 to 2^7 times HAWK's. The third prime is therefore
a cost of the lattice.

## E8 key generation limits

Maximum |coefficient| (bit length) of the public form, the limit in bits
(one bit above the maxima over 10^4 keys), and the keys rejected
(`tools/e8_keygen_limits.c`):

| n | q00[0] | q00[u>0] | q01 | q11[0] | q11[u>0] | rejected |
| ---: | --- | --- | --- | --- | --- | --- |
| 256 | 5,576 (13) / 14 | 2,088 (12) / 12 | 9,100 (14) / 15 | 174,704 (18) / 19 | 99,560 (17) / 18 | 0 of 10^5 |
| 512 | 20,296 (15) / 16 | 6,916 (13) / 14 | 35,336 (16) / 16 | 1,074,584 (21) / 21 | 441,020 (19) / 20 | 0 of 10^5 |
| 1024 | 77,168 (17) / 18 | 23,348 (15) / 16 | 129,684 (17) / 18 | 7,268,664 (23) / 24 | 2,721,892 (22) / 23 | 0 of 10^5 |

(maxima over 10^5 keys per degree; |x| < 2^limit for every
coefficient.) Every q00 of those keys is invertible modulo all 4 primes.
For comparison, HAWK's limits are 9/9/10 bits (q00), 11/12/14 (q01) and
13-17 (q11). Paper Table 3 (ten keys) agrees with the maxima.

## Correctness evidence

- E8 `fast` against E8 `ref` (`tools/e8_verify_diff.c`, run in
  `make validate` and `validate-quick`): per degree, 9,200 honest
  signatures and 165,600 candidates. These include 101,200 tampered or
  random ones (bit flips in the salt, s0, s1; random s0, s1 at every
  scale), honest signatures at their exact norm boundary, and bounds from
  sigma_verify +-1-3%. Both public-key forms were checked. Results: 0
  verdict mismatches, 0 norm mismatches, 0 tampered or random signatures
  accepted, and 552 of 552 malformed public keys rejected.
- All 2.1 * 10^7 signatures of the validation (every sampler variant of
  both schemes) verify under the encoded and decoded public key, and the
  E8 ones under both verifiers, with identical verdicts.
- The keyverify mode checks per key that tampered signatures (one bit of
  the salt, s1 and, for E8, s0) are rejected by every verifier in both
  accountings. 0 of 48,000 were accepted in a 1,000-key trial.

## Sizes (bytes)

From the size macros (`bench_compare --sizes`; Table 10). Empty: the
object does not exist for the scheme.

| object | HAWK 256 | E8 256 | HAWK 512 | E8 512 | HAWK 1024 | E8 1024 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `privkey` | 96 | 544 | 184 | 1,080 | 360 | 2,152 |
| `privkey_prepared` | 592 | 40,960 | 1,184 | 40,960 | 2,368 | 40,960 |
| `pubkey` | 450 | 2,048 | 1,024 | 4,096 | 2,440 | 8,192 |
| `pubkey_decoded` | 784 | 2,064 | 1,568 | 4,128 | 3,136 | 8,256 |
| `sig` | 249 | 1,038 | 555 | 2,072 | 1,221 | 4,136 |
| `sig_decoded` | 526 |  | 1,048 |  | 2,088 |  |
| `tmp_keygen` | 5,639 | 6,151 | 11,271 | 12,295 | 22,535 | 24,583 |
| `tmp_sign` | 1,543 | 40,960 | 3,079 | 40,960 | 6,151 | 40,960 |
| `tmp_verify` | 2,567 | 2,055 | 5,127 | 4,103 | 10,247 | 8,199 |
| `tmp_verify_fast` | 3,911 |  | 7,751 |  | 15,495 |  |

E8's functions also use stack buffers beyond `tmp` (not counted). HAWK's
signature carries only `s1` (compressed) and its verifier reconstructs
`s0`. E8's uncompressed signature carries `s0`, about 4 times larger, so
E8 does not reconstruct it.
