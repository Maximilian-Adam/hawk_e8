"""
The comparison tables from the comparison harness's runs, as
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
  8  key generation: mean and median per key, paired E8 / HAWK ratio,
     shared-core decomposition, key sizes
  9  verification: all-in and prepared public key, E8 fast and ref
     verifiers, ratios, prime counts, sizes
 10  sizes of every key, signature and tmp object

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


def _sizes(runs):
    """(scheme, object, logn) -> bytes from the first run's sizes.csv."""
    for run in runs:
        if run.sizes:
            return {(r['scheme'], r['object'], int(r['logn'])):
                    int(r['bytes']) for r in run.sizes}
    return {}


def _kv_ratio(kv_ratios, what, flags, logn, acct, verifier, hawk_variant):
    for r in kv_ratios:
        if (r['what'], r['flags'], r['logn'], r['accounting'],
                r['e8_verifier'], r['hawk_variant']) == (
                    what, flags, logn, acct, verifier, hawk_variant):
            return r
    return None


KEYGEN_NOTE = (
    'Each scheme\'s public keygen call (RNG to encoded private and public'
    ' key, retries included), one thread, pinned, interleaved per key index'
    ' in rotated order after warm-up. HAWK, E8 and the core (Hawk_keygen()'
    ' alone: HAWK\'s NTRU core, which both call) draw from the same paired'
    ' seed per key index. mean: mean of the per-run means over key indices'
    ' (key generation restarts, so its expected cost is the mean); median:'
    ' mean of the per-run medians. E8/HAWK: ratio of means, 95% CI from a'
    ' paired bootstrap (runs, then key indices). tail: the scheme\'s mean'
    ' minus the core\'s on the same key indices (HAWK: public-key'
    ' encoding; E8: its public form, key generation limits and'
    ' invertibility check over 3 primes; HAWK\'s own form and range check'
    ' run inside the core for both). diverged: share of key indices where'
    ' HAWK and E8 made different numbers of RNG calls (one restarted after'
    ' the core). draws: (f, g) draws (RNG calls) per key. Key bytes from'
    ' sizes.csv. HAWK AVX2 rows (ref-avx2): context, not a fair pair (E8'
    ' / HAWK AVX2 in its ratio column).')

VERIFY_NOTE = (
    'One thread, pinned, interleaved per signature in rotated order after'
    ' warm-up; inputs: the paired-stream keys, each scheme\'s ref signer,'
    ' all-in, T = 1 (verification cost does not depend on the sampler).'
    ' all-in: encoded signature and public key (decoding and hashing inside'
    ' the timed call; HAWK with HAWK_TMPSIZE_VERIFY_FAST, E8 with'
    ' E8_TMPSIZE_VERIFY); prepared: the public key decoded once per key'
    ' outside the timed call (decode only, for both; timed as pub dec), the'
    ' signature still encoded. Estimate: median per run, mean over runs;'
    ' E8/HAWK with its bootstrap 95% CI. primes: NTT primes of the norm'
    ' check, each scheme the minimum its own HAWK-style range argument'
    ' proves sound: HAWK 2, E8 3, because the E8-CM public form (paper'
    ' Sec. 4.5) has coefficients 2^3 to 2^7 times larger than HAWK\'s'
    ' (e8/docs/verifier_range.md); a cost of the lattice. E8 fast has'
    ' HAWK\'s verifier techniques (precomputed twiddles, constant NTT of'
    ' Delta, batch inversion, integer bound, early reject). E8 ref (4'
    ' primes, twiddles and Delta rebuilt per call, one Fermat inversion per'
    ' coefficient, double bound) is the verifier the submission measured,'
    ' as context. HAWK reconstructs s0 from s1 (its compressed signature'
    ' carries s1 only); E8\'s uncompressed signature carries s0 (about 4'
    ' times larger), so E8 does not. ref-avx2: HAWK AVX2 context (E8 fast'
    ' / HAWK AVX2 in its ratio column).')


def table_keygen(kv, runs, flags):
    verify, keygen, decode, kv_ratios = kv
    sizes = _sizes(runs)
    t = Table(8, 'Key generation: cycles per key', [
        'n', 'scheme', 'variant', 'mean', 'median', 'E8/HAWK [95% CI]',
        'tail', 'diverged', 'draws', 'pub B', 'priv B', 'runs x keys'],
        'rllrrlrrrrrr', KEYGEN_NOTE)
    for logn in (8, 9, 10):
        for hv in ('ref', 'ref-avx2'):
            core = keygen.get((flags, logn, 'hawk_core', hv))
            for scheme in ('hawk', 'e8', 'hawk_core'):
                if scheme == 'e8' and hv != 'ref':
                    continue
                per = keygen.get((flags, logn, scheme, hv))
                if per is None:
                    continue
                mi = cd.kg_metric(per)
                est, med, nr, nk = cd.kg_stats(per, mi)
                r = None
                if scheme == 'e8':
                    r = _kv_ratio(kv_ratios, 'keygen', flags, logn, '', '',
                                  'ref')
                elif scheme == 'hawk' and hv == 'ref-avx2':
                    r = _kv_ratio(kv_ratios, 'keygen', flags, logn, '', '',
                                  'ref-avx2')
                tail = div = None
                if core is not None and scheme != 'hawk_core':
                    idx = cd.kg_common(per, core)
                    if idx:
                        tail = cd.kg_stats(per, mi, idx)[0] \
                            - cd.kg_stats(core, mi, idx)[0]
                if scheme == 'e8':
                    h = keygen.get((flags, logn, 'hawk', 'ref'))
                    if h is not None:
                        div = cd.kg_diverged(per, h, cd.kg_common(per, h))
                n = sum(len(d) for d in per.values())
                draws = sum(t3[3] for d in per.values()
                            for t3 in d.values()) / n if n else None
                sz = 'e8' if scheme == 'e8' else 'hawk'
                t.rows.append([
                    str(n_of(logn)), {'hawk': 'HAWK', 'e8': 'E8',
                                      'hawk_core': 'core'}[scheme], hv,
                    f_int(est), f_int(med), f_ratio(r),
                    f_int(tail), '' if div is None else '%.1f%%'
                    % (100 * div), f_float(draws, 1),
                    '' if scheme == 'hawk_core' else
                    f_int(sizes.get((sz, 'pubkey', logn))),
                    '' if scheme == 'hawk_core' else
                    f_int(sizes.get((sz, 'privkey', logn))),
                    '%d x %d' % (nr, nk)])
    return t


def table_verify(kv, runs, flags):
    verify, keygen, decode, kv_ratios = kv
    sizes = _sizes(runs)
    t = Table(9, 'Verification: cycles per signature', [
        'n', 'acct', 'scheme', 'variant', 'verifier', 'primes', 'cycles',
        'TSC', 'E8/HAWK [95% CI]', 'pub dec', 'sig B', 'pub B'],
        'rllllrrrlrrr', VERIFY_NOTE)
    rows = (('hawk', 'ref', 'upstream'), ('e8', 'ref', 'fast'),
            ('e8', 'ref', 'ref'), ('hawk', 'ref-avx2', 'upstream'))
    for logn in (8, 9, 10):
        for acct in ('all_in', 'prepared'):
            for scheme, var, vf in rows:
                a = verify.get((flags, scheme, var, vf, logn, acct))
                if a is None:
                    continue
                m = a.primary()
                r = None
                if scheme == 'e8':
                    r = _kv_ratio(kv_ratios, 'verify', flags, logn, acct, vf,
                                  'ref')
                elif var == 'ref-avx2':
                    r = _kv_ratio(kv_ratios, 'verify', flags, logn, acct,
                                  'fast', 'ref-avx2')
                dec = decode.get((flags, logn, scheme, var))
                dv = None
                if dec:
                    mi = 0 if m == 'core_cycles' else 1
                    vals = [x[mi] for v in dec.values() for x in v
                            if x[mi] is not None]
                    dv = statistics.fmean(vals) if vals else None
                t.rows.append([
                    str(n_of(logn)), acct.replace('_', '-'),
                    'HAWK' if scheme == 'hawk' else 'E8', var, vf,
                    str(cd.VERIFIER_PRIMES[vf]), f_int(a.estimate(m)),
                    f_int(a.estimate('tsc_cycles')), f_ratio(r), f_int(dv),
                    f_int(sizes.get((scheme, 'sig', logn))),
                    f_int(sizes.get((scheme, 'pubkey', logn)))])
    return t


SIZE_OBJECTS = (('privkey', 'private key (encoded)'),
                ('privkey_prepared', 'private key (prepared / decoded)'),
                ('pubkey', 'public key (encoded)'),
                ('pubkey_decoded', 'public key (decoded)'),
                ('sig', 'signature (encoded)'),
                ('sig_decoded', 'signature (decoded)'),
                ('tmp_keygen', 'tmp: key generation'),
                ('tmp_sign', 'tmp: signing (encoded key)'),
                ('tmp_verify', 'tmp: verification'),
                ('tmp_verify_fast', 'tmp: verification, fastest'))


def table_sizes(runs):
    sizes = _sizes(runs)
    t = Table(10, 'Sizes in bytes', [
        'object', 'HAWK 256', 'E8 256', 'HAWK 512', 'E8 512', 'HAWK 1024',
        'E8 1024'], 'lrrrrrr',
        'From the size macros of hawk.h and hawk_e8.h (sizes.csv). E8\'s'
        ' prepared private key holds the per-key NTT and F2 basis tables of'
        ' the signer in a fixed 40,960-byte layout; its decoded public key'
        ' holds q00 and q01 as int32 (no precomputed NTT, as HAWK\'s). The'
        ' E8 functions also use stack buffers beyond tmp (not counted).'
        ' Empty: the object does not exist for the scheme.')
    if not sizes:
        return t
    for obj, label in SIZE_OBJECTS:
        row = [label]
        for logn in (8, 9, 10):
            for scheme in ('hawk', 'e8'):
                row.append(f_int(sizes.get((scheme, obj, logn))))
        t.rows.append(row)
    return t


def build_tables(runs, keys, ratios, precision, kv=None):
    """The tables of each flag set.  A cross-CCD run set (latency only)
    gives Table 4b alone; any other run set tables 1-9 per flag set and
    Table 10 (sizes) once."""
    out = []
    run_set = runs[0].run_set if runs else 'all'
    flagsets = {k[0] for k in keys}
    if kv is not None:
        flagsets |= {k[0] for k in kv[0]} | {k[0] for k in kv[1]}
    for flags in sorted(flagsets):
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
        if kv is not None:
            tabs += [table_keygen(kv, runs, flags),
                     table_verify(kv, runs, flags)]
        for t in tabs:
            t.title += ' (flags %s%s)' % (
                flags, '' if run_set == 'all' else ', run set ' + run_set)
            t.label = 'tab:cmp%s-%s' % (t.number, flags.lower())
        out.extend(tabs)
    if run_set != 'cross-ccd' and kv is not None:
        t = table_sizes(runs)
        t.label = 'tab:cmp10'
        out.append(t)
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
