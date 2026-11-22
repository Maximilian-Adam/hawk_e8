"""
The paper's tables, built from the committed small outputs of the
bare-metal measurements (docs/measurements/: the analysis's summary,
ratios and keyverify CSVs and its tables.md, of the headline and
cross-CCD run sets).  Python standard library only; no raw CSV needed.

    python3 -I compare/analysis/paper_tables.py \
        [--measurements docs/measurements] [--out results/paper_tables]

(`make paper-tables` runs it and compiles preview.tex.)  The output is
generated, not committed: it is a function of the committed measurement
outputs alone (byte-identical reruns), and the paper copies it.

Writes one LaTeX fragment per table (tab_*.tex: a table float with
booktabs rules, no other package beyond amssymb's \\checkmark), the
macros they use (macros.tex, \\providecommand only), preview.tex (an
document at the LNCS text width and llncs's 9 pt \\footnotesize that
inputs them all, for the compile check) and
tables.md (the same tables in Markdown).  Layout: both flag sets side
by side, thousands of cycles, E8/HAWK ratios to two decimals with the
largest 95% CI half-width of the table stated in its caption, threshold
bits and the HAWK spec's eq. (6) as marks (the SD and RD values are in
the paper's precision table).

Every value is looked up by its full key and a missing one is an error;
values that do not depend on the flag set (attempts, random bytes,
sizes, key generation divergence) are checked to agree between the flag
sets; the analysis's acceptance must be all PASS.
"""

import argparse
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FLAGS = ('O2', 'O3native')
FLAG_TEX = {'O2': '\\texttt{O2}', 'O3native': '\\texttt{O3native}'}
# Technique of each variant (the sampler table checks it).
TECHNIQUE = {'ref': 'ct full scan', 'ref-T': 'ct full scan',
             'hot64': 'vt hot table', 'hot32': 'vt hot table'}
LOGNS = (8, 9, 10)
ACCTS = ('all_in', 'prepared')
ACCT_TEX = {'all_in': 'all-in', 'prepared': 'prep.'}
HEAD_T = (1, 2, 4, 8)          # headline thread counts
CCD_T = (1, 8, 16)             # cross-CCD columns shown


class InputError(Exception):
    pass


def fail(msg):
    raise InputError(msg)


# ---------------------------------------------------------------- inputs

def read_csv(path):
    if not os.path.isfile(path):
        fail('missing input %s' % path)
    with open(path, newline='') as f:
        rows = list(csv.DictReader(f))
    if not rows:
        fail('empty input %s' % path)
    return rows


def index(rows, cols, path):
    out = {}
    for r in rows:
        k = tuple(r[c] for c in cols)
        if k in out:
            fail('%s: duplicate key %s' % (path, k))
        out[k] = r
    return out


KEYVERIFY_CHECKS = ('keyverify_complete', 'verify_failures', 'verify_reject')


def check_acceptance(path, skip_ok=()):
    """Every check PASS; SKIP only for the checks in skip_ok (the
    keyverify checks of a run set without keyverify steps)."""
    if not os.path.isfile(path):
        fail('missing input %s' % path)
    checks = 0
    with open(path) as f:
        for line in f:
            m = re.match(r'\s*(\w+)\s+(PASS|FAIL|SKIP)\b', line)
            if not m:
                continue
            checks += 1
            if m.group(2) == 'SKIP' and m.group(1) in skip_ok:
                continue
            if m.group(2) != 'PASS':
                fail('%s: check %s is %s' % (path, m.group(1), m.group(2)))
    if checks == 0:
        fail('%s: no acceptance checks found' % path)
    return checks


def parse_md_tables(path):
    """tables.md of the analysis -> {(number, flags or None): (columns,
    rows)}, rows as lists of strings."""
    if not os.path.isfile(path):
        fail('missing input %s' % path)
    out, cur = {}, None
    with open(path) as f:
        for line in f:
            line = line.rstrip('\n')
            m = re.match(r'## Table (\w+): ', line)
            if m:
                fl = re.search(r'\(flags (\w+)[,)]', line)
                cur = (m.group(1), fl.group(1) if fl else None)
                out[cur] = (None, [])
                continue
            if cur is None or not line.startswith('|'):
                continue
            cells = [c.strip() for c in line.strip('|').split('|')]
            cols, rows = out[cur]
            if cols is None:
                out[cur] = (cells, rows)
            elif not all(re.fullmatch(r':?-+:?', c) for c in cells):
                rows.append(cells)
    return out


def md_table(md, number, flags, path):
    t = md.get((number, flags))
    if t is None or t[0] is None or not t[1]:
        fail('%s: no Table %s for flags %s' % (path, number, flags))
    cols, rows = t
    return [dict(zip(cols, r)) for r in rows]


def num(s):
    return float(s.replace(',', '').rstrip('%'))


class Data:
    def __init__(self, root):
        self.root = root
        h = os.path.join(root, 'headline')
        c = os.path.join(root, 'cross-ccd')
        self.acceptance = {'headline': check_acceptance(
            os.path.join(h, 'acceptance.txt')),
            'cross-ccd': check_acceptance(os.path.join(c, 'acceptance.txt'),
                                          KEYVERIFY_CHECKS)}
        skey = ('flags', 'mode', 'scheme', 'variant', 'threads', 'logn',
                'accounting')
        rkey = ('flags', 'mode', 'e8_variant', 'hawk_variant', 'threads',
                'logn', 'accounting', 'metric')
        p = os.path.join(h, 'summary.csv')
        self.summary = index(read_csv(p), skey, p)
        p = os.path.join(h, 'ratios.csv')
        self.ratios = index(read_csv(p), rkey, p)
        p = os.path.join(c, 'summary.csv')
        self.ccd_summary = index(read_csv(p), skey, p)
        p = os.path.join(c, 'ratios.csv')
        self.ccd_ratios = index(read_csv(p), rkey, p)
        p = os.path.join(h, 'keyverify_summary.csv')
        self.kv = index(read_csv(p), ('flags', 'what', 'scheme', 'variant',
                                      'verifier', 'logn', 'accounting'), p)
        p = os.path.join(h, 'keyverify_ratios.csv')
        self.kv_ratios = index(read_csv(p), (
            'flags', 'what', 'logn', 'accounting', 'e8_variant',
            'e8_verifier', 'hawk_variant'), p)
        self.md_path = os.path.join(h, 'tables.md')
        self.md = parse_md_tables(self.md_path)
        self.runs = {}
        for rs, d in (('headline', self.summary),
                      ('cross-ccd', self.ccd_summary)):
            self.runs[rs] = sorted({r['runs'] for r in d.values()})
        for rs in self.runs:
            if len(self.runs[rs]) != 1:
                fail('%s: rows with different run counts %s'
                     % (rs, self.runs[rs]))

    def s(self, flags, mode, scheme, var, T, logn, acct, ccd=False):
        d = self.ccd_summary if ccd else self.summary
        k = (flags, mode, scheme, var, str(T), str(logn), acct)
        if k not in d:
            fail('summary.csv (%s): no row %s'
                 % ('cross-ccd' if ccd else 'headline', k))
        return d[k]

    def r(self, flags, mode, ev, hv, T, logn, acct, metric, ccd=False):
        d = self.ccd_ratios if ccd else self.ratios
        k = (flags, mode, ev, hv, str(T), str(logn), acct, metric)
        if k not in d:
            fail('ratios.csv (%s): no row %s'
                 % ('cross-ccd' if ccd else 'headline', k))
        return d[k]

    def k(self, flags, what, scheme, var, verifier, logn, acct):
        key = (flags, what, scheme, var, verifier, str(logn), acct)
        if key not in self.kv:
            fail('keyverify_summary.csv: no row %s' % (key,))
        return self.kv[key]

    def kr(self, flags, what, logn, acct, ev, everifier, hv):
        key = (flags, what, str(logn), acct, ev, everifier, hv)
        if key not in self.kv_ratios:
            fail('keyverify_ratios.csv: no row %s' % (key,))
        return self.kv_ratios[key]

    def same(self, what, values):
        """A value that must not depend on the flag set."""
        if len(set(values)) != 1:
            fail('%s differs between the flag sets: %s' % (what, values))
        return values[0]


# ------------------------------------------------------------- formatting

def kc(x):
    """Thousands of cycles, one decimal, grouped."""
    return '{:,.1f}'.format(float(x) / 1e3)


def mc(x):
    return '{:,.2f}'.format(float(x) / 1e6)


def grouped(x):
    return '{:,}'.format(int(round(float(x))))


class Table:
    """A paper table: header groups over leaf columns, body rows (lists of
    TeX cells) and separators.  Cells are TeX; Markdown is derived."""

    def __init__(self, name, label, caption, align, groups, columns):
        self.name, self.label, self.caption = name, label, caption
        self.align = align                 # e.g. 'rl@{\quad}rrr'
        self.groups = groups               # [(text, span)], '' for none
        self.columns = columns             # leaf column heads
        self.body = []                     # ('row', cells) | ('rule',)
        self.ci_half = 0.0                 # largest CI half-width shown
        ncols = len(re.findall(r'[lrc]', re.sub(r'@\{[^}]*\}', '', align)))
        if ncols != len(columns) or (groups and sum(
                s for _, s in groups) != ncols):
            fail('table %s: %d columns in %r, %d heads, groups %s'
                 % (name, ncols, align, len(columns), groups))

    def row(self, cells):
        if len(cells) != len(self.columns):
            fail('table %s: row of %d cells, %d columns'
                 % (self.name, len(cells), len(self.columns)))
        self.body.append(('row', cells))

    def rule(self):
        if self.body and self.body[-1][0] != 'rule':
            self.body.append(('rule',))

    def section(self, text):
        self.rule()
        self.body.append(('section', text))

    def ratio(self, r):
        """A ratio cell from a ratios row; tracks the CI half-width."""
        v, lo, hi = float(r['ratio']), float(r['ci_lo']), float(r['ci_hi'])
        # The ratio column has 4 decimals; round from the estimates so
        # that a stored ...5 is not rounded twice.
        exact = float(r['e8']) / float(r['hawk'])
        if abs(exact - v) > 6e-5:
            fail('ratio %s, but e8 / hawk = %s' % (v, exact))
        v = exact
        if not lo - 6e-5 <= v <= hi + 6e-5:
            fail('ratio %s outside its CI [%s, %s]' % (v, lo, hi))
        self.ci_half = max(self.ci_half, v - lo, hi - v)
        return '%.2f' % v

    def ci_text(self):
        return ('%.3f' % self.ci_half)


def tex_cell(s):
    """Digit grouping as {,} so LaTeX does not add space after the comma."""
    return re.sub(r'(?<=\d),(?=\d{3})', '{,}', s)


def render_tex(t):
    lines = ['%% %s (generated by e8/compare/analysis/paper_tables.py from'
             ' e8/docs/measurements/; do not edit)' % t.name,
             '\\begin{table}[tbp]',
             '\\centering',
             '\\caption{%s}' % t.caption,
             '\\label{%s}' % t.label,
             '\\footnotesize',
             '\\setlength{\\tabcolsep}{3pt}',
             '\\begin{tabular}{@{}%s@{}}' % t.align,
             '\\toprule']
    ncols = len(t.columns)
    if t.groups:
        cells, rules, col = [], [], 1
        for text, span in t.groups:
            if text:
                cells.append('\\multicolumn{%d}{c}{%s}' % (span, text))
                trim = ('l' if col > 1 else '') + ('r' if col + span - 1
                                                    < ncols else '')
                rules.append('\\cmidrule(%s){%d-%d}' % (trim, col,
                                                       col + span - 1)
                             if trim else '\\cmidrule{%d-%d}'
                             % (col, col + span - 1))
            else:
                cells.extend([''] * span)
            col += span
        lines.append(' & '.join(cells) + ' \\\\')
        lines.append(''.join(rules))
    lines.append(' & '.join(t.columns) + ' \\\\')
    lines.append('\\midrule')
    for item in t.body:
        if item[0] == 'rule':
            lines.append('\\midrule')
        elif item[0] == 'section':
            lines.append('\\multicolumn{%d}{@{}l@{}}{%s} \\\\' % (ncols,
                                                              item[1]))
        else:
            lines.append(' & '.join(tex_cell(c) for c in item[1]) + ' \\\\')
    while lines[-1] == '\\midrule':
        lines.pop()
    lines += ['\\bottomrule', '\\end{tabular}', '\\end{table}', '']
    return '\n'.join(lines)


DETEX = (
    (r'\\HAWK\{\}', 'HAWK'), (r'\\EightCM\{\}', 'E8-CM'),
    (r'\\mathbb\{([^{}]*)\}', r'\1'),
    (r'\\checkmark', 'yes'), (r'\\texttt\{([^{}]*)\}', r'`\1`'),
    (r'\\textsf\{([^{}]*)\}', r'\1'), (r'\\emph\{([^{}]*)\}', r'*\1*'),
    (r'\\cref\{([^{}]*)\}', r'[\1]'), (r'\\cite\{([^{}]*)\}', r'[\1]'),
    (r'\\cite\[([^\]]*)\]\{([^{}]*)\}', r'[\2, \1]'),
    (r'\$\\times\$', 'x'), (r'\\times', 'x'), (r'\\le\b', '<='),
    (r'\\ge\b', '>='), (r'\\pm', '+-'), (r'\\approx', '~'),
    (r'\\rlap\{([^{}]*)\}', r'\1'),
    (r'\\dagger', '+'), (r'\\ast', '*'), (r'\\,', ' '), (r'\\%', '%'), (r'\{,\}', ','),
    (r'\\ldots', '...'), (r'~', ' '), (r'--', '-'),
    (r'\^\{([^{}]*)\}', r'^\1'), (r'\$', ''), (r'\\_', '_'),
    (r'\\&', '&'), (r'\\\\', ' '), (r'\\ ', ' '), (r'\{\}', ''),
)


def detex(s):
    for a, b in DETEX:
        s = re.sub(a, b, s)
    return re.sub(r'  +', ' ', s).strip()


def render_md(t):
    heads = []
    col = 0
    groups = t.groups or [('', len(t.columns))]
    for text, span in groups:
        for c in t.columns[col:col + span]:
            heads.append(detex(text + ' ' + c if text and c else text or c))
        col += span
    aligns = re.findall(r'[lrc]', re.sub(r'@\{[^}]*\}', '', t.align))
    lines = ['## %s (`%s`)' % (detex(t.caption.split('. ')[0]), t.label), '',
             detex(t.caption), '',
             '| ' + ' | '.join(heads) + ' |',
             '|' + '|'.join(' ---: ' if a == 'r' else ' --- '
                            for a in aligns) + '|']
    for item in t.body:
        if item[0] == 'row':
            lines.append('| ' + ' | '.join(detex(c) for c in item[1]) + ' |')
        elif item[0] == 'section':
            lines.append('| **' + detex(item[1]) + '** |'
                         + ' |' * (len(t.columns) - 1))
    lines.append('')
    return '\n'.join(lines)


# ----------------------------------------------------------------- tables

def n_of(logn):
    return str(1 << int(logn))


def mark(ok):
    if ok not in ('yes', 'no'):
        fail('eq. (6) flag %r is neither yes nor no' % ok)
    return '\\checkmark' if ok == 'yes' else '--'


def var_tex(v):
    return '\\texttt{%s}' % v


def two_flag_groups(lead, per_flag, unit=''):
    return [('', lead)] + [(FLAG_TEX[f] + unit, per_flag) for f in FLAGS]


def runs_text(d, rs='headline'):
    return d.runs[rs][0]


def attempts(row):
    return '%.3f' % float(row['attempts_per_sig'])


def table_signing(d, pairs, name, label, title, headline):
    lead = ['$n$', 'acct']
    lead_align = 'rl'
    lead += ['eq.\\,(6)']
    lead_align += 'c'
    groups = [('', len(lead))]
    lead += ['\\HAWK{}', '\\EightCM{}']
    lead_align += 'rr'
    groups.append(('random bytes', 2))
    cols = lead + ['\\HAWK{}', '\\EightCM{}', 'ratio'] * 2
    t = Table(name, label, '', lead_align + 'rrr' * 2,
              groups + [(FLAG_TEX[f] + ' (kcycles)', 3) for f in FLAGS], cols)
    att_note = {}
    for ev, hv in pairs:
        if headline:
            t.rule()
        else:
            t.section('%s, %s-bit thresholds' % (pair_tex(ev, hv), ev[3:]))
        for logn in LOGNS:
            for ai, acct in enumerate(ACCTS):
                h = [d.s(f, 'latency', 'hawk', hv, 1, logn, acct)
                     for f in FLAGS]
                e = [d.s(f, 'latency', 'e8', ev, 1, logn, acct)
                     for f in FLAGS]
                bits = d.same('threshold bits', [x['prec_threshold_bits']
                                                 for x in h + e])
                ok = '%s/%s' % (
                    mark(d.same('eq6', [x['prec_cond6_ok'] for x in h])),
                    mark(d.same('eq6', [x['prec_cond6_ok'] for x in e])))
                ha = d.same('attempts', [attempts(x) for x in h])
                ea = d.same('attempts', [attempts(x) for x in e])
                att_note.setdefault(('hawk', logn), []).append(float(ha))
                att_note.setdefault(('e8', logn), []).append(float(ea))
                if bits != ('78' if ev == 'ref' else ev[3:]):
                    fail('%s pair with %s-bit thresholds' % (ev, bits))
                hb = d.same('random bytes', [grouped(x['rng_bytes_per_sig'])
                                             for x in h])
                eb = d.same('random bytes', [grouped(x['rng_bytes_per_sig'])
                                             for x in e])
                first = ai == 0
                cells = [n_of(logn) if first else '', ACCT_TEX[acct],
                          ok if first else '', hb, eb]
                for i, f in enumerate(FLAGS):
                    r = d.r(f, 'latency', ev, hv, 1, logn, acct,
                            h[i]['primary'])
                    if h[i]['primary'] != 'core_cycles':
                        fail('signing rows without core cycles')
                    cells += [kc(h[i]['core_cycles_est']),
                              kc(e[i]['core_cycles_est']), t.ratio(r)]
                t.row(cells)
    common = (
        'kcycles: thousands of core cycles per signature on one core,'
        ' %s runs (setup, accounting and estimator in \\cref{sec:method}).'
        ' ratio:'
        ' \\EightCM{}/\\HAWK{}; every 95\\%% bootstrap CI half-width is at'
        ' most %s. eq.\\,(6): whether \\HAWK{}/\\EightCM{} meets eq.\\,(6)'
        ' of the \\HAWK{} specification (\\cref{tab:precision}). random'
        ' bytes: drawn per signature, attempts included. Attempts per'
        ' signature: \\HAWK{} %.3f to %.3f at $n=256$, at most %.3f above;'
        ' \\EightCM{} at most %.3f.'
        % (runs_text(d), t.ci_text(), min(att_note[('hawk', 8)]),
           max(att_note[('hawk', 8)]),
           max(att_note[('hawk', 9)] + att_note[('hawk', 10)]),
           max(v for k, l in att_note.items() if k[0] == 'e8' for v in l)))
    t.caption = '%s. %s' % (title, common)
    return t


def table_sampler(d):
    cols = ['scheme', 'variant', 'bits'] + \
        ['$%s$' % n_of(l) for l in LOGNS] * 3
    t = Table('tab_sampler', 'tab:cmp-sampler', '', 'llr' + 'rrr' * 3,
              [('', 3)] + [(FLAG_TEX[f] + ' cycles', 3) for f in FLAGS]
              + [('random bytes', 3)], cols)
    per = {}
    for f in FLAGS:
        for r in md_table(d.md, '3', f, d.md_path):
            per.setdefault((r['scheme'], r['variant']), {})[
                (f, r['n'])] = r
    order = [('HAWK', 'ref'), ('HAWK', 'ref-T'), ('HAWK', 'hot64'),
             ('HAWK', 'hot32'), ('E8', 'ref'), ('E8', 'hot64'),
             ('E8', 'hot32')]
    if set(per) != set(order):
        fail('Table 3: variants %s' % sorted(per))
    for scheme, var in order:
        if var == 'ref' and scheme == 'E8':
            t.rule()
        rows = per[(scheme, var)]
        for r in rows.values():
            if r['technique'] != TECHNIQUE[var] or r['bits'] != (
                    '78' if var.startswith('ref') else var[3:]):
                fail('Table 3: %s %s is %s bits %s' % (
                    scheme, var, r['bits'], r['technique']))
        cells = ['\\HAWK{}' if scheme == 'HAWK' else '\\EightCM{}',
                 var_tex(var), next(iter(rows.values()))['bits']]
        for f in FLAGS:
            for logn in LOGNS:
                r = rows.get((f, n_of(logn)))
                if r is None:
                    fail('Table 3: no %s %s n=%s %s' % (scheme, var,
                                                         n_of(logn), f))
                if int(r['runs']) != int(runs_text(d)):
                    fail('Table 3: %s %s %s runs' % (scheme, var, r['runs']))
                cells.append(grouped(num(r['cycles / 8'])))
        for logn in LOGNS:
            cells.append(d.same('sampler bytes', [
                rows[(f, n_of(logn))]['bytes / 8'] for f in FLAGS]))
        t.row(cells)
    t.caption = (
        'The samplers alone on one core: cycles and random bytes per 8'
        ' coordinates ($2n$ coordinates per call; the 40-byte seed'
        ' included), median per call, mean over %s runs. bits: threshold'
        ' width; \\texttt{ref}: constant-time full table scan;'
        ' \\texttt{hot64}, \\texttt{hot32}: variable-time hot table.'
        ' \\HAWK{} \\texttt{ref-T}: \\HAWK{}\'s \\texttt{ref} on'
        ' \\EightCM{}\'s per-worker stream split (the \\HAWK{} half of the'
        ' parallel pairs).' % runs_text(d))
    return t


PARALLEL_PAIRS = (('ref', 'ref-T'), ('hot64', 'hot64'), ('hot32', 'hot32'))


def t_heads(ts):
    """Thread-count column heads: T=1, then the bare counts."""
    return ['$T{=}%d$' % ts[0]] + ['$%d$' % T for T in ts[1:]]


def pair_tex(ev, hv):
    return var_tex(ev) if ev == hv else '%s/%s' % (var_tex(hv), var_tex(ev))


def table_parallel(d):
    cols = ['$n$', 'acct'] + (t_heads(HEAD_T) + ['CPU']) * 2
    t = Table('tab_parallel', 'tab:cmp-parallel', '',
              'rl' + 'rrrrr' * 2, two_flag_groups(2, 5), cols)
    for ev, hv in PARALLEL_PAIRS:
        t.section(pair_tex(ev, hv))
        for logn in LOGNS:
            for ai, acct in enumerate(ACCTS):
                cells = [n_of(logn) if ai == 0 else '', ACCT_TEX[acct]]
                for f in FLAGS:
                    for T in HEAD_T:
                        cells.append(t.ratio(d.r(f, 'latency', ev, hv, T,
                                                 logn, acct, 'tsc_cycles')))
                    cells.append(t.ratio(d.r(f, 'latency', ev, hv, 8, logn,
                                             acct, 'core_cycles')))
                t.row(cells)
    t.caption = (
        'Parallel signing latency, \\EightCM{}/\\HAWK{}, both schemes on'
        ' the same worker pool and stream split with $T$ threads on $T$'
        ' cores: ratio of wall-clock (TSC) cycles at $T{=}1$ to $8$, and'
        ' CPU, the ratio of core cycles summed over all threads (total CPU'
        ' per signature) at $T=8$. \\HAWK{} \\texttt{ref-T}: \\HAWK{}\'s'
        ' \\texttt{ref} on the worker streams. Every 95\\%% CI half-width is'
        ' at most %s; cycles in \\cref{tab:cmp-parallel-abs}.'
        % t.ci_text())
    return t


def table_parallel_abs(d):
    cols = ['$n$', 'acct', 'scheme'] + t_heads(HEAD_T) * 2
    t = Table('tab_parallel_abs', 'tab:cmp-parallel-abs', '',
              'rll' + 'rrrr' * 2, two_flag_groups(3, 4, ' (kcycles)'), cols)
    for ev, hv in PARALLEL_PAIRS:
        t.section(pair_tex(ev, hv))
        for logn in LOGNS:
            for ai, acct in enumerate(ACCTS):
                for si, (scheme, var) in enumerate((('hawk', hv),
                                                    ('e8', ev))):
                    top = ai == 0 and si == 0
                    cells = [n_of(logn) if top else '',
                             ACCT_TEX[acct] if si == 0 else '',
                             '\\HAWK{}' if scheme == 'hawk'
                             else '\\EightCM{}']
                    for f in FLAGS:
                        for T in HEAD_T:
                            cells.append(kc(d.s(f, 'latency', scheme, var, T,
                                                logn, acct)['tsc_cycles_est']))
                    t.row(cells)
    worst = max(abs(float(r['tsc_cycles_est']) / float(r['core_cycles_est'])
                    - 1) for r in d.summary.values()
                if r['mode'] == 'latency' and r['threads'] == '1')
    t.caption = (
        'Thousands of wall-clock (TSC) cycles per signature with $T$'
        ' threads, behind \\cref{tab:cmp-parallel}. At $T=1$ the TSC and'
        ' core-cycle counts of every configuration agree within %.1f\\%%.'
        % (100 * worst))
    return t


def table_parallel_ccd(d):
    cols = ['$n$', 'acct'] + (t_heads(CCD_T) + ['CPU']) * 2
    t = Table('tab_parallel_ccd', 'tab:cmp-parallel-ccd', '',
              'rl' + 'rrrr' * 2, two_flag_groups(2, 4), cols)
    for ev, hv in PARALLEL_PAIRS:
        t.section(pair_tex(ev, hv))
        for logn in LOGNS:
            for ai, acct in enumerate(ACCTS):
                cells = [n_of(logn) if ai == 0 else '', ACCT_TEX[acct]]
                for f in FLAGS:
                    for T in CCD_T:
                        cells.append(t.ratio(d.r(f, 'latency', ev, hv, T,
                                                 logn, acct, 'tsc_cycles',
                                                 ccd=True)))
                    cells.append(t.ratio(d.r(f, 'latency', ev, hv, 16, logn,
                                             acct, 'core_cycles', ccd=True)))
                t.row(cells)
    # Agreement of the cross-CCD T <= 8 rows with the headline's.
    worst = 0.0
    for k, row in d.ccd_summary.items():
        if row['mode'] != 'latency' or int(row['threads']) > 8:
            continue
        h = d.summary.get(k)
        if h is None:
            fail('cross-CCD key %s has no headline row' % (k,))
        worst = max(worst, abs(float(row['tsc_cycles_est'])
                               / float(h['tsc_cycles_est']) - 1))
    t.caption = (
        'Parallel signing latency across both CCDs, \\EightCM{}/\\HAWK{}: a'
        ' separate run set (%s runs per flag set) on all 16 cores, the'
        ' V-cache CCD\'s 8 first, so $T \\le 8$ runs there (wall-clock'
        ' cycles within %.2f\\%% of the headline runs) and $T=16$ spans'
        ' both CCDs (split L3, V-cache on one CCD only). Fair within the'
        ' run set, and never combined with the headline tables. Known'
        ' asymmetry: \\EightCM{}\'s sampler workers write $8n$ bytes per'
        ' call, \\HAWK{}\'s $2n$, so at $T=16$ \\EightCM{} moves more data'
        ' between the CCDs (an implementation property, not the'
        ' lattice\'s). Columns as in \\cref{tab:cmp-parallel}, CPU at'
        ' $T=16$; every 95\\%% CI half-width is at most %s.'
        % (runs_text(d, 'cross-ccd'), 100 * worst, t.ci_text()))
    return t


THROUGHPUT_PAIRS = (('ref', 'ref'), ('hot64', 'hot64'), ('hot32', 'hot32'))


def table_throughput(d):
    cols = ['$n$', 'acct'] + ['\\HAWK{}', '\\EightCM{}', 'ratio'] * 2
    t = Table('tab_throughput', 'tab:cmp-throughput', '', 'rl' + 'rrr' * 2,
              two_flag_groups(2, 3, ' (sig/s/core)'), cols)
    procs = set()
    ahead = []
    for ev, hv in THROUGHPUT_PAIRS:
        t.section(pair_tex(ev, hv))
        for logn in LOGNS:
            for ai, acct in enumerate(ACCTS):
                cells = [n_of(logn) if ai == 0 else '', ACCT_TEX[acct]]
                for f in FLAGS:
                    h = d.s(f, 'throughput', 'hawk', hv, 1, logn, acct)
                    e = d.s(f, 'throughput', 'e8', ev, 1, logn, acct)
                    procs |= {h['procs'], e['procs']}
                    r = d.r(f, 'throughput', ev, hv, 1, logn, acct,
                            'sigs_per_s_per_core')
                    cell = t.ratio(r)
                    if float(r['e8']) > float(r['hawk']):
                        cell += '\\rlap{$^\\ast$}'
                        ahead.append(r)
                    cells += [grouped(h['sigs_per_s_per_core']),
                              grouped(e['sigs_per_s_per_core']), cell]
                t.row(cells)
    if len(procs) != 1:
        fail('throughput rows with different process counts %s' % procs)
    star = ''
    if ahead:
        star = ' $^\\ast$\\EightCM{} ahead: ' + '; '.join(
            '%.3f, 95\\%% CI [%.3f, %.3f]' % (
                float(r['e8']) / float(r['hawk']), float(r['ci_lo']),
                float(r['ci_hi'])) for r in ahead) + '.'
    t.caption = (
        'Throughput per core: $P = %s$ single-thread signer processes, one'
        ' per core, running at once. Signatures per second of signing time,'
        ' mean over the processes and the %s runs; ratio'
        ' \\EightCM{}/\\HAWK{} (above 1 favours \\EightCM{}), every 95\\%%'
        ' CI half-width at most %s.%s'
        % (procs.pop(), runs_text(d), t.ci_text(), star))
    return t


# Stage categories shared by both profiles: (label, HAWK stages, E8 stages).
STAGES = (
    ('key preparation', ('key expand',), ('key prepare',)),
    ('salt, hashing', ('hash msg hpub', 'salt derivation',
                          'challenge hash'), ('salt', 'hash challenge')),
    ('target coset', ('target coset',), ('target coset',)),
    ('sampler', ('gaussian sampler',), ('e8 sampler',)),
    ('reconstruction', ('reconstruct s1',), ('reconstruct s',)),
    ('norm checks', ('norm rejection', 'symbreak bounds'),
     ('norm rejection',)),
    ('encoding', ('encode sig',), ('encode sig',)),
    ('other', ('other',), ('other',)),
)


def table_stages(d):
    cols = ['stage'] + ['\\HAWK{}', '\\EightCM{}'] * 3
    t = Table('tab_stages', 'tab:cmp-stages', '', 'l' + 'rr' * 3,
              [('', 1)] + [('$n=%s$' % n_of(l), 2) for l in LOGNS], cols)
    for f in FLAGS:
        st = {}
        for r in md_table(d.md, '6', f, d.md_path):
            if int(r['runs']) != int(runs_text(d)):
                fail('Table 6: %s runs' % r['runs'])
            st[(r['scheme'], r['n'], r['stage'])] = num(r['cycles / sign'])
        for scheme in ('HAWK', 'E8'):
            names = {k[2] for k in st if k[0] == scheme}
            want = {s for c in STAGES
                    for s in c[1 if scheme == 'HAWK' else 2]} | {'total'}
            if names != want:
                fail('Table 6 %s %s: stages %s, expected %s'
                     % (f, scheme, sorted(names), sorted(want)))
        t.section('%s, thousands of cycles' % FLAG_TEX[f])
        for label, hs, es in STAGES + (('total', ('total',), ('total',)),):
            if label == 'total':
                t.rule()
            cells = [label]
            for logn in LOGNS:
                for scheme, names in (('HAWK', hs), ('E8', es)):
                    cells.append(kc(sum(st[(scheme, n_of(logn), s)]
                                        for s in names)))
            t.row(cells)
        for logn in LOGNS:
            for scheme in ('HAWK', 'E8'):
                parts = sum(v for k, v in st.items() if k[0] == scheme
                            and k[1] == n_of(logn) and k[2] != 'total')
                total = st[(scheme, n_of(logn), 'total')]
                if abs(parts - total) > 1e-3 * total:
                    fail('Table 6 %s %s n=%s: stages sum to %s, total %s'
                         % (f, scheme, n_of(logn), parts, total))
    t.caption = (
        'Where the signing time goes: stage profile of both \\texttt{ref}'
        ' signers on one core, all-in, thousands of TSC cycles per'
        ' signature, mean over %s runs (profiling builds: the timers are'
        ' included, so the totals exceed \\cref{tab:cmp-ref}). \\HAWK{}:'
        ' reconstruction computes $s_1$, norm checks include'
        ' \\textsf{sym-break}. \\EightCM{}: key preparation builds the'
        ' prepared basis, reconstruction computes $s$.' % runs_text(d))
    return t


def table_avx2(d):
    cols = ['$n$', 'operation'] + ['\\HAWK{}', 'AVX2', '\\EightCM{}',
                                   'ratio'] * 2
    t = Table('tab_avx2', 'tab:cmp-avx2', '', 'rl' + 'rrrr' * 2,
              two_flag_groups(2, 4, ' (kcycles)'), cols)
    ops = (('sign all-in', 'sign', 'all_in'), ('sign prep.', 'sign',
                                               'prepared'),
           ('verify all-in', 'verify', 'all_in'),
           ('verify prep.', 'verify', 'prepared'))
    for logn in LOGNS:
        t.rule()
        for oi, (label, what, acct) in enumerate(ops):
            cells = [n_of(logn) if oi == 0 else '', label]
            for f in FLAGS:
                if what == 'sign':
                    h = d.s(f, 'latency', 'hawk', 'ref', 1, logn, acct)
                    x = d.s(f, 'latency', 'hawk', 'ref-avx2', 1, logn, acct)
                    e = d.s(f, 'latency', 'e8', 'ref', 1, logn, acct)
                    vals = [r['core_cycles_est'] for r in (h, x, e)]
                    r = d.r(f, 'latency', 'ref', 'ref-avx2', 1, logn, acct,
                            'core_cycles')
                else:
                    vals = [d.k(f, 'verify', 'hawk', 'ref', 'upstream', logn,
                                acct)['estimate'],
                            d.k(f, 'verify', 'hawk', 'ref-avx2', 'upstream',
                                logn, acct)['estimate'],
                            d.k(f, 'verify', 'e8', 'ref', 'fast', logn,
                                acct)['estimate']]
                    r = d.kr(f, 'verify', logn, acct, 'ref', 'fast',
                             'ref-avx2')
                cells += [kc(v) for v in vals] + [t.ratio(r)]
            t.row(cells)
    t.caption = (
        'Implementation context, not a fair pair: upstream \\HAWK{}\'s AVX2'
        ' build (AVX2) against its portable \\texttt{ref} (\\HAWK{}) and'
        ' \\EightCM{} (\\texttt{ref} signer, fast verifier), which has no'
        ' AVX2 path. Thousands of core cycles on one core, same runs as'
        ' \\cref{tab:cmp-ref,tab:cmp-verify}; ratio \\EightCM{}/AVX2, every'
        ' 95\\%% CI half-width at most %s. Key generation in'
        ' \\cref{tab:cmp-keygen}.' % t.ci_text())
    return t


def table_keygen(d):
    cols = ['scheme'] + ['mean', 'median', 'tail', 'ratio'] * 2 + \
        ['diverged']
    t = Table('tab_keygen', 'tab:cmp-keygen', '', 'l' + 'rrrr' * 2 + 'r',
              two_flag_groups(1, 4, ' (Mcycles)') + [('', 1)], cols)
    md = {f: {(r['n'], r['scheme'], r['variant']): r
              for r in md_table(d.md, '8', f, d.md_path)} for f in FLAGS}
    rows = (('\\HAWK{}', 'hawk', 'ref'), ('\\EightCM{}', 'e8', 'ref'),
            ('core', 'hawk_core', 'ref'),
            ('AVX2$^\\dagger$', 'hawk', 'ref-avx2'),
            ('AVX2 core$^\\dagger$', 'hawk_core', 'ref-avx2'))
    counts = set()
    for logn in LOGNS:
        t.section('$n=%s$' % n_of(logn))
        for ri, (label, scheme, var) in enumerate(rows):
            cells = [label]
            for f in FLAGS:
                row = d.k(f, 'keygen', scheme, var, '', logn, '')
                core = d.k(f, 'keygen', 'hawk_core', var, '', logn, '')
                counts.add((row['runs'], row['count']))
                tail = ''
                if scheme != 'hawk_core':
                    tv = float(row['estimate']) - float(core['estimate'])
                    tail = grouped(tv / 1e3)
                    mdt = md[f].get((n_of(logn), {'hawk': 'HAWK',
                                                  'e8': 'E8'}[scheme], var))
                    if mdt is None or abs(num(mdt['tail']) - tv) > 1:
                        fail('Table 8 %s: tail of %s %s n=%s disagrees'
                             % (f, scheme, var, n_of(logn)))
                ratio = ''
                if scheme == 'e8':
                    ratio = t.ratio(d.kr(f, 'keygen', logn, '', 'ref', '',
                                         'ref'))
                elif var == 'ref-avx2' and scheme == 'hawk':
                    ratio = t.ratio(d.kr(f, 'keygen', logn, '', 'ref', '',
                                         'ref-avx2'))
                cells += [mc(row['estimate']), mc(row['median']), tail,
                          ratio]
            div = ''
            if scheme == 'e8':
                div = d.same('diverged', [md[f][(n_of(logn), 'E8', 'ref')]
                                          ['diverged'] for f in FLAGS])
                div = div.replace('%', '\\%')
            cells.append(div)
            t.row(cells)
    if len(counts) != 1:
        fail('key generation rows with different counts %s' % counts)
    runs, count = counts.pop()
    t.caption = (
        'Key generation on one core: millions of core cycles per key, %s'
        ' runs of %s keys on paired seeds. mean: mean of the per-run means'
        ' (restarts make the cost heavy-tailed, so the mean is the expected'
        ' cost); median: mean of the per-run medians; ratio:'
        ' \\EightCM{}/\\HAWK{} of the means (\\EightCM{}/AVX2 on the AVX2'
        ' row), every 95\\%% paired-bootstrap CI half-width at most %s.'
        ' core: \\HAWK{}\'s NTRU key generation core, which both schemes'
        ' call; tail: mean minus core on the same keys, in thousands of'
        ' cycles (\\HAWK{}: public-key encoding; \\EightCM{}: public form,'
        ' limits and invertibility over 3 primes). diverged: keys on which'
        ' \\HAWK{} restarted after the core because its compressed public'
        ' key failed to encode, which \\EightCM{}\'s uncompressed key never'
        ' does; hence the ratio below 1 at $n = 512$ and $1024$.'
        ' $^\\dagger$Implementation context (\\EightCM{} has no AVX2 path).'
        % (runs, grouped(int(count) // int(runs)), t.ci_text()))
    return t


def table_verify(d):
    cols = ['verifier', 'primes'] + \
        ['all-in', 'prep.', 'all-in', 'prep.'] * 2
    groups = [('', 2)]
    for f in FLAGS:
        groups += [(FLAG_TEX[f] + ' kcycles', 2), ('ratio', 2)]
    t = Table('tab_verify', 'tab:cmp-verify', '', 'lr' + 'rrrr' * 2,
              groups, cols)
    rows = (('\\HAWK{}', 'hawk', 'upstream', '2'),
            ('\\EightCM{} fast', 'e8', 'fast', '3'),
            ('\\EightCM{} ref$^\\dagger$', 'e8', 'ref', '4'))
    dec = {'hawk': [], 'e8': []}
    for logn in LOGNS:
        t.section('$n=%s$' % n_of(logn))
        for ri, (label, scheme, vf, primes) in enumerate(rows):
            cells = [label, primes]
            for f in FLAGS:
                vals, rats = [], []
                for acct in ACCTS:
                    vals.append(kc(d.k(f, 'verify', scheme, 'ref', vf, logn,
                                       acct)['estimate']))
                    rats.append(t.ratio(d.kr(f, 'verify', logn, acct, 'ref',
                                             vf, 'ref'))
                                if scheme == 'e8' else '')
                cells += vals + rats
                if ri < 2:
                    dec[scheme].append(float(d.k(f, 'pub_decode', scheme,
                                                 'ref', '', logn,
                                                 'prepared')['estimate']))
            t.row(cells)
    t.caption = (
        'Verification on one core: thousands of core cycles per signature'
        ' and \\EightCM{}/\\HAWK{} ratios (every 95\\%% CI half-width at most'
        ' %s), %s runs of 10 keys $\\times$ 1{,}000 signatures. prep.: the'
        ' public key decoded outside the timed call (decoding costs'
        ' \\HAWK{} %s to %s and \\EightCM{} %s to %s kcycles). primes: NTT'
        ' primes of the norm check, the minimum each scheme\'s range'
        ' argument proves sound (\\cref{prop:range}). fast: \\EightCM{}\'s'
        ' verifier with \\HAWK{}\'s techniques. $^\\dagger$ref: the'
        ' verifier of an earlier version (tables rebuilt per call, one'
        ' inversion per coefficient, floating-point bound), context only.'
        % (t.ci_text(), runs_text(d), kc(min(dec['hawk'])),
           kc(max(dec['hawk'])), kc(min(dec['e8'])), kc(max(dec['e8']))))
    return t


SIZE_ROWS = (
    ('private key (encoded)', 'private key, encoded'),
    ('private key (prepared / decoded)', 'private key, prepared'),
    ('public key (encoded)', 'public key, encoded'),
    ('public key (decoded)', 'public key, decoded'),
    ('signature (encoded)', 'signature, encoded'),
    ('signature (decoded)', 'signature, decoded'),
    ('tmp: key generation', '\\texttt{tmp}, key generation'),
    ('tmp: signing (encoded key)', '\\texttt{tmp}, signing'),
    ('tmp: verification', '\\texttt{tmp}, verification'),
    ('tmp: verification, fastest', '\\texttt{tmp}, fast verification'),
)


def table_sizes(d):
    cols = ['object'] + ['\\HAWK{}', '\\EightCM{}'] * 3
    t = Table('tab_sizes', 'tab:cmp-sizes', '', 'l' + 'rr' * 3,
              [('', 1)] + [('$n=%s$' % n_of(l), 2) for l in LOGNS], cols)
    rows = {r['object']: r for r in md_table(d.md, '10', None, d.md_path)}
    if set(rows) != {a for a, _ in SIZE_ROWS}:
        fail('Table 10: objects %s' % sorted(rows))
    for src, label in SIZE_ROWS:
        r = rows[src]
        cells = [label]
        for logn in LOGNS:
            for scheme in ('HAWK', 'E8'):
                v = r['%s %s' % (scheme, n_of(logn))]
                cells.append(v if v else '--')
        t.row(cells)
    # The signature sizes must be the signed ones.
    for logn in LOGNS:
        for scheme, var in (('hawk', 'ref'), ('e8', 'ref')):
            s = d.s('O2', 'latency', scheme, var, 1, logn, 'all_in')
            v = rows['signature (encoded)']['%s %s' % (
                'HAWK' if scheme == 'hawk' else 'E8', n_of(logn))]
            if int(num(v)) != int(s['sig_bytes']):
                fail('Table 10: signature size %s, signed %s' % (
                    v, s['sig_bytes']))
    t.caption = (
        'Sizes in bytes. \\HAWK{}\'s signature is compressed ($s_1$ only),'
        ' \\EightCM{}\'s uncompressed (salt, $s_0$ and $s_1$). Prepared'
        ' private key: \\HAWK{}\'s decoded key, \\EightCM{}\'s per-key NTT'
        ' and $\\mathbb{F}_2$ basis tables in a fixed 40{,}960-byte layout;'
        ' decoded public key: $q_{00}$ and $q_{01}$ (\\EightCM{} as 32-bit'
        ' integers). \\texttt{tmp}: the caller-supplied scratch of each'
        ' call (\\EightCM{}\'s functions also use stack buffers, not'
        ' counted); a dash: the object does not exist for the scheme.')
    return t


def build(d):
    t1 = table_signing(
        d, (('ref', 'ref'),), 'tab_ref', 'tab:cmp-ref',
        'Signing on one core at the as-specified precision: \\HAWK{}'
        ' \\texttt{ref} against \\EightCM{} \\texttt{ref} (78-bit'
        ' thresholds, constant-time full table scan)', True)
    t2 = table_signing(
        d, (('hot64', 'hot64'), ('hot32', 'hot32')), 'tab_hot',
        'tab:cmp-hot', 'Signing on one core with the same variable-time'
        ' hot-table sampler in both schemes, at 64 bits (\\texttt{hot64})'
        ' and 32 bits (\\texttt{hot32})', False)
    return [t1, t2, table_sampler(d), table_parallel(d),
            table_parallel_abs(d), table_parallel_ccd(d),
            table_throughput(d), table_stages(d), table_avx2(d),
            table_keygen(d), table_verify(d), table_sizes(d)]


MACROS = r"""% Macros of the generated comparison tables (paper_tables.py); each
% only if the document does not define it.  The tables also need
% booktabs and amssymb (\checkmark).
\providecommand{\HAWK}{\textsf{HAWK}}
\providecommand{\EightCM}{\textsf{E8-CM}}
"""

PREVIEW_HEAD = r"""%% Compile check of the generated tables at the LNCS text width
%% (12.2 cm, 10 pt) and font sizes: llncs sets \footnotesize to \small
%% (9 pt), so the preview does too.  Not part of the paper.
\documentclass[10pt]{article}
\usepackage[T1]{fontenc}
\usepackage{amsmath,amssymb,booktabs}
\newtheorem{proposition}{Proposition}
\usepackage[paperwidth=16cm,paperheight=26cm,textwidth=122mm,textheight=193mm]{geometry}
\usepackage{hyperref}
\usepackage{cleveref}
\input{macros}
\let\footnotesize\small
\begin{document}
%% Placeholders for the paper's labels that the captions cite.
\section{Placeholder for the paper's methodology}\label{sec:method}
\begin{table}[h]\caption{Placeholder for the paper's precision table}
\label{tab:precision}\end{table}
\begin{proposition}[Placeholder for the paper's range proposition]
\label{prop:range}\end{proposition}
"""


def write(path, text):
    with open(path, 'w', newline='\n') as f:
        f.write(text)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--measurements', default=os.path.normpath(
        os.path.join(HERE, '..', '..', 'docs', 'measurements')))
    ap.add_argument('--out', default=os.path.normpath(
        os.path.join(HERE, '..', '..', 'results', 'paper_tables')))
    a = ap.parse_args(argv)
    try:
        d = Data(a.measurements)
        tables = build(d)
    except InputError as e:
        print('paper_tables: %s' % e, file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)
    write(os.path.join(a.out, 'macros.tex'), MACROS)
    md = ['# Comparison tables for the paper', '',
          'Generated by `e8/compare/analysis/paper_tables.py` from'
          ' `e8/docs/measurements/` (headline and cross-CCD run sets);'
          ' the LaTeX versions are the `tab_*.tex` files beside this one;'
          ' conventions and checks in `e8/docs/harness.md`, "Paper'
          ' tables". Do not edit.', '']
    preview = [PREVIEW_HEAD]
    for t in tables:
        write(os.path.join(a.out, t.name + '.tex'), render_tex(t))
        md.append(render_md(t))
        preview.append('\\input{%s}\n\\clearpage\n' % t.name)
    preview.append('\\end{document}\n')
    write(os.path.join(a.out, 'tables.md'), '\n'.join(md))
    write(os.path.join(a.out, 'preview.tex'), ''.join(preview))
    print('paper_tables: %d tables in %s (acceptance: headline %d,'
          ' cross-ccd %d checks PASS)' % (len(tables), a.out,
                                          d.acceptance['headline'],
                                          d.acceptance['cross-ccd']))
    return 0


if __name__ == '__main__':
    sys.exit(main())
