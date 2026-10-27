#!/usr/bin/env python3
"""
Analysis of the HAWK vs E8 comparison harness, Python
standard library only.  Run with python3 -I; it writes no bytecode.

usage: analyze.py [--out-dir DIR] [--bootstrap B] [--seed S]
                  [--run-tolerance F] [--core-tsc-tolerance F]
                  [--precision CSV] [--validation CSV] [--no-fail]
                  PATH...
       analyze.py --self-test

PATH: run directories of compare/run_compare.sh
(results/compare/<flag set>/<run_id>/) or single CSV files in the
per-signature schema (e8/common/cmn_csv.h; for example e8_sign_bench
output; NAME.csv.gz is read like NAME.csv).  Repeated runs of one
configuration (at least 5 process restarts per flag set) are
combined; different flag sets stay apart; inputs of more than one run set
(run_set in env.txt: all, headline, cross-ccd) are refused.

Writes to DIR (default results/compare/analysis/<first run_id>):

  summary.csv     per key (flags, mode, scheme, variant, threads, logn,
                  accounting): runs, signatures, warm-ups, failures,
                  attempts and rng_bytes per signature, sig_bytes, per
                  timing column the estimate (mean of per-run medians),
                  pooled median / q1 / q3 / mean and the run spread,
                  throughput per core, and the joined precision
                  (precision_summary.csv) and validation
                  (validation_summary.csv) columns
  runs.csv        per key and run: signatures and medians
  ratios.csv      E8 / HAWK for matched pairs, bootstrap 95% CIs
  keyverify_summary.csv
                  key generation (per scheme, variant and degree: runs,
                  keys, mean and median cycles, RNG calls per key) and
                  verification (per scheme, variant, verifier, degree
                  and accounting: runs, verifications, estimate and
                  pooled quartiles, run spread) and pub_decode
  keyverify_ratios.csv
                  E8 / HAWK key generation (ratio of means, paired
                  bootstrap) and verification (bootstrap) ratios
  acceptance.txt  the run-acceptance checks
  tables.md       the comparison tables for review
  tables.tex      the same tables in LaTeX (booktabs, graphicx)

Exit status 1 if an acceptance check fails (unless --no-fail); 2 if the
inputs cannot be analysed (a missing path, a single CSV that is empty or
not in the schema, mixed run sets, no signature rows).  The
statistics and checks are described in compare_data.py, the tables in
compare_tables.py.
"""

import argparse
import csv
import os
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import compare_data as cd  # noqa: E402
import compare_tables as ct  # noqa: E402

DOCS = os.path.normpath(os.path.join(HERE, '..', '..', 'docs'))
RESULTS = os.path.normpath(os.path.join(HERE, '..', '..', 'results'))


def fmt(x, nd=1):
    if x is None:
        return ''
    if isinstance(x, float):
        return '%.*f' % (nd, x)
    return str(x)


def write_summary(path, keys, precision, validation):
    cols = ['flags', 'mode', 'scheme', 'variant', 'threshold_bits',
            'technique', 'threads', 'logn', 'accounting', 'runs',
            'signatures', 'warmups', 'failures', 'attempts_per_sig',
            'rng_bytes_per_sig', 'sig_bytes', 'primary', 'procs',
            'sigs_per_s_per_core']
    for m in cd.TIMING_METRICS:
        cols += ['%s_%s' % (m, s) for s in ('est', 'median', 'q1', 'q3',
                                            'mean', 'run_spread')]
    cols += ['prec_' + c for c in cd.PRECISION_COLUMNS]
    cols += ['val_' + c for c in cd.VALIDATION_COLUMNS]
    with open(path, 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(cols)
        for k in sorted(keys):
            a = keys[k]
            jk = (a.scheme, cd.join_variant(a.variant), a.logn)
            row = [a.flags, a.mode, a.scheme, a.variant,
                   fmt(a.threshold_bits()), fmt(a.technique()), a.threads,
                   a.logn, a.accounting, len(a.cells), a.signatures(),
                   a.warmups(), a.failures(), fmt(a.attempts_per_sig(), 6),
                   fmt(a.rng_bytes_per_sig(), 1), fmt(a.sig_bytes()),
                   a.primary(), a.procs() if a.mode == 'throughput' else '',
                   fmt(a.throughput(), 1) if a.mode == 'throughput' else '']
            for m in cd.TIMING_METRICS:
                p = a.pooled(m)
                sp = a.spread(m)
                row += [fmt(a.estimate(m)),
                        fmt(p['median']) if p else '',
                        fmt(p['q1']) if p else '',
                        fmt(p['q3']) if p else '',
                        fmt(p['mean']) if p else '',
                        fmt(sp, 5) if sp is not None else '']
            pr = precision.get(jk, {})
            va = validation.get(jk, {})
            row += [pr.get(c, '') for c in cd.PRECISION_COLUMNS]
            row += [va.get(c, '') for c in cd.VALIDATION_COLUMNS]
            w.writerow(row)


def write_runs(path, keys):
    cols = ['flags', 'mode', 'scheme', 'variant', 'threads', 'logn',
            'accounting', 'run_id', 'signatures', 'attempts_per_sig']
    cols += ['%s_median' % m for m in cd.TIMING_METRICS]
    cols += ['sigs_per_s_per_core']
    with open(path, 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(cols)
        for k in sorted(keys):
            a = keys[k]
            for rid in a.run_ids():
                c = a.cells[rid]
                n = c.count()
                row = [a.flags, a.mode, a.scheme, a.variant, a.threads,
                       a.logn, a.accounting, rid, n,
                       fmt(c.attempts_sum / n, 6) if n else '']
                for m in cd.TIMING_METRICS:
                    v = sorted(c.values[m])
                    row.append(fmt(cd.quantile(v, 0.5))
                               if v and not c.missing[m] else '')
                tp = cd.throughput_of(c.procs) if a.mode == 'throughput' \
                    else None
                row.append(fmt(tp, 1) if tp else '')
                w.writerow(row)


def write_ratios(path, ratios):
    cols = ['flags', 'mode', 'pair', 'e8_variant', 'hawk_variant', 'threads',
            'logn', 'accounting', 'metric', 'runs', 'e8', 'hawk', 'ratio',
            'ci_lo', 'ci_hi']
    with open(path, 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(cols)
        for r in ratios:
            w.writerow([fmt(r[c], 4) if c in ('ratio', 'ci_lo', 'ci_hi')
                        else fmt(r[c]) for c in cols])


def write_kv_summary(path, verify, keygen, decode):
    cols = ['flags', 'what', 'scheme', 'variant', 'verifier', 'logn',
            'accounting', 'runs', 'count', 'metric', 'estimate',
            'median', 'q1', 'q3', 'run_spread', 'rng_calls_per_key',
            'failures', 'warmups']
    rows = []
    for (flags, logn, scheme, variant), per in keygen.items():
        mi = cd.kg_metric(per)
        est, med, nr, _ = cd.kg_stats(per, mi)
        n = sum(len(d) for d in per.values())
        calls = sum(t[3] for d in per.values() for t in d.values())
        rows.append([flags, 'keygen', scheme, variant, '', logn, '', nr, n,
                     ('core_cycles', 'tsc_cycles')[mi], fmt(est),
                     fmt(med), '', '', '', fmt(calls / n if n else None, 3),
                     '', ''])
    for (flags, logn, scheme, variant), per in decode.items():
        mi = 1 if any(t[0] is None for v in per.values() for t in v) else 0
        vals = [t[mi] for v in per.values() for t in v]
        d = cd.describe(vals) if vals else None
        rows.append([flags, 'pub_decode', scheme, variant, '', logn,
                     'prepared', len(per), len(vals),
                     ('core_cycles', 'tsc_cycles')[mi],
                     fmt(d['mean']) if d else '', fmt(d['median']) if d
                     else '', fmt(d['q1']) if d else '',
                     fmt(d['q3']) if d else '', '', '', '', ''])
    for k, a in verify.items():
        m = a.primary()
        p = a.pooled(m)
        sp = a.spread(m)
        rows.append([a.flags, 'verify', a.scheme, a.variant, a.verifier,
                     a.logn, a.accounting, len(a.cells), a.signatures(), m,
                     fmt(a.estimate(m)), fmt(p['median']) if p else '',
                     fmt(p['q1']) if p else '', fmt(p['q3']) if p else '',
                     fmt(sp, 5) if sp is not None else '', '',
                     a.failures(), a.warmups()])
    rows.sort(key=lambda r: [str(x) for x in r[:7]])
    with open(path, 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(cols)
        w.writerows(rows)


def write_kv_ratios(path, ratios):
    cols = ['flags', 'what', 'pair', 'logn', 'accounting', 'e8_variant',
            'e8_verifier', 'hawk_variant', 'metric', 'runs', 'e8', 'hawk',
            'ratio', 'ci_lo', 'ci_hi']
    with open(path, 'w', newline='') as f:
        w = csv.writer(f, lineterminator='\n')
        w.writerow(cols)
        for r in ratios:
            w.writerow([fmt(r[c], 4) if c in ('ratio', 'ci_lo', 'ci_hi')
                        else fmt(r[c]) for c in cols])


def acceptance(runs, keys, precision, validation, args, kv_verify=None):
    if kv_verify is None:
        kv_verify = cd.kv_aggregate(runs)[0]
    return [
        cd.check_failures(keys),
        cd.check_complete(runs),
        cd.check_attempts(keys, validation),
        cd.check_spread(keys, args.run_tolerance, kv_verify),
        cd.check_core_tsc(runs, keys, args.core_tsc_tolerance),
        cd.check_joins(keys, precision, validation),
        cd.check_preflight(runs),
        cd.check_kv_complete(runs),
        cd.check_verify_failures(runs, kv_verify),
        cd.check_verify_reject(runs),
    ]


def analyse(paths, out_dir, args):
    try:
        runs = [cd.load_run(p) for p in paths]
    except cd.LoadError as e:
        print('analyze.py: %s' % e, file=sys.stderr)
        return 2
    run_sets = sorted({r.run_set for r in runs})
    if len(run_sets) > 1:
        print('analyze.py: the inputs mix run sets (%s); the headline and'
              ' cross-CCD runs are analysed separately, each under its own'
              ' --out-dir' % ', '.join('%s: %d run(s)' % (
                  s, sum(r.run_set == s for r in runs)) for s in run_sets),
              file=sys.stderr)
        return 2
    keys = cd.aggregate(runs)
    kv_verify, kv_keygen, kv_decode = cd.kv_aggregate(runs)
    if not keys and not kv_verify and not kv_keygen:
        print('analyze.py: no signature rows in %s' % ' '.join(paths),
              file=sys.stderr)
        return 2
    precision = cd.read_summary(args.precision, cd.PRECISION_COLUMNS)
    validation = cd.read_summary(args.validation, cd.VALIDATION_COLUMNS)
    ratios = cd.ratio_rows(keys, args.bootstrap, args.seed)
    kv_ratios = cd.kv_ratio_rows(kv_verify, kv_keygen, args.bootstrap,
                                 args.seed)
    checks = acceptance(runs, keys, precision, validation, args, kv_verify)
    os.makedirs(out_dir, exist_ok=True)
    write_summary(os.path.join(out_dir, 'summary.csv'), keys, precision,
                  validation)
    write_runs(os.path.join(out_dir, 'runs.csv'), keys)
    write_ratios(os.path.join(out_dir, 'ratios.csv'), ratios)
    write_kv_summary(os.path.join(out_dir, 'keyverify_summary.csv'),
                     kv_verify, kv_keygen, kv_decode)
    write_kv_ratios(os.path.join(out_dir, 'keyverify_ratios.csv'), kv_ratios)

    header = ('HAWK vs E8 comparison, run set %s: %d run(s): %s' % (
        run_sets[0], len(runs),
        ', '.join('%s (%s)' % (r.run_id, r.flags) for r in runs)))
    env_notes = []
    for r in runs:
        e = r.env
        if e:
            env_notes.append(
                '%s: %s; %s; boost %s; governor %s; SMT %s; cpus %s;'
                ' core cycles %s; git %s%s' % (
                    r.run_id, e.get('cpu_model', '?'), e.get('kernel', '?'),
                    e.get('boost', '?'), e.get('governor', '?'),
                    e.get('smt_control', '?'), e.get('cpus_pinned', '?'),
                    e.get('core_cycles', '?'),
                    e.get('git_commit', '?')[:10],
                    ' (dirty)' if e.get('git_dirty', 'no') != 'no' else ''))
    acc_lines = ['%-18s %-4s %s' % (c.name, c.status, c.detail)
                 for c in checks]
    with open(os.path.join(out_dir, 'acceptance.txt'), 'w') as f:
        f.write(header + '\n\n' + '\n'.join(acc_lines) + '\n')
        if env_notes:
            f.write('\n' + '\n'.join(env_notes) + '\n')

    tables = ct.build_tables(runs, keys, ratios, precision,
                             (kv_verify, kv_keygen, kv_decode, kv_ratios))
    md_header = '# ' + header + '\n\n' + '\n'.join(
        '- ' + n for n in env_notes) + '\n\nAcceptance:\n\n' + '\n'.join(
        '    ' + l for l in acc_lines)
    if any(r.env.get('wsl') == 'yes' for r in runs):
        md_header += ('\n\nWSL run: smoke test only; no'
                      ' number here goes into the paper.')
    with open(os.path.join(out_dir, 'tables.md'), 'w') as f:
        f.write(ct.render_markdown(tables, md_header) + '\n')
    with open(os.path.join(out_dir, 'tables.tex'), 'w') as f:
        f.write(ct.render_latex(tables, header) + '\n')

    print(header)
    print('\n'.join(acc_lines))
    print('wrote summary.csv runs.csv ratios.csv keyverify_summary.csv'
          ' keyverify_ratios.csv acceptance.txt tables.md tables.tex in %s'
          % out_dir)
    failed = [c.name for c in checks if c.status == 'FAIL']
    if failed:
        print('analyze.py: acceptance checks failed: %s' % ', '.join(failed),
              file=sys.stderr)
        return 0 if args.no_fail else 1
    return 0


# ----------------------------------------------------------------------
# Self-test

def _synthetic_run(dirpath, run_id, flags='O2', verify_fail=False,
                   drop_row=False, scale=1.0, env_extra='', truncate=False,
                   extra_attempt=False):
    """A small latency run: HAWK ref and E8 ref, n = 256, T = 1, 2 keys x
    3 signatures.  truncate cuts the last row in half (a killed run)."""
    os.makedirs(dirpath, exist_ok=True)
    with open(os.path.join(dirpath, 'env.txt'), 'w') as f:
        f.write('run_id: %s\nflags: %s\nboost: off\n%s'
                % (run_id, flags, env_extra))
    cols = ['kind', 'run_id', 'scheme', 'variant', 'threshold_bits',
            'technique', 'threads', 'logn', 'key_index', 'msg_index',
            'attempts', 'sig_bytes', 'rng_bytes', 'tsc_cycles', 'core_cycles',
            'wall_ns', 'cpu_ns_all_threads', 'accounting',
            'encoding_included', 'verify_ok', 'mode', 'proc', 'flags', 'batch']
    rows = []

    def row(**kw):
        r = dict.fromkeys(cols, '')
        r.update(run_id=run_id, mode='latency', flags=flags, logn='8')
        r.update({k: str(v) for k, v in kw.items()})
        rows.append(r)

    for scheme in ('hawk', 'e8'):
        for k in range(2):
            row(kind='keygen', scheme=scheme, key_index=k, tsc_cycles=1000)
            row(kind='prepare', scheme=scheme, key_index=k, tsc_cycles=100,
                accounting='prepared')
    base = {'hawk': 100000, 'e8': 150000}
    batch = 0
    for scheme in ('hawk', 'e8'):
        for acct in ('all_in', 'prepared'):
            row(kind='warmup', scheme=scheme, variant='ref',
                threshold_bits=78, technique='ct_full_scan', threads=1,
                key_index=0, attempts=1, sig_bytes=249, rng_bytes=5174,
                tsc_cycles=base[scheme], core_cycles=base[scheme],
                wall_ns=1000, cpu_ns_all_threads=1000, accounting=acct,
                encoding_included=1, verify_ok=1)
    for k in range(2):
        for scheme in ('hawk', 'e8'):
            for m in range(3):
                for acct in ('all_in', 'prepared'):
                    c = int(base[scheme] * scale) + 37 * m + 11 * k
                    row(kind='sign', scheme=scheme, variant='ref',
                        threshold_bits=78, technique='ct_full_scan',
                        threads=1, key_index=k, msg_index=3 * k + m,
                        attempts=1, sig_bytes=249, rng_bytes=5174,
                        tsc_cycles=c, core_cycles=int(c * 1.25),
                        wall_ns=c // 4, cpu_ns_all_threads=c // 4,
                        accounting=acct, encoding_included=1, verify_ok=1,
                        batch=batch)
            batch += 1
    if verify_fail:
        rows[-1]['verify_ok'] = '0'
    if extra_attempt:
        rows[-1]['attempts'] = '2'
    if drop_row:
        del rows[-2]
    with open(os.path.join(dirpath, 'latency.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols, lineterminator='\n')
        w.writeheader()
        w.writerows(rows)
    if truncate:
        p = os.path.join(dirpath, 'latency.csv')
        with open(p, 'rb') as f:
            data = f.read()
        with open(p, 'wb') as f:
            f.write(data[:-40])


def _synthetic_kv(dirpath, run_id, tamper_accept=False, verify_fail=False,
                  truncate=False, scale=1.0):
    """A small keyverify run (n = 256): keyverify.csv, its --list file,
    sizes.csv and env.txt; 4 timed key generations per unit, 2 keys x 3
    signatures per verifier unit."""
    os.makedirs(dirpath, exist_ok=True)
    with open(os.path.join(dirpath, 'env.txt'), 'w') as f:
        f.write('run_id: %s\nflags: O2\nboost: off\n' % run_id)
    cols = ['kind', 'run_id', 'scheme', 'variant', 'threshold_bits',
            'technique', 'threads', 'logn', 'key_index', 'msg_index',
            'attempts', 'sig_bytes', 'rng_bytes', 'tsc_cycles', 'core_cycles',
            'wall_ns', 'cpu_ns_all_threads', 'accounting',
            'encoding_included', 'verify_ok', 'mode', 'proc', 'flags', 'batch',
            'verifier']
    rows, listed = [], {}

    def row(kind, scheme, key=0, cyc=None, ok=1, verifier='', acct='',
            attempts=''):
        r = dict.fromkeys(cols, '')
        r.update(kind=kind, run_id=run_id, scheme=scheme, variant='ref',
                 threads=1, logn=8, key_index=key, verify_ok=ok,
                 mode='keyverify', flags='O2', verifier=verifier,
                 accounting=acct, attempts=attempts,
                 rng_bytes=16 * attempts if attempts != '' else '')
        if cyc is not None:
            c = int(cyc * scale)
            r.update(tsc_cycles=c, core_cycles=int(c * 1.25), wall_ns=c // 4,
                     cpu_ns_all_threads=c // 4)
        rows.append({k: str(v) for k, v in r.items()})
        lk = (8, kind, scheme, 'ref', verifier, acct)
        listed[lk] = listed.get(lk, 0) + 1

    base = {'hawk': 2500000, 'e8': 2600000, 'hawk_core': 2480000}
    for scheme in ('hawk', 'e8', 'hawk_core'):
        row('warmup_keygen', scheme, 0, base[scheme], attempts=50)
    for i in range(4):
        for scheme in ('hawk', 'e8', 'hawk_core'):
            row('keygen_core' if scheme == 'hawk_core' else 'keygen',
                scheme, i, base[scheme] + 1000 * i, attempts=50 + i)
    units = (('hawk', 'upstream', 100000), ('e8', 'fast', 140000),
             ('e8', 'ref', 700000))
    for k in range(2):
        for scheme in ('hawk', 'e8'):
            row('pub_decode', scheme, k, 10000, acct='prepared')
    for scheme, vf, cyc in units:
        for acct in ('all_in', 'prepared'):
            for k in range(2):
                for flip in range(2 if scheme == 'hawk' else 3):
                    row('verify_reject', scheme, k, None, 0, vf, acct)
            row('warmup_verify', scheme, 0, cyc, 1, vf, acct)
            for j in range(6):
                row('verify', scheme, j // 3, cyc + 17 * j, 1, vf, acct)
    if tamper_accept:
        for r in rows:
            if r['kind'] == 'verify_reject':
                r['verify_ok'] = '1'
                break
    if verify_fail:
        rows[-1]['verify_ok'] = '0'
    with open(os.path.join(dirpath, 'keyverify.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols, lineterminator='\n')
        w.writeheader()
        w.writerows(rows)
    if truncate:
        p = os.path.join(dirpath, 'keyverify.csv')
        with open(p, 'rb') as f:
            data = f.read()
        with open(p, 'wb') as f:
            f.write(data[:-30])
    with open(os.path.join(dirpath, 'keyverify_list.csv'), 'w') as f:
        f.write('mode,logn,kind,scheme,variant,verifier,accounting,rows\n')
        for k, n in listed.items():
            f.write('keyverify,%d,%s,%s,%s,%s,%s,%d\n' % (k + (n,)))
    with open(os.path.join(dirpath, 'sizes.csv'), 'w') as f:
        f.write('scheme,object,logn,bytes\nhawk,pubkey,8,450\n'
                'e8,pubkey,8,2048\nhawk,sig,8,249\ne8,sig,8,1038\n')


def self_test():
    import random
    import statistics
    import tempfile

    ok = True

    def expect(cond, what):
        nonlocal ok
        print('%s %s' % ('ok  ' if cond else 'FAIL', what))
        ok = ok and cond

    # Exact median bootstrap against brute-force resampling.
    for n in (7, 8):
        xs = sorted(random.Random(n).sample(range(1000), n))
        r1, r2 = random.Random(1), random.Random(2)
        a = [cd.boot_median(xs, r1) for _ in range(20000)]
        b = [statistics.median(r2.choices(xs, k=n)) for _ in range(20000)]
        se = (statistics.pstdev(a) ** 2 / len(a)
              + statistics.pstdev(b) ** 2 / len(b)) ** 0.5
        expect(abs(statistics.fmean(a) - statistics.fmean(b)) < 4 * se
               and abs(statistics.pstdev(a) - statistics.pstdev(b))
               < 0.05 * statistics.pstdev(b),
               'order-statistic median bootstrap matches resampling (n=%d)'
               % n)

    class A:
        bootstrap, seed = 300, 7
        run_tolerance, core_tsc_tolerance = 0.03, 0.02
        precision = os.path.join(DOCS, 'precision_summary.csv')
        validation = os.path.join(DOCS, 'validation_summary.csv')
        no_fail = False

    with tempfile.TemporaryDirectory() as tmp:
        good = [os.path.join(tmp, 'good%d' % i) for i in range(3)]
        for i, d in enumerate(good):
            _synthetic_run(d, 'run%d' % i, scale=1.0 + 0.002 * i)
        runs = [cd.load_run(p) for p in good]
        keys = cd.aggregate(runs)
        r1 = cd.ratio_rows(keys, 300, 7)
        r2 = cd.ratio_rows(keys, 300, 7)
        expect(r1 == r2 and len(r1) > 0, 'bootstrap CIs are deterministic')
        main_ratio = [r for r in r1 if r['metric'] == 'core_cycles'
                      and r['accounting'] == 'all_in']
        expect(len(main_ratio) == 1 and main_ratio[0]['ci_lo']
               <= main_ratio[0]['ratio'] <= main_ratio[0]['ci_hi']
               and abs(main_ratio[0]['ratio'] - 1.5) < 0.01,
               'ratio of the synthetic pair is 1.5 inside its CI')
        checks = {c.name: c.status for c in acceptance(
            runs, keys, cd.read_summary(A.precision, cd.PRECISION_COLUMNS),
            cd.read_summary(A.validation, cd.VALIDATION_COLUMNS), A)}
        expect(checks['verify'] == 'PASS' and checks['complete'] == 'PASS'
               and checks['run_spread'] == 'PASS'
               and checks['core_vs_tsc'] == 'PASS'
               and checks['attempts'] == 'PASS',
               'acceptance passes on good runs (%s)' % checks)
        expect(analyse(good, os.path.join(tmp, 'out'), A) == 0,
               'analysis of good runs exits 0')

        bad = os.path.join(tmp, 'bad')
        _synthetic_run(bad, 'bad', verify_fail=True)
        expect(analyse([bad], os.path.join(tmp, 'out_bad'), A) == 1,
               'analysis fails on a verification failure')
        trunc = os.path.join(tmp, 'trunc')
        _synthetic_run(trunc, 'trunc', drop_row=True)
        checks = {c.name: c.status for c in acceptance(
            [cd.load_run(trunc)], cd.aggregate([cd.load_run(trunc)]),
            {}, {}, A)}
        expect(checks['complete'] == 'FAIL', 'a missing row fails completeness')
        spread = os.path.join(tmp, 'spread')
        _synthetic_run(spread, 'spread', scale=1.2)
        rs = [cd.load_run(good[0]), cd.load_run(spread)]
        expect(cd.check_spread(cd.aggregate(rs), 0.03).status == 'FAIL',
               'a 20% slower run fails the run-spread check')

        # Copies, inputs, run sets and the preflight record.
        copies = [cd.load_run(good[0])]
        for i in range(1, 40):      # 5 runs x 8 processes, identical
            copies.append(cd.load_run(good[0]))
        prec = cd.read_summary(A.validation, cd.VALIDATION_COLUMNS)
        expect(cd.check_attempts(cd.aggregate(copies), prec).status
               == 'PASS', 'identical copies of the signatures pass attempts')
        other = os.path.join(tmp, 'other')
        _synthetic_run(other, 'other', extra_attempt=True)
        expect(cd.check_attempts(cd.aggregate(
            [cd.load_run(good[0]), cd.load_run(other)]), prec).status
            == 'FAIL', 'copies with different attempts fail attempts')
        cut = os.path.join(tmp, 'cut')
        _synthetic_run(cut, 'cut', truncate=True)
        rs = [cd.load_run(cut)]
        expect(cd.check_complete(rs).status == 'FAIL',
               'a truncated CSV fails completeness')
        expect(analyse([os.path.join(tmp, 'none')], os.path.join(
            tmp, 'out_none'), A) == 2, 'a missing run directory is an error')
        cross = os.path.join(tmp, 'cross')
        _synthetic_run(cross, 'cross', env_extra='run_set: cross-ccd\n')
        expect(analyse([good[0], cross], os.path.join(tmp, 'out_mix'), A)
               == 2, 'mixed run sets are refused')
        forced = os.path.join(tmp, 'forced')
        _synthetic_run(forced, 'forced', env_extra=(
            'preflight: failed (forced: --force)\n'
            'preflight_boost: FAIL (on)\n'))
        passed = os.path.join(tmp, 'passed')
        _synthetic_run(passed, 'passed', env_extra='preflight: passed\n')
        expect(cd.check_preflight([cd.load_run(forced)]).status == 'FAIL'
               and cd.check_preflight([cd.load_run(passed)]).status
               == 'PASS', 'a forced preflight fails, a passed one passes')

        # Key generation and verification.
        kvg = [os.path.join(tmp, 'kv%d' % i) for i in range(2)]
        for i, d in enumerate(kvg):
            _synthetic_kv(d, 'kv%d' % i, scale=1.0 + 0.003 * i)
        kruns = [cd.load_run(p) for p in kvg]
        kver, kkg, _kdec = cd.kv_aggregate(kruns)
        checks = {c.name: c.status for c in acceptance(
            kruns, cd.aggregate(kruns), {}, {}, A, kver)}
        expect(checks['keyverify_complete'] == 'PASS'
               and checks['verify_failures'] == 'PASS'
               and checks['verify_reject'] == 'PASS'
               and checks['run_spread'] == 'PASS',
               'keyverify checks pass on good runs (%s)' % checks)
        kr = cd.kv_ratio_rows(kver, kkg, 300, 7)
        fast = [r for r in kr if r['what'] == 'verify'
                and r['e8_verifier'] == 'fast'
                and r['accounting'] == 'all_in']
        kg = [r for r in kr if r['what'] == 'keygen']
        expect(len(fast) == 1 and abs(fast[0]['ratio'] - 1.4) < 0.01
               and fast[0]['ci_lo'] <= fast[0]['ratio'] <= fast[0]['ci_hi']
               and len(kg) == 1 and kg[0]['ci_lo'] <= kg[0]['ratio']
               <= kg[0]['ci_hi'] and abs(kg[0]['ratio'] - 2601500.0
                                          / 2501500.0) < 1e-6,
               'keyverify ratios (verification 1.4, keygen ratio of means)'
               ' inside their CIs')
        expect(cd.kv_ratio_rows(kver, kkg, 300, 7) == kr,
               'keyverify bootstrap CIs are deterministic')
        expect(analyse(kvg, os.path.join(tmp, 'out_kv'), A) == 0,
               'analysis of good keyverify runs exits 0')
        for what, kw, check in (
                ('an accepted tampered signature', dict(tamper_accept=True),
                 'verify_reject'),
                ('a verification failure', dict(verify_fail=True),
                 'verify_failures'),
                ('a truncated keyverify.csv', dict(truncate=True),
                 'keyverify_complete')):
            d = os.path.join(tmp, 'kvbad_' + check)
            _synthetic_kv(d, 'kvbad', **kw)
            r = [cd.load_run(d)]
            st = {c.name: c.status for c in acceptance(
                r, cd.aggregate(r), {}, {}, A)}
            expect(st[check] == 'FAIL' and analyse(
                [d], os.path.join(tmp, 'out_' + check), A) == 1,
                '%s fails %s and the analysis' % (what, check))
        slow = os.path.join(tmp, 'kvslow')
        _synthetic_kv(slow, 'kvslow', scale=1.2)
        rs = [cd.load_run(kvg[0]), cd.load_run(slow)]
        expect(cd.check_spread({}, 0.03, cd.kv_aggregate(rs)[0]).status
               == 'FAIL', 'a 20% slower keyverify run fails run_spread')
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('paths', nargs='*')
    ap.add_argument('--out-dir')
    ap.add_argument('--bootstrap', type=int, default=2000)
    ap.add_argument('--seed', default='hawk-e8')
    ap.add_argument('--run-tolerance', type=float, default=0.03)
    ap.add_argument('--core-tsc-tolerance', type=float, default=0.02)
    ap.add_argument('--precision',
                    default=os.path.join(DOCS, 'precision_summary.csv'))
    ap.add_argument('--validation',
                    default=os.path.join(DOCS, 'validation_summary.csv'))
    ap.add_argument('--no-fail', action='store_true')
    ap.add_argument('--self-test', action='store_true')
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.paths:
        ap.error('no run directory or CSV given')
    out_dir = args.out_dir
    if out_dir is None:
        out_dir = os.path.join(RESULTS, 'compare', 'analysis',
                               cd.peek_run_id(args.paths[0]))
    return analyse(args.paths, out_dir, args)


if __name__ == '__main__':
    sys.exit(main())
