#!/usr/bin/env python3
"""
Analysis of the HAWK vs E8 comparison harness (plan.md Steps 7-10), Python
standard library only.  Run with python3 -I; it writes no bytecode.

usage: analyze.py [--out-dir DIR] [--bootstrap B] [--seed S]
                  [--run-tolerance F] [--core-tsc-tolerance F]
                  [--precision CSV] [--validation CSV] [--no-fail]
                  PATH...
       analyze.py --self-test

PATH: run directories of compare/run_compare.sh
(results/compare/<flag set>/<run_id>/) or single CSV files in the
per-signature schema (e8/common/cmn_csv.h; for example e8_sign_bench
output).  Repeated runs of one configuration (Step 9: at least 5 process
restarts per flag set) are combined; different flag sets stay apart.

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
  acceptance.txt  the run-acceptance checks of Step 9
  tables.md       the Step 10 tables for review
  tables.tex      the same tables in LaTeX (booktabs)

Exit status 1 if an acceptance check fails (unless --no-fail).  The
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
                       fmt(sum(c.attempts) / n, 6) if n else '']
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


def acceptance(runs, keys, precision, validation, args):
    return [
        cd.check_failures(keys),
        cd.check_complete(runs),
        cd.check_attempts(keys, validation),
        cd.check_spread(keys, args.run_tolerance),
        cd.check_core_tsc(runs, keys, args.core_tsc_tolerance),
        cd.check_joins(keys, precision, validation),
    ]


def analyse(paths, out_dir, args):
    runs = [cd.load_run(p) for p in paths]
    keys = cd.aggregate(runs)
    if not keys:
        print('analyze.py: no signature rows in %s' % ' '.join(paths),
              file=sys.stderr)
        return 2
    precision = cd.read_summary(args.precision, cd.PRECISION_COLUMNS)
    validation = cd.read_summary(args.validation, cd.VALIDATION_COLUMNS)
    ratios = cd.ratio_rows(keys, args.bootstrap, args.seed)
    checks = acceptance(runs, keys, precision, validation, args)
    os.makedirs(out_dir, exist_ok=True)
    write_summary(os.path.join(out_dir, 'summary.csv'), keys, precision,
                  validation)
    write_runs(os.path.join(out_dir, 'runs.csv'), keys)
    write_ratios(os.path.join(out_dir, 'ratios.csv'), ratios)

    header = ('HAWK vs E8 comparison: %d run(s): %s' % (
        len(runs), ', '.join('%s (%s)' % (r.run_id, r.flags) for r in runs)))
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
    acc_lines = ['%-12s %-4s %s' % (c.name, c.status, c.detail)
                 for c in checks]
    with open(os.path.join(out_dir, 'acceptance.txt'), 'w') as f:
        f.write(header + '\n\n' + '\n'.join(acc_lines) + '\n')
        if env_notes:
            f.write('\n' + '\n'.join(env_notes) + '\n')

    tables = ct.build_tables(runs, keys, ratios, precision)
    md_header = '# ' + header + '\n\n' + '\n'.join(
        '- ' + n for n in env_notes) + '\n\nAcceptance:\n\n' + '\n'.join(
        '    ' + l for l in acc_lines)
    if any(r.env.get('wsl') == 'yes' for r in runs):
        md_header += ('\n\nWSL run: smoke test only (plan.md Step 8); no'
                      ' number here goes into the paper.')
    with open(os.path.join(out_dir, 'tables.md'), 'w') as f:
        f.write(ct.render_markdown(tables, md_header) + '\n')
    with open(os.path.join(out_dir, 'tables.tex'), 'w') as f:
        f.write(ct.render_latex(tables, header) + '\n')

    print(header)
    print('\n'.join(acc_lines))
    print('wrote summary.csv runs.csv ratios.csv acceptance.txt tables.md'
          ' tables.tex in %s' % out_dir)
    failed = [c.name for c in checks if c.status == 'FAIL']
    if failed:
        print('analyze.py: acceptance checks failed: %s' % ', '.join(failed),
              file=sys.stderr)
        return 0 if args.no_fail else 1
    return 0


# ----------------------------------------------------------------------
# Self-test

def _synthetic_run(dirpath, run_id, flags='O2', verify_fail=False,
                   drop_row=False, scale=1.0):
    """A small latency run: HAWK ref and E8 ref, n = 256, T = 1, 2 keys x
    3 signatures."""
    os.makedirs(dirpath, exist_ok=True)
    with open(os.path.join(dirpath, 'env.txt'), 'w') as f:
        f.write('run_id: %s\nflags: %s\nboost: off\n' % (run_id, flags))
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
    if drop_row:
        del rows[-2]
    with open(os.path.join(dirpath, 'latency.csv'), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=cols, lineterminator='\n')
        w.writeheader()
        w.writerows(rows)


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
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('paths', nargs='*')
    ap.add_argument('--out-dir')
    ap.add_argument('--bootstrap', type=int, default=2000)
    ap.add_argument('--seed', default='hawk-e8-step7')
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
        first = cd.load_run(args.paths[0])
        out_dir = os.path.join(RESULTS, 'compare', 'analysis', first.run_id)
    return analyse(args.paths, out_dir, args)


if __name__ == '__main__':
    sys.exit(main())
