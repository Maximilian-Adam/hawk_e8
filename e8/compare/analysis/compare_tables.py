"""
The tables of plan.md Step 10 from the comparison harness's runs, as
LaTeX (booktabs, graphicx) and Markdown.  Python standard library only; imported by
analyze.py.  One set of tables per flag set:

  1  headline, single core: HAWK ref vs E8 ref, both accounting modes
  2  same technique, single core: hot64 and hot32 pairs
  3  sampler only: cycles and random bytes per 8 coordinates, T = 1
     (e8_sampler_bench --variant-matrix)
  4  parallel latency: wall-clock (TSC) and total CPU (core) cycles per
     signature for T in the run, HAWK ref-T / hot64 / hot32 vs E8
  5  throughput per core: P single-thread signer processes
  6  stage breakdown at the headline variant (profile benches; their
     timings include the profile timers)
  7  context: HAWK AVX2 (not a fair pair: E8 has no AVX2 path)

Cycle columns are the primary metric, core cycles (TSC when the counter
is unavailable, as the notes then say); TSC and time are secondary.
Ratios are E8 / HAWK with the bootstrap 95% CI of compare_data.  The
precision columns come from e8/docs/precision_summary.csv.
"""

import re
import statistics

import compare_data as cd


class Table:
    def __init__(self, number, title, columns, align, notes=''):
        self.number, self.title = number, title
        self.columns, self.align = columns, align
        self.rows, self.notes = [], notes
        self.label = ''


def f_int(x):
    return '' if x is None else '{:,}'.format(int(round(x)))


def f_float(x, nd=2):
    return '' if x is None or x == '' else '{:.{}f}'.format(float(x), nd)


def f_loss(x):
    """RD loss factor: 4 decimals, scientific when huge, inf as is."""
    if x in (None, ''):
        return ''
    v = float(x)
    if v != v or v == float('inf'):
        return 'inf'
    return '%.4f' % v if v < 1e4 else '%.1e' % v


# Registry order of the variants (tables list them in this order).
VARIANT_ORDER = {'ref': 0, 'ref-T': 1, 'ref-avx2': 2, 'hot64': 3, 'hot32': 4}


def f_ratio(r):
    if r is None:
        return ''
    return '%.3f [%.3f, %.3f]' % (r['ratio'], r['ci_lo'], r['ci_hi'])


def n_of(logn):
    return 1 << logn


def ratio_index(ratios):
    return {(r['flags'], r['mode'], r['threads'], r['logn'], r['accounting'],
             r['e8_variant'], r['hawk_variant'], r['metric']): r
            for r in ratios}


def _prec(precision, scheme, variant, logn, col):
    p = precision.get((scheme, cd.join_variant(variant), logn))
    return p.get(col, '') if p else ''


def table_single(number, title, keys, ratios, precision, flags, pairs,
                 metric_note):
    t = Table(number, title,
              ['n', 'acct', 'scheme', 'variant', 'cycles', 'TSC', 'us',
               'attempts', 'sig B', 'rng B', 'bits', 'SD log2', 'loss',
               'eq6', 'E8/HAWK [95% CI]'],
              'rllcrrrrrrrrrcl', metric_note)
    ri = ratio_index(ratios)
    for ev, hv in pairs:
        for logn in (8, 9, 10):
            for acct in ('all_in', 'prepared'):
                for scheme, var in (('hawk', hv), ('e8', ev)):
                    a = keys.get((flags, 'latency', scheme, var, 1, logn,
                                  acct))
                    if a is None:
                        continue
                    m = a.primary()
                    r = None
                    if scheme == 'e8':
                        r = ri.get((flags, 'latency', 1, logn, acct, ev, hv,
                                    m))
                    wall = a.estimate('wall_ns')
                    t.rows.append([
                        str(n_of(logn)), acct.replace('_', '-'), scheme.upper()
                        if scheme == 'e8' else 'HAWK', var,
                        f_int(a.estimate(m)), f_int(a.estimate('tsc_cycles')),
                        f_float(wall / 1000.0 if wall else None, 1),
                        f_float(a.attempts_per_sig(), 4),
                        f_int(a.sig_bytes()), f_int(a.rng_bytes_per_sig()),
                        _prec(precision, scheme, var, logn, 'threshold_bits'),
                        _prec(precision, scheme, var, logn,
                              'sig_sd_bound_log2'),
                        f_loss(_prec(precision, scheme, var, logn,
                                     'loss_opt')),
                        _prec(precision, scheme, var, logn, 'cond6_ok'),
                        f_ratio(r)])
    return t


def table_sampler(runs, flags):
    t = Table(3, 'Sampler only, one thread: cycles and random bytes per 8'
              ' coordinates', ['scheme', 'variant', 'bits', 'technique', 'n',
                               'cycles / 8', 'TSC / 8', 'bytes / 8', 'runs'],
              'llrlrrrrr',
              'e8_sampler_bench --variant-matrix: median per call over its'
              ' trials, divided by n/4 (2n coordinates per call); bytes'
              ' include the 40-byte seed; mean over runs.')
    acc = {}
    for run in runs:
        if run.flags != flags:
            continue
        for row in run.sampler:
            if row.get('threads') != '1':
                continue
            k = (row['scheme'], row['variant'], row['threshold_bits'],
                 row['technique'], int(row['logn']))
            acc.setdefault(k, []).append(row)
    order = {'hawk': 0, 'e8': 1}
    for k in sorted(acc, key=lambda k: (k[4], order.get(k[0], 2),
                                        VARIANT_ORDER.get(k[1], 9))):
        rows = acc[k]
        units = n_of(k[4]) / 4.0

        def mean_of(col):
            vals = [float(r[col]) for r in rows if r.get(col)]
            return statistics.fmean(vals) if len(vals) == len(rows) else None

        core = mean_of('median_core_cycles')
        tsc = mean_of('median_tsc_cycles')
        byt = mean_of('rng_bytes_per_call')
        t.rows.append([k[0].upper() if k[0] == 'e8' else 'HAWK', k[1], k[2],
                       k[3].replace('_', ' '), str(n_of(k[4])),
                       f_float(core / units if core else None, 1),
                       f_float(tsc / units if tsc else None, 1),
                       f_float(byt / units if byt else None, 1),
                       str(len(rows))])
    return t


PARALLEL_NOTE = ('TSC: wall-clock cycles of the signing call; core: core'
                 ' cycles summed over the caller and the pool workers\' jobs'
                 ' (total CPU cycles; empty if the counter is unavailable).'
                 ' HAWK ref-T is HAWK\'s split ref on the same bound worker'
                 ' streams as E8.')

CROSS_CCD_NOTE = (
    ' Cross-CCD run set, kept apart from the headline tables: latency on 16'
    ' cores of both CCDs (SMT off, the same clock on both), the V-cache'
    ' CCD\'s 8 cores first, so T <= 8 runs on the V-cache CCD (compare'
    ' with Table 4) and T = 16 spans both CCDs (split L3, V-cache on one'
    ' CCD only, cross-CCD transfers). Fair within the run: same process,'
    ' interleaving, pinning, pool, RNG split, build, keys and messages for'
    ' both schemes. Known asymmetry: E8\'s sampler workers write 32-bit'
    ' outputs (8n bytes per call), HAWK\'s 8-bit outputs (2n bytes), so at'
    ' T = 16 E8 moves more data across the CCDs; an implementation'
    ' property, not the lattice\'s.')


def table_parallel(keys, ratios, flags, number=4,
                   title='Parallel latency: wall-clock and total CPU cycles'
                   ' per signature', note=PARALLEL_NOTE):
    t = Table(number, title,
              ['pair', 'n', 'acct', 'T', 'HAWK TSC', 'HAWK core',
               'E8 TSC', 'E8 core', 'E8/HAWK TSC [95% CI]', 'E8/HAWK core'],
              'lrlrrrrrlr', note)
    ri = ratio_index(ratios)
    threads = sorted({k[4] for k in keys if k[0] == flags and k[1] == 'latency'})
    for ev, hv in (('ref', 'ref-T'), ('hot64', 'hot64'), ('hot32', 'hot32')):
        for logn in (8, 9, 10):
            for acct in ('all_in', 'prepared'):
                for T in threads:
                    h = keys.get((flags, 'latency', 'hawk', hv, T, logn, acct))
                    e = keys.get((flags, 'latency', 'e8', ev, T, logn, acct))
                    if h is None or e is None:
                        continue
                    rt = ri.get((flags, 'latency', T, logn, acct, ev, hv,
                                 'tsc_cycles'))
                    rc = ri.get((flags, 'latency', T, logn, acct, ev, hv,
                                 'core_cycles'))
                    t.rows.append([
                        '%s / %s' % (hv, ev), str(n_of(logn)),
                        acct.replace('_', '-'), str(T),
                        f_int(h.estimate('tsc_cycles')),
                        f_int(h.estimate('core_cycles')),
                        f_int(e.estimate('tsc_cycles')),
                        f_int(e.estimate('core_cycles')),
                        f_ratio(rt), f_float(rc['ratio'] if rc else None, 3)])
    return t


def table_throughput(keys, ratios, flags):
    t = Table(5, 'Throughput per core: P independent single-thread signers',
              ['pair', 'n', 'acct', 'P', 'HAWK sig/s/core', 'E8 sig/s/core',
               'E8/HAWK [95% CI]'], 'lrlrrrl',
              'Signatures per second of signing time per process (count /'
              ' sum of wall_ns), mean over the P processes, which run the'
              ' same schedule (the same keys, messages and seeds, so the'
              ' same signatures) at the same time on their own (pinned)'
              ' CPUs.')
    ri = ratio_index(ratios)
    for ev, hv in cd.PAIRS:
        for logn in (8, 9, 10):
            for acct in ('all_in', 'prepared'):
                h = keys.get((flags, 'throughput', 'hawk', hv, 1, logn, acct))
                e = keys.get((flags, 'throughput', 'e8', ev, 1, logn, acct))
                if h is None or e is None:
                    continue
                r = ri.get((flags, 'throughput', 1, logn, acct, ev, hv,
                            'sigs_per_s_per_core'))
                t.rows.append(['%s / %s' % (hv, ev), str(n_of(logn)),
                               acct.replace('_', '-'), str(e.procs()),
                               f_int(h.throughput()), f_int(e.throughput()),
                               f_ratio(r)])
    return t


def table_stages(runs, flags):
    t = Table(6, 'Stage breakdown at the headline variant (ref, one thread,'
              ' all-in)', ['scheme', 'n', 'stage', 'cycles / sign', 'share',
                           'runs'], 'lrlrrr',
              'Profile benches (hawk_sign_profile_bench: upstream HAWK ref;'
              ' e8_sign_profile_bench: E8 ref): TSC cycles per accepted'
              ' signature including the per-stage timers, so the totals'
              ' exceed the timed figures; mean over runs.')
    acc = {}
    for run in runs:
        if run.flags != flags:
            continue
        for row in run.stages:
            if row.get('accounting') != 'all_in' or row.get('threads') != '1' \
                    or row.get('variant') != 'ref':
                continue
            k = (row['scheme'], int(row['logn']))
            sig = int(row['signatures'] or 0)
            if sig == 0:
                continue
            acc.setdefault(k, {}).setdefault(row['stage'], []).append(
                int(row['cycles_total']) / sig)
    for scheme in ('hawk', 'e8'):
        for logn in (8, 9, 10):
            stages = acc.get((scheme, logn))
            if not stages:
                continue
            total = statistics.fmean(stages.get('total', [0]))
            for stage, vals in stages.items():
                v = statistics.fmean(vals)
                t.rows.append(['HAWK' if scheme == 'hawk' else 'E8',
                               str(n_of(logn)), stage.replace('_', ' '),
                               f_int(v),
                               '%.1f%%' % (100.0 * v / total) if total else '',
                               str(len(vals))])
    return t


def table_avx2(keys, ratios, flags):
    t = Table(7, 'Context: HAWK AVX2 (upstream Optimized_Implementation; not'
              ' a fair pair)', ['n', 'acct', 'HAWK ref', 'HAWK ref-avx2',
                                'AVX2 / ref', 'E8 ref',
                                'E8 ref / HAWK AVX2 [95% CI]'],
              'rlrrrrl',
              'Implementation context only: E8 has no AVX2 path, so no claim'
              ' about the lattice rests on this table. Primary cycles, one'
              ' thread, same harness and flag set.')
    ri = ratio_index(ratios)
    for logn in (8, 9, 10):
        for acct in ('all_in', 'prepared'):
            h = keys.get((flags, 'latency', 'hawk', 'ref', 1, logn, acct))
            x = keys.get((flags, 'latency', 'hawk', 'ref-avx2', 1, logn, acct))
            e = keys.get((flags, 'latency', 'e8', 'ref', 1, logn, acct))
            if x is None:
                continue
            m = x.primary()
            hv = h.estimate(m) if h else None
            xv = x.estimate(m)
            r = ri.get((flags, 'latency', 1, logn, acct, 'ref', 'ref-avx2', m))
            t.rows.append([str(n_of(logn)), acct.replace('_', '-'), f_int(hv),
                           f_int(xv),
                           f_float(xv / hv if hv and xv else None, 3),
                           f_int(e.estimate(m) if e else None), f_ratio(r)])
    return t


def build_tables(runs, keys, ratios, precision):
    """The tables of each flag set.  A cross-CCD run set (latency only)
    gives Table 4b alone; any other run set tables 1-7."""
    out = []
    run_set = runs[0].run_set if runs else 'all'
    for flags in sorted({k[0] for k in keys}):
        if run_set == 'cross-ccd':
            t = table_parallel(keys, ratios, flags, '4b',
                               'Parallel latency across both CCDs',
                               PARALLEL_NOTE + CROSS_CCD_NOTE)
            t.title += ' (flags %s, run set cross-ccd)' % flags
            t.label = 'tab:cmp4b-%s' % flags.lower()
            out.append(t)
            continue
        lat = [a for k, a in keys.items() if k[0] == flags]
        prim = {a.primary() for a in lat if a.mode == 'latency'}
        note = ('cycles: core cycles (primary); TSC and us secondary.'
                if prim == {'core_cycles'} else
                'cycles: TSC (core cycles unavailable in some rows).')
        note += (' Estimates: median per run, mean over runs. all-in: from'
                 ' the encoded private key; prepared: from a prepared key'
                 ' (HAWK decoded, E8 prepared basis; preparation timed'
                 ' separately). attempts, sig B (encoded signature bytes:'
                 ' HAWK compressed, E8 uncompressed) and rng B (random bytes)'
                 ' per signature. Precision:'
                 ' threshold bits, per-signature SD bound (log2), RD loss'
                 ' factor at the optimal order and eq. (6) of the HAWK spec'
                 ' (e8/docs/precision_summary.csv).')
        tabs = [
            table_single(1, 'Headline, one core, as-specified precision:'
                         ' HAWK ref vs E8 ref', keys, ratios, precision,
                         flags, (('ref', 'ref'),), note),
            table_single(2, 'Same technique, one core: hot64 and hot32',
                         keys, ratios, precision, flags,
                         (('hot64', 'hot64'), ('hot32', 'hot32')), note),
            table_sampler(runs, flags),
            table_parallel(keys, ratios, flags),
            table_throughput(keys, ratios, flags),
            table_stages(runs, flags),
            table_avx2(keys, ratios, flags),
        ]
        for t in tabs:
            t.title += ' (flags %s%s)' % (
                flags, '' if run_set == 'all' else ', run set ' + run_set)
            t.label = 'tab:cmp%s-%s' % (t.number, flags.lower())
        out.extend(tabs)
    return out


def render_markdown(tables, header):
    lines = [header, '']
    for t in tables:
        lines.append('## Table %s: %s' % (t.number, t.title))
        lines.append('')
        if t.notes:
            lines.append(t.notes)
            lines.append('')
        if not t.rows:
            lines.append('(no data)')
            lines.append('')
            continue
        lines.append('| ' + ' | '.join(t.columns) + ' |')
        lines.append('|' + '|'.join(' ---: ' if a == 'r' else ' --- '
                                    for a in t.align) + '|')
        for row in t.rows:
            lines.append('| ' + ' | '.join(row) + ' |')
        lines.append('')
    return '\n'.join(lines)


def _tex(s):
    if s == 'us':
        return '$\\mu$s'
    s = s.replace('\\', '\\textbackslash{}')
    for a, b in (('&', '\\&'), ('%', '\\%'), ('_', '\\_'), ('#', '\\#')):
        s = s.replace(a, b)
    return re.sub(r'(?<=\d),(?=\d{3})', '{,}', s)


# Rows per LaTeX float: a longer table continues in further floats
# ("continued", same number) so that each fits a page.
TEX_MAX_ROWS = 40


def render_latex(tables, header):
    lines = ['% ' + header,
             '% Generated by e8/compare/analysis/analyze.py; requires'
             ' booktabs and graphicx.', '']
    for t in tables:
        nparts = max(1, -(-len(t.rows) // TEX_MAX_ROWS))
        size = -(-len(t.rows) // nparts) if t.rows else 0
        for part in range(nparts):
            rows = t.rows[part * size:(part + 1) * size]
            lines.append('%% Table %s: %s%s' % (
                t.number, t.title, ' (part %d of %d)' % (part + 1, nparts)
                if nparts > 1 else ''))
            lines.append('\\begin{table}[t]')
            lines.append('\\centering\\footnotesize')
            if part == 0:
                lines.append('\\caption{%s}' % _tex(t.title))
                lines.append('\\label{%s}' % t.label)
            else:
                lines.append('\\addtocounter{table}{-1}')
                lines.append('\\caption{%s (continued)}' % _tex(t.title))
            # Shrink a table wider than the text to fit; narrower ones
            # keep their size.
            lines.append('\\resizebox{\\ifdim\\width>\\linewidth'
                         '\\linewidth\\else\\width\\fi}{!}{%')
            lines.append('\\begin{tabular}{%s}' % t.align)
            lines.append('\\toprule')
            lines.append(' & '.join(_tex(c) for c in t.columns) + ' \\\\')
            lines.append('\\midrule')
            if not rows:
                lines.append('\\multicolumn{%d}{c}{(no data)} \\\\'
                             % len(t.columns))
            for row in rows:
                lines.append(' & '.join(_tex(c) for c in row) + ' \\\\')
            lines.append('\\bottomrule')
            lines.append('\\end{tabular}}')
            if t.notes and part == nparts - 1:
                lines.append('\\par\\smallskip\\parbox{\\linewidth}'
                             '{\\scriptsize %s}' % _tex(t.notes))
            lines.append('\\end{table}')
            lines.append('')
    return '\n'.join(lines)
