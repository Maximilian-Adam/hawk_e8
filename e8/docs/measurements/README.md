# Bare-metal measurements

The HAWK vs E8 comparison measured on bare metal with the comparison
harness ([`../harness.md`](../harness.md)) as the run book
([`../measurement_runbook.md`](../measurement_runbook.md)) describes: two
run sets, `headline` and `cross-ccd`, each 5 runs of both flag sets.  These
are the committed small outputs of their analyses; the raw per-signature
CSVs are archived outside git (two tarballs, one per run set, from which
the analysis reproduces these files byte for byte).

| directory | contents |
| --- | --- |
| `headline/` | `tables.md`, `tables.tex` (Tables 1-10), `acceptance.txt`, `summary.csv`, `runs.csv`, `ratios.csv`, `keyverify_summary.csv`, `keyverify_ratios.csv`; every run's `RUN_ID.env.txt` and `RUN_ID.commands.txt` |
| `cross-ccd/` | the same without the keyverify files (Table 4b) |

## Machine and setup

- AMD Ryzen 9 9950X3D (16 cores, two CCDs of 8; microcode `0xb404035`),
  Arch Linux, kernel `7.1.5-arch1-1`, GCC 16.1.1 20260725
  (`-march=native` = `znver5`).
- SMT off, amd-pstate in `passive` mode, governor `performance`, boost off,
  `perf_event_paranoid` 1; no `isolcpus` / `nohz_full`.  The desktop
  session stayed up with other applications closed and file indexing
  suspended.
- Passive mode, because in `active` mode (the Arch default) boost off is
  not applied on this kernel: `cpufreq/boost` read 0 and `scaling_max_freq`
  4.3 GHz, but pinned busy loops ran at 5.5 GHz (V-cache CCD) and 5.7 GHz
  (other CCD).  In passive mode every core holds the base clock: the
  preflight's busy loops measured 4.286-4.289 GHz on every pinned CPU
  (headline, spread at most 0.04%), and the serial core / TSC ratio was
  1.002-1.014 in every run.
- CPU lists (`compare/ccd_cpus.sh`): L3 0 (96 MiB, the V-cache CCD) has
  cores 0-7, L3 1 (32 MiB) cores 8-15.  `headline`: `0,1,2,3,4,5,6,7`;
  `cross-ccd`: `0,1,...,15` (the V-cache CCD first, so worker `w` sits on
  the same CPU in both run sets and the `T <= 8` rows run on the V-cache
  CCD).
- Flag sets: `O2` (`-O2`, as upstream) and `O3native` (`-O3 -march=native
  -fno-strict-aliasing`, see `harness.md`, "Builds").
- Code: commit `35e714c`, clean tree in every run (`git_dirty: no`,
  preflight `git` ok).  Volume per run and flag set: 10 keys x 1,000
  signatures per configuration; key generation 1,000 keys; verification
  10 keys x 1,000 signatures per verifier.  The full validation
  (`make validate`) passed on the same commit and machine immediately
  before the runs.

## Runs

Every run passed the preflight (boost, governor, SMT, own cores, perf,
equal frequencies at most `scaling_max_freq`, clean tree) and every step
exited 0.  Runs alternate O2 / O3native; times are UTC on 2026-11-01.

| run set | run ID | flags | start | minutes |
| --- | --- | --- | --- | ---: |
| headline | `20261101T180503Z-3868e103` | O2 | 18:05 | 7.6 |
| headline | `20261101T181241Z-03d6bc94` | O3native | 18:12 | 6.0 |
| headline | `20261101T181845Z-aebb1bea` | O2 | 18:18 | 7.6 |
| headline | `20261101T182623Z-b256e741` | O3native | 18:26 | 6.1 |
| headline | `20261101T183228Z-d046fc2d` | O2 | 18:32 | 7.6 |
| headline | `20261101T184006Z-0fb0bd3e` | O3native | 18:40 | 6.0 |
| headline | `20261101T184610Z-905cc769` | O2 | 18:46 | 7.6 |
| headline | `20261101T185348Z-a9faef6f` | O3native | 18:53 | 6.1 |
| headline | `20261101T185953Z-82d98c58` | O2 | 18:59 | 7.6 |
| headline | `20261101T190731Z-9bdbc684` | O3native | 19:07 | 6.1 |
| cross-CCD | `20261101T191706Z-bf033570` | O2 | 19:17 | 4.7 |
| cross-CCD | `20261101T192147Z-968f1666` | O3native | 19:21 | 3.4 |
| cross-CCD | `20261101T192513Z-171118ae` | O2 | 19:25 | 4.7 |
| cross-CCD | `20261101T192954Z-507af249` | O3native | 19:29 | 3.4 |
| cross-CCD | `20261101T193319Z-e746ecbc` | O2 | 19:33 | 4.7 |
| cross-CCD | `20261101T193800Z-217a4425` | O3native | 19:38 | 3.4 |
| cross-CCD | `20261101T194606Z-6cd28c07` | O3native | 19:46 | 3.4 |
| cross-CCD | `20261101T194933Z-ef8faeb1` | O2 | 19:49 | 4.7 |
| cross-CCD | `20261101T195413Z-a16d348f` | O3native | 19:54 | 3.4 |
| cross-CCD | `20261101T230632Z-e0a5ff9f` | O2 | 23:06 | 4.7 |

The headline run set took 72 minutes including its analysis (3.5
minutes); the cross-CCD run set 42 minutes for its first ten runs.  The first
analysis of the cross-CCD run set failed `run_spread` on 2 of 372 keys
(HAWK `hot32`, T = 16, n = 256, O2: 3.2% prepared, 3.1% all-in, in core
cycles; 1.2-1.4% in TSC).  One run, the fourth O2 run
(`20261101T194126Z-f64de05d`), had every HAWK T = 16 configuration at
n = 256 2.0-2.6% higher than the other runs (about 1% at n = 512, at most
0.5% at T <= 8), with nothing unusual in its `env.txt`; at T = 16 every
core is pinned, so other activity on the machine shares a benchmark core.
As the run book prescribes, that run was replaced by one more O2 run
(`20261101T230632Z-e0a5ff9f`, the same commit, setup and preflight) and
the run set analysed again; the replaced run is not part of these files.

## Acceptance

Every check passed in both analyses (`headline/acceptance.txt`,
`cross-ccd/acceptance.txt`): headline 49,200,000 signatures signed and
verified with 0 failures, attempts per signature as in the validation,
per-run medians within 3% for all 444 keys (largest 2.3%, the HAWK AVX2
context rows at O3native; 1.2% for every other row), serial core / TSC
within 2% in every run, 2,400,000 timed verifications accepted and 6,000
tampered signatures rejected by every verifier; cross-CCD 18,600,000
signatures with 0 failures, all 372 keys within 3% (largest 2.4%, E8
`hot32` T = 16 n = 256 prepared, O2; T = 16 is the noisiest thread count),
serial core / TSC 1.002-1.007 (the keyverify checks do not apply: the run
set has no keyverify steps).

## Reading the tables

- Table 8, `diverged`: HAWK's `hawk_keygen()` restarts after the shared
  NTRU core when its compressed public-key encoding fails; E8's
  uncompressed key never does.  Measured: 0.5% / 8.5% / 1.6% of key
  indices at n = 256 / 512 / 1024 (both flag sets), which is why E8 / HAWK
  key generation is below 1 at n = 512 and 1024 (`harness.md`, "Reading
  Tables 8-10").
- Table 7 and the `ref-avx2` rows of Tables 8 and 9 are upstream HAWK's
  AVX2 build, context only (E8 has no AVX2 path).
- Table 4b (cross-CCD) has its own conditions (split L3, V-cache on one
  CCD, cross-CCD transfers; E8's sampler workers write 8n bytes per call,
  HAWK's 2n).  Its `T <= 8` rows run on the same cores as the headline's
and agree with the headline Table 4 within 0.21% (TSC, every key).  The
preflight measured 4.257-4.278 GHz on the 16 cores of the cross-CCD runs
(spread at most 0.41%) against 4.287-4.289 GHz on the headline's 8.

## Implementation fixes found on this machine

The measured code is the branch head.  Three implementation defects showed
up on this machine and were fixed before these runs; outputs (samples,
signatures, `test-e8` output, every non-timing CSV column) are identical
before and after, timings changed as listed in `harness.md`, "Builds":

- E8's modular addition and subtraction (`e8_mp_add`, `e8_ntt` /
  `e8_intt`, the signer's `mp_sub`) compiled to data-dependent branches at
  O3native; they now use HAWK's mask form (commit `c67bf42`).
- E8 `ref`'s constant-time table scan staged rows in stack buffers that,
  for 1 in 16 stack offsets, 4K-aliased the table loads (O3native about
  5.5% slower in such a process); it now gathers in registers.  E8 `ref`
  signing got faster (n = 256 prepared: O2 265k -> 253k, O3native 192k ->
  160k cycles).
- HAWK `ref-T`'s scan copied each 40-byte random group to the stack, with
  the same effect (up to 3.3%); it now decodes in place (within 1%).
  Both scans still read every table entry (commit `35e714c`).

All earlier rehearsal and pilot runs are superseded by these runs.
