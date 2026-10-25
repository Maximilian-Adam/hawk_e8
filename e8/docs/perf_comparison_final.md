# HAWK and Experimental E8 Signing Comparison

## Accounting

Both schemes are timed through their own `sign_finish` call, one call per
signature, under two accounting modes that are applied identically to both.
No separately measured cost is added to or subtracted from a timed call, and
nothing is amortised.

| scheme | accounting | timed call | setup, timed separately and never added |
| --- | --- | --- | --- |
| HAWK | `all_in` | `hawk_sign_finish(encoded priv)` | none |
| HAWK | `prepared` | `hawk_shim_sign_finish_decoded(decoded priv)` | `hawk_decode_private_key` |
| E8 | `all_in` | `e8_sign_finish(encoded priv)` | none |
| E8 | `prepared` | `e8_sign_finish_prepared(prepared key)` | `e8_prepare_private_key` |

- `all_in`: everything from the encoded private key to the encoded signature.
  HAWK regenerates `f, g` from the key seed; E8 regenerates `f, g` and builds
  its NTT and F2 basis tables, on every call.
- `prepared`: the key is expanded once per key outside the timed call (HAWK
  decodes `f, g`; E8 also builds its basis tables).  The expansion is reported
  in its own CSV rows.
- Both timed calls include `hm = SHAKE256(message || hpub)`, per-attempt salt
  derivation with HAWK's construction (`SHAKE256(hm || K || enc32le(attempt)
  || rng(salt_len))`, with `K` the key seed in `all_in` and `f || g` in
  `prepared`), challenge hashing, sampling, rejection, reconstruction and
  encoding.  HAWK compresses its signature (and retries if the compressed
  signature does not fit); E8 encodes uncompressed.  Signature bytes are
  reported beside every timing.
- Keys come from `hawk_keygen()` / `e8_keygen()`, so every E8 signature is
  bound to a real public key.  Both schemes sign the same message set.  Every
  signature is verified with `hawk_verify_finish()` / `e8_verify_finish()`
  outside the timed region; failures are counted and reported.
- Attempts are counted identically: both signers call the RNG exactly twice
  per attempt (salt, then sampler seed), so `attempts = rng_calls / 2`.
- The E8 sampler thread count is a reported parameter (1, and 8, 8, 16 for
  n = 256, 512, 1024); HAWK is serial.  Thread fairness is handled separately
  (`plan.md`, Step 4).
- Before each block of signatures (scheme, degree, thread count), one untimed
  signature per accounting mode warms caches, and for E8 the sampler tables
  and worker pool.  Key generation, key preparation and message context setup
  are outside the timed region.

The benchmark (`e8/compare/e8_sign_bench.c`) writes one CSV row per
signature:

```text
kind,scheme,accounting,logn,trial,key_index,attempts,sig_bytes,
sampler_threads,tsc_cycles,wall_ns,verify_ok
```

with `kind = sign` (or `sign_failed`), plus `kind = keygen` and
`kind = prepare` rows for the separately timed setup.  The stderr summary
prints one table per accounting mode (median and mean cycles per signature,
median wall time, attempts per signature, signature bytes, threads, signing
and verification failures), then the setup costs.

```sh
make -B -C e8 \
  E8_CFLAGS='-Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always -DHAWK_ENABLE_E8_EXPERIMENTAL=1 -DHAWK_E8_DEBUG_CHECKS=0' \
  E8_PROFILE_SIGN_CFLAGS='-Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always -DHAWK_ENABLE_E8_EXPERIMENTAL=1 -DHAWK_E8_DEBUG_CHECKS=0 -DHAWK_E8_PROFILE_SIGN=1' \
  bin/e8_sign_bench bin/e8_sign_profile_bench
cd e8
./bin/e8_sign_bench --trials 1000 > e8_sign_bench.csv 2> e8_sign_bench.txt
./bin/e8_sign_profile_bench --trials 1000 > /dev/null 2> e8_sign_profile.txt
```

`--sigs-per-key N` (default 1) sets how many signatures share one key; it does
not change the accounting.  `--logn L` restricts the run to one degree.

Results under this accounting are to be measured on bare-metal Linux
(`plan.md`, Step 9); WSL runs are smoke tests only.

## Submission accounting (paper-v1-submission)

The rest of this document records the accounting used for the submitted
paper (tag `paper-v1-submission`).  It is superseded by the accounting above:
HAWK's "amortized key" figure subtracted separately measured key expansion and
salt derivation from its measured total, the E8 "all in" figure amortised
basis preparation over 1000 signatures per key while HAWK paid key expansion
on every signature, E8 reused one caller-supplied salt for every attempt, and
E8 signatures used a synthetic `hpub` rather than a real public key.  The
`--key-reuse` option referred to below no longer exists.

This summary is parity within 29% under the two symmetric accounting
models when E8 uses block parallel sampling.  The serial rows show that the
sampler advantage is what funds the remaining coset matching overhead.

Each row contains 1000 accepted signatures from the same run.  Cycles are
host specific and should be compared within this run.

### Optimized E8 Path

The final signer computes the target coset with bit packed F2 arithmetic and
per key 4-bit comb tables.  It reconstructs `w = B^-1 z` exactly with a shared
negacyclic NTT, Montgomery pointwise products, one 31-bit prime with a guarded
centered reduction, and caller owned per key basis transforms.  The sampler,
hash path, signature encoding, and verifier semantics are unchanged.

The arithmetic differential test compares the packed F2 path with a test local
bitwise reference and the NTT reconstruction with independent schoolbook/CRT
references for logn 8, 9, and 10.  The full E8 suite passes with debug checks
enabled and disabled, including the fixed seed byte for byte signature
regression.

### Amortized Key Accounting

Both schemes treat key material and salt derivation as setup.  The HAWK value
is its measured total minus one measured encoded key expansion and one measured
SHAKE salt derivation per observed attempt; the E8 value is the prepared call
with its caller supplied salt.

| logn | scheme | sampler | threads | attempts/sign | signature bytes | cycles/sign | cycles/attempt |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 8 | HAWK | serial | 1 | 1.174 | 249 | 69,020 | 58,790 |
| 8 | E8 | serial | 1 | 1.000 | 1,038 | 134,784 | 134,784 |
| 8 | E8 | block-parallel | 8 | 1.000 | 1,038 | 63,528 | 63,528 |
| 9 | HAWK | serial | 1 | 1.000 | 555 | 154,885 | 154,885 |
| 9 | E8 | serial | 1 | 1.000 | 2,072 | 277,514 | 277,514 |
| 9 | E8 | block-parallel | 8 | 1.000 | 2,072 | 126,220 | 126,220 |
| 10 | HAWK | serial | 1 | 1.000 | 1,221 | 330,714 | 330,714 |
| 10 | E8 | serial | 1 | 1.000 | 4,136 | 560,292 | 560,292 |
| 10 | E8 | block-parallel | 16 | 1.000 | 4,136 | 264,503 | 264,503 |

### All In Accounting

HAWK is its complete measured `hawk_sign_finish()` call.  E8 is its prepared
call plus one measured equivalent SHAKE salt derivation per signature and the
measured NTT/F2 basis preparation amortized over 1000 signatures per key.

| logn | scheme | sampler | threads | attempts/sign | signature bytes | cycles/sign | cycles/attempt |
| ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |
| 8 | HAWK | serial | 1 | 1.174 | 249 | 75,196 | 64,051 |
| 8 | E8 | serial | 1 | 1.000 | 1,038 | 136,042 | 136,042 |
| 8 | E8 | block-parallel | 8 | 1.000 | 1,038 | 64,877 | 64,877 |
| 9 | HAWK | serial | 1 | 1.000 | 555 | 166,812 | 166,812 |
| 9 | E8 | serial | 1 | 1.000 | 2,072 | 278,780 | 278,780 |
| 9 | E8 | block-parallel | 8 | 1.000 | 2,072 | 127,675 | 127,675 |
| 10 | HAWK | serial | 1 | 1.000 | 1,221 | 372,753 | 372,753 |
| 10 | E8 | serial | 1 | 1.000 | 4,136 | 562,555 | 562,555 |
| 10 | E8 | block-parallel | 16 | 1.000 | 4,136 | 267,290 | 267,290 |

For the threaded rows, E8 differs from HAWK by 8.0%, 18.5%, and 20.0% under
amortized-key accounting, and by 13.7%, 23.5%, and 28.3% under all in
accounting for logn 8, 9, and 10 respectively.  These are parallel versus 
serial comparisons; the E8 serial rows are the single core algorithmic view.

### Methods

HAWK: the timed `hawk_sign_finish()` region includes encoded key expansion,
per attempt SHAKE salt derivation, challenge generation, serial sampling,
reconstruction, rejection checks, compressed encoding, and encoding retries;
key generation and message context setup are outside.

E8: the timed prepared region starts with caller owned NTT/F2 bases and a
supplied salt, and includes challenge generation, target coset computation,
sampling, reconstruction, rejection checks, and uncompressed encoding; basis
preparation, salt derivation, key generation, cache warm up, and worker pool
warm up are outside.

HAWK attempts are counted from the signing RNG's per loop salt request (the
logn=10 counter accounts separately for the sampler's same-sized 40-byte seed
request).  E8 attempts come directly from the signer's trace.  Signature sizes
are `HAWK_SIG_SIZE(logn)` for HAWK and `e8_salt_len(logn) + 4*n` for E8.

The measured salt setup uses the same SHAKE256 structure as HAWK: a 64-byte
message digest, parameter-sized key material, a 32-bit counter, and random salt
input, with 14, 24, or 40 output bytes.  The `--key-reuse` option controls only
the amortization of E8's measured NTT and F2 preparation; it defaults to 1000.

### Reproduction

```sh
make -B -C e8 \
  E8_CFLAGS='-Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always -DHAWK_ENABLE_E8_EXPERIMENTAL=1 -DHAWK_E8_DEBUG_CHECKS=0' \
  E8_PROFILE_SIGN_CFLAGS='-Wall -Wextra -Wshadow -Wundef -O2 -fdiagnostics-color=always -DHAWK_ENABLE_E8_EXPERIMENTAL=1 -DHAWK_E8_DEBUG_CHECKS=0 -DHAWK_E8_PROFILE_SIGN=1' \
  bin/e8_sign_profile_bench
cd e8
./bin/e8_sign_profile_bench --trials 1000 --key-reuse 1000 \
  > e8_comparison_final.csv 2> e8_comparison_final.txt
```

The per trial CSV records attempts, signature bytes, cycles per attempt,
key expansion/salt equivalent cycles, and NTT/F2 preparation cycles.  The two
accounting tables are generated at the end of the stderr report.

Key reuse caveat: the prepared basis rows assume repeated signatures under one
key; the all in table states and charges the 1000 signature amortization.

Security caveat: the E8 path remains experimental, floating point,
data dependent, and non-constant time.

Format caveat: E8 emits an uncompressed `salt || s0 || s1` signature, while
ordinary HAWK pays for compressed encoding and its encod rejection loop.
