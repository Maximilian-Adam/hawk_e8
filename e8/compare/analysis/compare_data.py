"""
Loading, statistics, bootstrap and acceptance checks of the HAWK vs E8
comparison harness (plan.md Steps 7 and 9).  Python standard library only;
imported by analyze.py.

Inputs are run directories written by compare/run_compare.sh
(results/compare/<flag set>/<run_id>/: env.txt, latency*.csv,
throughput*.csv, sampler.csv, stages_*.csv) or single CSV files in the
per-signature schema of e8/common/cmn_csv.h (for example an e8_sign_bench
output; columns added in Step 7 may be missing and default to mode =
latency); NAME.csv.gz is read like NAME.csv.  A run directory's problems
(a step that failed or never finished per commands.txt, a missing, empty,
header-only, unreadable or malformed CSV) are collected for the complete
check.  Per run and key, the timing columns are kept as sorted compact
arrays and attempts and rng_bytes as sums (Step 8: under 1 GB at Step 9's
volume).

Statistics, per key = (flags, mode, scheme, variant, threads, logn,
accounting) and timing column:

  - per run: median, quartiles and mean over the run's signatures (rows of
    kind sign; warm-up rows are counted, never timed);
  - estimate: the mean of the per-run medians (one run: its median);
    spread: (max - min) / estimate of the per-run medians;
  - pooled: median, quartiles (IQR) and mean over all runs' signatures;
  - throughput (mode throughput): per process, signatures per second of
    signing time, count / sum(wall_ns) * 1e9; per run the mean over the
    processes (signatures per second per core); estimate: mean over runs.

Ratios E8 / HAWK are formed for matched pairs: same flags, mode, threads,
logn and accounting, and the variant pairs of PAIRS (plus the AVX2 context
pairs).  Bootstrap 95% CIs (percentile, B resamples, seeded per pair from
--seed, so deterministic): runs are resampled with replacement (the same
run indices for both schemes, as both ran in the same processes), then
signatures within each chosen run.  The resampled median of n values is
drawn exactly through its order statistic: the median of n uniform draws
of the sorted values has the index floor(n U), with U ~ Beta(k, n + 1 - k)
for the k-th order statistic (and the next order statistic for even n
from the minimum of the n - k uniforms above it).  Throughput resamples
each process's mean wall time from its normal approximation.
"""

from array import array
import csv
import gzip
import math
import os
import random
import statistics

TIMING_METRICS = ('core_cycles', 'tsc_cycles', 'wall_ns', 'cpu_ns_all_threads')

# (E8 variant, HAWK variant): the fair pairs (same threshold width and
# technique; HAWK ref-T is HAWK's split form of ref on the bound worker
# streams, the same stream construction as every E8 variant).
PAIRS = (('ref', 'ref'), ('ref', 'ref-T'), ('hot64', 'hot64'),
         ('hot32', 'hot32'))
# Context only (not fair: HAWK AVX2 has SIMD code E8 lacks).
CONTEXT_PAIRS = (('ref', 'ref-avx2'),)

PRECISION_COLUMNS = ('threshold_bits', 'sig_sd_bound_log2',
                     'sig_rd513_m1_log2', 'cond6_ok', 'a_opt', 'loss_opt')
VALIDATION_COLUMNS = ('signatures', 'sign_failures', 'verify_failures',
                      'attempts_per_sig')

# Upstream-entry-point HAWK variants that exist with one thread only.
SERIAL_ONLY = ('ref', 'ref-avx2')


def join_variant(variant):
    """Variant name in the precision / validation summaries."""
    return 'ref' if variant == 'ref-avx2' else variant


# ----------------------------------------------------------------------
# Loading

class LoadError(Exception):
    """An input that cannot be analysed at all (a missing path, a CSV that
    is empty or not in the schema); analyze.py reports it and exits 2."""


# The steps of compare/run_compare.sh; step S writes S.csv.  commands.txt
# lists a run's steps ("steps:"); run directories written before Step 8
# have no such line and ran all of them.
ALL_STEPS = ('latency', 'throughput', 'latency_avx2', 'sampler',
             'stages_hawk', 'stages_e8')


def read_env(path):
    env = {}
    if not os.path.exists(path):
        return env
    with open(path) as f:
        for line in f:
            if ':' in line:
                k, v = line.split(':', 1)
                env[k.strip()] = v.strip()
    return env


def read_commands(path):
    """commands.txt: its "key: value" header lines, and per step the exit
    status of its "start ..., exit N" line (None: the step started but no
    status was written, so the driver itself was stopped)."""
    hdr, exits = {}, {}
    if not os.path.exists(path):
        return hdr, exits
    label = None
    with open(path) as f:
        for line in f:
            if line.startswith('  start '):
                if label is not None:
                    exits[label] = line.rsplit('exit ', 1)[-1].strip()
                    label = None
            elif ':' in line:
                k, v = line.split(':', 1)
                k = k.strip()
                if k in ALL_STEPS:
                    label = k
                    exits[k] = None
                else:
                    hdr[k] = v.strip()
    return hdr, exits


def open_csv(path):
    """A CSV for reading; NAME.csv.gz (gzip) is read like NAME.csv."""
    if path.endswith('.gz'):
        return gzip.open(path, 'rt', newline='')
    return open(path, newline='')


def csv_name(name):
    """NAME.csv for NAME.csv or NAME.csv.gz, else None."""
    if name.endswith('.csv.gz'):
        return name[:-3]
    return name if name.endswith('.csv') else None


def is_sign_csv(path):
    try:
        with open_csv(path) as f:
            header = f.readline().strip().split(',')
    except (OSError, EOFError):
        return False
    return 'kind' in header and 'tsc_cycles' in header


class Cell:
    """The signatures of one key in one run.  Timing values are kept in
    compact unsigned arrays (4 bytes per value, widened to 8 if a value
    needs it) and sorted once loaded (finish()); attempts and rng_bytes
    only as sums, which is all the statistics use."""

    def __init__(self):
        self.values = {m: array('I') for m in TIMING_METRICS}
        self.missing = {m: 0 for m in TIMING_METRICS}
        self.n = 0
        self.attempts_sum = 0
        self.rng_bytes_sum = 0
        self.sig_bytes = set()
        self.threshold_bits = set()
        self.technique = set()
        self.sign_failures = 0
        self.verify_failures = 0
        self.warmups = 0
        self.warmup_failures = 0
        self.procs = {}     # proc -> [count, sum wall_ns, sum wall_ns^2]
        self.copies = {}    # proc -> [count, sum attempts, sum rng_bytes]
        self.batches = set()
        self.keys = set()

    def add(self, metric, v):
        a = self.values[metric]
        try:
            a.append(v)
        except OverflowError:
            self.values[metric] = a = array('q', a)
            a.append(v)

    def finish(self):
        for m in TIMING_METRICS:
            a = self.values[m]
            self.values[m] = array(a.typecode, sorted(a))

    def count(self):
        return self.n


class Run:
    def __init__(self, path):
        self.path = path
        self.env = {}
        self.commands = {}      # commands.txt header (run_set, steps, ...)
        self.step_exits = {}    # step -> exit status (str) or None
        self.run_id = None
        self.flags = None
        self.run_set = 'all'
        self.cells = {}     # key -> Cell
        self.files = []     # sign-schema CSVs
        self.sampler = []   # rows of sampler.csv
        self.stages = []    # rows of stages_*.csv
        self.setup = {}     # (file, mode, proc, scheme, logn, kind) -> set(key_index)
        self.file_counts = {}   # (file, mode, proc) -> {(cfg, acct): count}
        self.file_warmups = {}  # (file, mode, proc) -> {(cfg, acct): count}
        self.file_keys = {}     # (file, mode, proc, scheme, logn) -> set(key_index)
        self.flag_values = set()
        self.problems = []      # missing, empty or malformed inputs


def peek_run_id(path):
    """The run_id of a run directory (env.txt) or CSV (its first row)
    without loading it; else the path's base name."""
    if os.path.isdir(path):
        rid = read_env(os.path.join(path, 'env.txt')).get('run_id')
    else:
        rid = None
        try:
            with open_csv(path) as f:
                row = next(csv.DictReader(f), None)
            rid = row.get('run_id') if row else None
        except (OSError, EOFError, csv.Error):
            pass
    return rid or os.path.basename(os.path.normpath(path))


def load_run(path):
    if not os.path.exists(path):
        raise LoadError('%s: no such run directory or CSV' % path)
    run = Run(path)
    if os.path.isdir(path):
        run.env = read_env(os.path.join(path, 'env.txt'))
        cpath = os.path.join(path, 'commands.txt')
        run.commands, run.step_exits = read_commands(cpath)
        files = sorted(os.listdir(path))
        names = [csv_name(f) for f in files]
        if len(set(n for n in names if n)) < sum(1 for n in names if n):
            raise LoadError('%s: both NAME.csv and NAME.csv.gz present'
                            % path)
        for fname, name in zip(files, names):
            p = os.path.join(path, fname)
            if name is None:
                continue
            if os.path.getsize(p) == 0:
                run.problems.append('%s is empty' % fname)
            elif name == 'sampler.csv':
                with open_csv(p) as f:
                    run.sampler = list(csv.DictReader(f))
                if not run.sampler:
                    run.problems.append('%s has no rows' % fname)
            elif name.startswith('stages_'):
                with open_csv(p) as f:
                    rows = list(csv.DictReader(f))
                if not rows:
                    run.problems.append('%s has no rows' % fname)
                run.stages = run.stages + rows
            elif is_sign_csv(p):
                run.files.append(p)
            else:
                run.problems.append('%s is not in the per-signature schema'
                                    % fname)
        # A driver-written run directory: every step's output and env.txt.
        if os.path.exists(cpath):
            steps = run.commands.get('steps', '').split() or ALL_STEPS
            for s in steps:
                if s + '.csv' not in names:
                    run.problems.append('%s.csv missing' % s)
            if 'env.txt' not in files:
                run.problems.append('env.txt missing')
            for s, rc in sorted(run.step_exits.items()):
                if rc != '0':
                    run.problems.append('step %s %s' % (
                        s, 'has no exit status (driver stopped)'
                        if rc is None else 'exited with status %s' % rc))
            for s in steps:
                if s not in run.step_exits:
                    run.problems.append('step %s never started' % s)
    else:
        if os.path.getsize(path) == 0 or not is_sign_csv(path):
            raise LoadError('%s: empty or not a per-signature CSV' % path)
        run.files.append(path)
    for p in run.files:
        try:
            _load_sign_csv(run, p)
        except (OSError, EOFError, csv.Error) as e:
            run.problems.append('%s unreadable after some rows (%s)'
                                % (os.path.basename(p), e))
    for cell in run.cells.values():
        cell.finish()
    if run.run_id is None:
        run.run_id = run.env.get('run_id') or os.path.basename(path)
    if run.flags is None:
        run.flags = run.env.get('flags', 'unknown')
    run.run_set = (run.env.get('run_set') or run.commands.get('run_set')
                   or 'all')
    return run


def _load_sign_csv(run, path):
    fname = csv_name(os.path.basename(path)) or os.path.basename(path)
    env_flags = run.env.get('flags')
    rows = malformed = 0
    # Per distinct (scheme, variant, threads, logn, accounting, flags,
    # mode, proc) text: (cell, (cfg, acct), fk, logn).
    cache = {}
    with open_csv(path) as f:
        reader = csv.reader(f)
        header = next(reader, None) or []
        col = {c: i for i, c in enumerate(header)}
        width = len(header)

        def ix(name):
            # Optional columns (added in Step 7) may be missing.
            return col.get(name, width)

        i_kind, i_run, i_scheme = col['kind'], col['run_id'], col['scheme']
        i_var, i_thr, i_logn = col['variant'], col['threads'], col['logn']
        i_key, i_acct, i_ok = col['key_index'], col['accounting'], \
            col['verify_ok']
        i_att, i_rng, i_sig = col['attempts'], col['rng_bytes'], \
            col['sig_bytes']
        i_bits, i_tech = ix('threshold_bits'), ix('technique')
        i_flags, i_mode, i_proc, i_batch = ix('flags'), ix('mode'), \
            ix('proc'), ix('batch')
        i_metric = [(m, col[m]) for m in TIMING_METRICS]
        i_wall = col['wall_ns']
        for row in reader:
            if len(row) != width:
                malformed += 1
                continue
            row.append('')      # missing optional columns read as ''
            rows += 1
            try:
                kind = row[i_kind]
                if run.run_id is None and row[i_run]:
                    run.run_id = row[i_run]
                if kind in ('keygen', 'prepare'):
                    flags = row[i_flags] or env_flags or 'unknown'
                    run.flag_values.add(flags)
                    if run.flags is None:
                        run.flags = flags
                    fk = (fname, row[i_mode] or 'latency', row[i_proc])
                    run.setup.setdefault(
                        fk + (row[i_scheme], int(row[i_logn]), kind),
                        set()).add(int(row[i_key]))
                    continue
                raw = (row[i_scheme], row[i_var], row[i_thr], row[i_logn],
                       row[i_acct], row[i_flags], row[i_mode], row[i_proc])
                ent = cache.get(raw)
                if ent is None:
                    flags = row[i_flags] or env_flags or 'unknown'
                    run.flag_values.add(flags)
                    if run.flags is None:
                        run.flags = flags
                    logn = int(row[i_logn])
                    cfg = (row[i_scheme], row[i_var], int(row[i_thr]), logn)
                    acct = row[i_acct]
                    key = (flags, row[i_mode] or 'latency') + cfg + (acct,)
                    fk = (fname, row[i_mode] or 'latency', row[i_proc])
                    cell = run.cells.setdefault(key, Cell())
                    ent = cache[raw] = (cell, (cfg, acct), fk, logn,
                                        cell.copies.setdefault(
                                            row[i_proc], [0, 0, 0]))
                cell, ca, fk, logn, copy = ent
                if kind == 'warmup':
                    cell.warmups += 1
                    if row[i_ok] != '1':
                        cell.warmup_failures += 1
                    w = run.file_warmups.setdefault(fk, {})
                    w[ca] = w.get(ca, 0) + 1
                    continue
                counts = run.file_counts.setdefault(fk, {})
                counts[ca] = counts.get(ca, 0) + 1
                run.file_keys.setdefault(
                    fk + (row[i_scheme], logn), set()).add(int(row[i_key]))
                if kind == 'sign_failed':
                    cell.sign_failures += 1
                    continue
                if kind != 'sign':
                    continue
                if row[i_ok] != '1':
                    cell.verify_failures += 1
                for m, i in i_metric:
                    v = row[i]
                    if v == '':
                        cell.missing[m] += 1
                    else:
                        cell.add(m, int(v))
                att, rng = int(row[i_att]), int(row[i_rng])
                cell.n += 1
                cell.attempts_sum += att
                cell.rng_bytes_sum += rng
                copy[0] += 1
                copy[1] += att
                copy[2] += rng
                cell.sig_bytes.add(int(row[i_sig]))
                if row[i_bits]:
                    cell.threshold_bits.add(int(row[i_bits]))
                if row[i_tech]:
                    cell.technique.add(row[i_tech])
                if row[i_batch]:
                    cell.batches.add(int(row[i_batch]))
                cell.keys.add(int(row[i_key]))
                if row[i_wall] != '':
                    w = int(row[i_wall])
                    pc = cell.procs.setdefault(row[i_proc], [0, 0, 0])
                    pc[0] += 1
                    pc[1] += w
                    pc[2] += w * w
            except ValueError:
                malformed += 1
    if rows == 0:
        run.problems.append('%s has no rows' % fname)
    if malformed:
        run.problems.append('%s: %d malformed rows (wrong column count or'
                            ' unreadable values; truncated?)'
                            % (fname, malformed))


def read_summary(path, cols):
    """(scheme, variant, logn) -> {col: value} of a committed summary."""
    out = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, newline='') as f:
        for row in csv.DictReader(f):
            out[(row['scheme'], row['variant'], int(row['logn']))] = {
                c: row.get(c, '') for c in cols}
    return out


# ----------------------------------------------------------------------
# Statistics

def quantile(sorted_xs, q):
    """Linear-interpolation quantile of sorted values."""
    n = len(sorted_xs)
    if n == 0:
        return None
    pos = q * (n - 1)
    lo = int(math.floor(pos))
    hi = min(lo + 1, n - 1)
    return sorted_xs[lo] + (sorted_xs[hi] - sorted_xs[lo]) * (pos - lo)


def describe(xs):
    s = sorted(xs)
    return {'n': len(s), 'median': quantile(s, 0.5), 'q1': quantile(s, 0.25),
            'q3': quantile(s, 0.75),
            'mean': statistics.fmean(s) if s else None}


def boot_median(sorted_xs, rng):
    """One bootstrap replicate of the median of sorted_xs (exact)."""
    n = len(sorted_xs)
    if n == 1:
        return sorted_xs[0]
    if n % 2 == 1:
        k = (n + 1) // 2
        u = rng.betavariate(k, n + 1 - k)
        return sorted_xs[min(int(u * n), n - 1)]
    k = n // 2
    u1 = rng.betavariate(k, n + 1 - k)
    v = rng.random()
    u2 = u1 + (1.0 - u1) * (1.0 - v ** (1.0 / (n - k)))
    return (sorted_xs[min(int(u1 * n), n - 1)]
            + sorted_xs[min(int(u2 * n), n - 1)]) / 2.0


def percentile_ci(reps, level=0.95):
    s = sorted(reps)
    a = (1.0 - level) / 2.0
    return quantile(s, a), quantile(s, 1.0 - a)


def pair_seed(seed, *parts):
    return random.Random('%s:%s' % (seed, ':'.join(str(p) for p in parts)))


def ratio_ci_median(runs_a, runs_b, B, rng):
    """Bootstrap CI of est(a) / est(b), est = mean of per-run medians;
    runs_a[i] and runs_b[i] are the sorted values of run i."""
    R = len(runs_a)
    reps = []
    for _ in range(B):
        idx = [rng.randrange(R) for _ in range(R)] if R > 1 else [0]
        ea = statistics.fmean(boot_median(runs_a[i], rng) for i in idx)
        eb = statistics.fmean(boot_median(runs_b[i], rng) for i in idx)
        reps.append(ea / eb)
    return percentile_ci(reps)


def throughput_of(procs):
    """Signatures per second per core of one run: mean over processes of
    count / sum(wall_ns)."""
    vals = [c / w * 1e9 for c, w, _ in procs.values() if w > 0]
    return statistics.fmean(vals) if vals else None


def _boot_throughput(procs, rng):
    vals = []
    for c, w, w2 in procs.values():
        mean = w / c
        var = max(w2 / c - mean * mean, 0.0)
        mw = rng.gauss(mean, math.sqrt(var / c)) if c > 1 else mean
        vals.append(1e9 / mw if mw > 0 else 0.0)
    return statistics.fmean(vals)


def ratio_ci_throughput(runs_a, runs_b, B, rng):
    R = len(runs_a)
    reps = []
    for _ in range(B):
        idx = [rng.randrange(R) for _ in range(R)] if R > 1 else [0]
        ea = statistics.fmean(_boot_throughput(runs_a[i], rng) for i in idx)
        eb = statistics.fmean(_boot_throughput(runs_b[i], rng) for i in idx)
        reps.append(ea / eb)
    return percentile_ci(reps)


class Key:
    """Aggregated statistics of one key over runs."""

    def __init__(self, key):
        self.key = key
        (self.flags, self.mode, self.scheme, self.variant, self.threads,
         self.logn, self.accounting) = key
        self.cells = {}     # run_id -> Cell

    def run_ids(self):
        return sorted(self.cells)

    def available(self, metric):
        return all(c.missing[metric] == 0 and c.values[metric]
                   for c in self.cells.values())

    def primary(self):
        return 'core_cycles' if self.available('core_cycles') else 'tsc_cycles'

    def run_values(self, metric, run_ids=None):
        ids = run_ids if run_ids is not None else self.run_ids()
        return [self.cells[r].values[metric] for r in ids]   # sorted

    def estimate(self, metric, run_ids=None):
        if not self.available(metric):
            return None
        meds = [quantile(v, 0.5) for v in self.run_values(metric, run_ids)]
        return statistics.fmean(meds) if meds else None

    def spread(self, metric):
        if not self.available(metric):
            return None
        meds = [quantile(v, 0.5) for v in self.run_values(metric)]
        if len(meds) < 2:
            return None
        est = statistics.fmean(meds)
        return (max(meds) - min(meds)) / est if est else None

    def pooled(self, metric):
        if not self.available(metric):
            return None
        xs = []
        for c in self.cells.values():
            xs.extend(c.values[metric])
        return describe(xs)

    def signatures(self):
        return sum(c.count() for c in self.cells.values())

    def attempts_per_sig(self):
        n = self.signatures()
        return sum(c.attempts_sum for c in self.cells.values()) / n if n else None

    def rng_bytes_per_sig(self):
        n = self.signatures()
        return sum(c.rng_bytes_sum for c in self.cells.values()) / n if n else None

    def sig_bytes(self):
        s = set()
        for c in self.cells.values():
            s |= c.sig_bytes
        return min(s) if len(s) == 1 else None

    def threshold_bits(self):
        s = set()
        for c in self.cells.values():
            s |= c.threshold_bits
        return min(s) if len(s) == 1 else None

    def technique(self):
        s = set()
        for c in self.cells.values():
            s |= c.technique
        return min(s) if len(s) == 1 else None

    def failures(self):
        return sum(c.sign_failures + c.verify_failures + c.warmup_failures
                   for c in self.cells.values())

    def warmups(self):
        return sum(c.warmups for c in self.cells.values())

    def procs(self):
        return max((len(c.procs) for c in self.cells.values()), default=0)

    def throughput(self, run_ids=None):
        ids = run_ids if run_ids is not None else self.run_ids()
        vals = [throughput_of(self.cells[r].procs) for r in ids]
        vals = [v for v in vals if v is not None]
        return statistics.fmean(vals) if vals else None


def aggregate(runs):
    keys = {}
    for run in runs:
        for k, cell in run.cells.items():
            agg = keys.setdefault(k, Key(k))
            rid = run.run_id
            while rid in agg.cells:
                rid += "'"
            agg.cells[rid] = cell
    return keys


def pairs_for(keys, pairs, context=False):
    """Matched (E8 key, HAWK key, fair) triples."""
    out = []
    for k, e in keys.items():
        if e.scheme != 'e8':
            continue
        for ev, hv in pairs:
            if e.variant != ev:
                continue
            hk = (e.flags, e.mode, 'hawk', hv, e.threads, e.logn, e.accounting)
            if hk not in keys:
                continue
            h = keys[hk]
            fair = (not context and e.threshold_bits() == h.threshold_bits()
                    and e.technique() == h.technique())
            out.append((e, h, fair))
    return out


def ratio_rows(keys, B, seed):
    rows = []
    for plist, context in ((PAIRS, False), (CONTEXT_PAIRS, True)):
        for e, h, fair in pairs_for(keys, plist, context):
            common = sorted(set(e.run_ids()) & set(h.run_ids()))
            if not common:
                continue
            if e.mode == 'throughput':
                metrics = ('sigs_per_s_per_core',)
            else:
                metrics = TIMING_METRICS
            for m in metrics:
                rng = pair_seed(seed, e.key, h.variant, m)
                if m == 'sigs_per_s_per_core':
                    ea, ha = e.throughput(common), h.throughput(common)
                    if not ea or not ha:
                        continue
                    lo, hi = ratio_ci_throughput(
                        [e.cells[r].procs for r in common],
                        [h.cells[r].procs for r in common], B, rng)
                else:
                    if not (e.available(m) and h.available(m)):
                        continue
                    ea, ha = e.estimate(m, common), h.estimate(m, common)
                    lo, hi = ratio_ci_median(e.run_values(m, common),
                                             h.run_values(m, common), B, rng)
                rows.append({
                    'flags': e.flags, 'mode': e.mode, 'threads': e.threads,
                    'logn': e.logn, 'accounting': e.accounting,
                    'e8_variant': e.variant, 'hawk_variant': h.variant,
                    'pair': 'context' if context else (
                        'fair' if fair else 'unmatched'),
                    'metric': m, 'runs': len(common),
                    'e8': ea, 'hawk': ha, 'ratio': ea / ha,
                    'ci_lo': lo, 'ci_hi': hi})
    rows.sort(key=lambda r: (r['flags'], r['mode'], r['pair'],
                             r['e8_variant'], r['hawk_variant'], r['threads'],
                             r['logn'], r['accounting'], r['metric']))
    return rows


# ----------------------------------------------------------------------
# Acceptance checks (plan.md Step 9)

class Check:
    def __init__(self, name, status, detail):
        self.name, self.status, self.detail = name, status, detail


def check_failures(keys):
    bad = [(k, a.failures()) for k, a in keys.items() if a.failures()]
    total = sum(a.signatures() for a in keys.values())
    if bad:
        detail = '; '.join('%s: %d' % (fmt_key(k), n) for k, n in bad[:10])
        return Check('verify', 'FAIL', '%d keys with signing or verification'
                     ' failures (%s)' % (len(bad), detail))
    return Check('verify', 'PASS', '%d signatures (and every warm-up)'
                 ' signed and verified, 0 failures' % total)


def check_complete(runs):
    problems = []
    for run in runs:
        problems.extend('%s: %s' % (run.run_id, p) for p in run.problems)
        for fk, counts in run.file_counts.items():
            fname, mode, proc = fk
            where = '%s %s/%s%s' % (run.run_id, fname, mode,
                                    ' proc ' + proc if proc else '')
            vals = set(counts.values())
            if len(vals) != 1:
                problems.append('%s: unequal signature counts %s'
                                % (where, sorted(vals)))
            cfgs = {c for c, _ in counts}
            for cfg in cfgs:
                for acct in ('all_in', 'prepared'):
                    if (cfg, acct) not in counts:
                        problems.append('%s: %s missing %s'
                                        % (where, cfg, acct))
            warm = run.file_warmups.get(fk, {})
            for ca in counts:
                if ca not in warm:
                    problems.append('%s: %s has no warm-up rows'
                                    % (where, ca))
            # Grid: every variant of a scheme at every threads and logn
            # seen (serial-only HAWK variants at T = 1 only).
            schemes = {(c[0], c[1]) for c in cfgs}
            threads = {c[2] for c in cfgs}
            logns = {c[3] for c in cfgs}
            for s, v in schemes:
                for t in threads:
                    if v in SERIAL_ONLY and t != 1:
                        continue
                    for l in logns:
                        if (s, v, t, l) not in cfgs:
                            problems.append('%s: configuration %s missing'
                                            % (where, (s, v, t, l)))
            # Keys: keygen and prepare rows for every key signed with
            # (written by the parent process in throughput mode).
            for (f2, m2, p2, s, l), kidx in run.file_keys.items():
                if (f2, m2, p2) != fk:
                    continue
                for kind in ('keygen', 'prepare'):
                    have = set()
                    for (f3, m3, _p, s3, l3, k3), ks in run.setup.items():
                        if (f3, m3, s3, l3, k3) == (fname, mode, s, l, kind):
                            have |= ks
                    if not kidx <= have:
                        problems.append('%s: %s rows missing for %s n=%d'
                                        % (where, kind, s, 1 << l))
        for f in run.flag_values:
            if run.env.get('flags') and f != run.env['flags']:
                problems.append('%s: rows with flags %s, env says %s'
                                % (run.run_id, f, run.env['flags']))
    if problems:
        return Check('complete', 'FAIL', '%d problems: %s'
                     % (len(problems), '; '.join(problems[:8])))
    nfiles = sum(len(r.file_counts) for r in runs)
    return Check('complete', 'PASS', '%d run(s), %d CSV parts: every'
                 ' configuration with both accounting modes, equal counts,'
                 ' warm-up, keygen and prepare rows' % (len(runs), nfiles))


def check_preflight(runs):
    """The preflight result run_compare.sh recorded in env.txt: PASS if
    every run passed it; FAIL if a non-WSL run was forced past it; SKIP
    for smoke runs, WSL runs and runs written before the preflight."""
    bad, skip = [], []
    for run in runs:
        pre = run.env.get('preflight')
        failed = sorted(k[len('preflight_'):] for k, v in run.env.items()
                        if k.startswith('preflight_') and not v.startswith('ok'))
        if pre is None:
            skip.append('%s: no preflight record' % run.run_id)
        elif pre == 'passed':
            continue
        elif 'smoke' in pre or run.env.get('wsl') == 'yes':
            skip.append('%s: %s on %s (%s)' % (
                run.run_id, pre, 'WSL' if run.env.get('wsl') == 'yes'
                else 'a smoke run', ', '.join(failed)))
        else:
            bad.append('%s: %s (%s)' % (run.run_id, pre, ', '.join(failed)))
    if bad:
        return Check('preflight', 'FAIL', '%d run(s) not passing the'
                     ' preflight: %s' % (len(bad), '; '.join(bad[:6])))
    if skip:
        return Check('preflight', 'SKIP', '%d of %d run(s) without a passed'
                     ' preflight, not enforced: %s' % (
                         len(skip), len(runs), '; '.join(skip[:3])))
    return Check('preflight', 'PASS', 'every run passed the preflight (boost'
                 ' off, performance governor, SMT off, own cores, perf,'
                 ' equal frequencies, clean tree)')


def check_attempts(keys, validation):
    """Every run and every throughput process signs the same messages
    with the same signing-RNG streams, so their signatures, attempts and
    rng_bytes are identical copies: the copies must agree (n, attempts and
    rng_bytes sums per run and process), and one copy is compared with
    Step 6 (counting the copies as independent would shrink the 5 sigma
    tolerance by the square root of their number)."""
    bad, differ, checked, skipped = [], [], 0, 0
    for k, a in keys.items():
        copies = {tuple(c) for cell in a.cells.values()
                  for c in cell.copies.values()}
        if len(copies) > 1:
            differ.append('%s: %d distinct (n, attempts, rng_bytes) over %d'
                          ' copies' % (fmt_key(k), len(copies), sum(
                              len(c.copies) for c in a.cells.values())))
            continue
        v = validation.get((a.scheme, join_variant(a.variant), a.logn))
        n, att = next(iter(copies))[:2] if copies else (0, 0)
        if not v or not v.get('attempts_per_sig') or n == 0:
            skipped += 1
            continue
        mu = float(v['attempts_per_sig'])
        nval = int(v['signatures'] or 1000000)
        var = max(mu * mu - mu, 0.0)
        extra = att - n
        expect = n * (mu - 1.0)
        tol = 5.0 * math.sqrt(n * var) + 5.0 * n * math.sqrt(var / nval) + 2.0
        checked += 1
        if abs(extra - expect) > tol:
            bad.append('%s: %.4f vs %.6f' % (fmt_key(k), att / n, mu))
    if differ:
        return Check('attempts', 'FAIL', '%d keys whose runs or processes,'
                     ' which sign the same messages with the same seeds,'
                     ' disagree (%s)' % (len(differ), '; '.join(differ[:4])))
    if bad:
        return Check('attempts', 'FAIL', '%d of %d keys outside 5 sigma of'
                     ' Step 6 (%s)' % (len(bad), checked, '; '.join(bad[:6])))
    if checked == 0:
        return Check('attempts', 'SKIP', 'no validation summary to compare')
    return Check('attempts', 'PASS', '%d keys: every run and process signed'
                 ' the same signatures (identical attempts and rng_bytes),'
                 ' attempts per signature within 5 sigma of Step 6\'s (HAWK'
                 ' n = 256 about 1.1796, the rest about 1.0000)%s'
                 % (checked, ', %d without a Step 6 row' % skipped
                    if skipped else ''))


def check_spread(keys, tol):
    multi = [a for a in keys.values() if len(a.cells) >= 2]
    if not multi:
        return Check('run_spread', 'SKIP', 'one run per key: repeat the run'
                     ' (Step 9: at least 5 process restarts)')
    bad = []
    for a in multi:
        m = a.primary()
        sp = a.spread(m) if a.mode == 'latency' else None
        if a.mode == 'throughput':
            vals = [throughput_of(c.procs) for c in a.cells.values()]
            est = statistics.fmean(vals)
            sp = (max(vals) - min(vals)) / est if est else None
        if sp is not None and sp > tol:
            bad.append((sp, fmt_key(a.key)))
    bad.sort(reverse=True)
    if bad:
        return Check('run_spread', 'FAIL', '%d of %d keys with per-run'
                     ' medians spread above %.1f%% (worst: %s)'
                     % (len(bad), len(multi), 100 * tol, '; '.join(
                         '%s %.1f%%' % (k, 100 * s) for s, k in bad[:5])))
    return Check('run_spread', 'PASS', '%d keys with >= 2 runs: per-run'
                 ' medians within %.1f%%' % (len(multi), 100 * tol))


def check_core_tsc(runs, keys, tol):
    boosts = {r.env.get('boost', 'unavailable') for r in runs}
    if boosts != {'off'}:
        return Check('core_vs_tsc', 'SKIP', 'boost is not off in every run'
                     ' (%s); core cycles and TSC need not agree'
                     % ', '.join(sorted(boosts)))
    bad, n = [], 0
    for run in runs:
        ratios = []
        for k, cell in run.cells.items():
            if k[1] != 'latency' or k[4] != 1:
                continue
            if cell.missing['core_cycles'] or not cell.values['core_cycles']:
                continue
            c = quantile(sorted(cell.values['core_cycles']), 0.5)
            t = quantile(sorted(cell.values['tsc_cycles']), 0.5)
            ratios.append(c / t)
        if len(ratios) < 2:
            continue
        n += 1
        mean = statistics.fmean(ratios)
        sp = (max(ratios) - min(ratios)) / mean
        if sp > tol:
            bad.append('%s: core/TSC %.4f..%.4f' % (run.run_id, min(ratios),
                                                    max(ratios)))
    if n == 0:
        return Check('core_vs_tsc', 'SKIP', 'no serial latency rows with'
                     ' core cycles')
    if bad:
        return Check('core_vs_tsc', 'FAIL', '; '.join(bad))
    return Check('core_vs_tsc', 'PASS', 'serial core/TSC ratios within'
                 ' %.1f%% in each of %d runs' % (100 * tol, n))


def check_joins(keys, precision, validation):
    miss_p, miss_v, mism = [], [], []
    for k, a in keys.items():
        jk = (a.scheme, join_variant(a.variant), a.logn)
        p = precision.get(jk)
        if p is None:
            miss_p.append(fmt_key(k))
        elif a.threshold_bits() is not None and str(a.threshold_bits()) != \
                p['threshold_bits']:
            mism.append('%s: %s vs %s' % (fmt_key(k), a.threshold_bits(),
                                          p['threshold_bits']))
        if jk not in validation:
            miss_v.append(fmt_key(k))
    if miss_p or miss_v or mism:
        return Check('joins', 'FAIL', 'precision rows missing: %d, validation'
                     ' rows missing: %d, threshold_bits mismatches: %d (%s)'
                     % (len(miss_p), len(miss_v), len(mism),
                        '; '.join((miss_p + miss_v + mism)[:6])))
    return Check('joins', 'PASS', 'every key has its precision_summary and'
                 ' validation_summary row; threshold_bits agree')


def fmt_key(k):
    flags, mode, scheme, variant, threads, logn, acct = k
    return '%s/%s %s %s T=%d n=%d %s' % (flags, mode, scheme, variant,
                                          threads, 1 << logn, acct)
