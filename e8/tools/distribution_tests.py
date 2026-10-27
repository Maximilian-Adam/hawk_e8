#!/usr/bin/env python3
"""
Distribution tests of every HAWK and HAWK-E8-CM sampler variant (method
and results: e8/docs/validation.md).
Python standard library only.

Input: histograms written by e8_distribution_sample (rows kind, scheme,
variant, logn, id, value, count; repeated keys are summed). References:
the exact implemented distributions of the committed tables, from
precision_audit.load() (Fractions with power-of-two denominators), not
the ideal Gaussians: the tests detect implementation errors (wrong table,
comparison, mapping or stream layout), not the table approximation, whose
distance from the ideal (e8/docs/precision_audit.md) is far below what any feasible sample
resolves.

Tests, the same for both schemes:

  1d          one 1D table: HAWK parity (values x), E8 centre class
              (product coordinates); HAWK ref-T against HAWK ref's table;
  component   E8 component given tau;
  blocknorm   E8 block squared norm ||P z||^2 given tau, against the exact
              distribution of sum_v 2 (4 x_v - k_v)^2 (x_v from the class of
              coordinate v, centre k_v / 4), mixed over the components:
              computed by exact convolution;
  blockcoord  E8 product coordinates of all sampled blocks, by centre
              class, against the class's 1D table;
  signorm     the squared norm of full samples for one fixed target:
              sample mean and variance against the exact implemented
              mean and variance (sums of the per-coordinate or per-block
              cumulants), two z-tests.

Goodness of fit: values in increasing order (components by index) are
pooled into adjacent cells, each closed once its expected count reaches 5
(a trailing remainder joins the last cell); Pearson's chi-square and the
G-test on the pooled cells, df = cells - 1, p = Q(df/2, statistic/2)
(regularised upper incomplete gamma function). An observed value outside
the reference support is an impossible outcome: the test fails with p = 0.

Multiple testing: Holm-Bonferroni over the whole family (every
chi-square, G-test and z-test) at family-wise level alpha (default 1e-3);
every test must pass.

Negative controls (their own Holm family at the same alpha): the same
samples against a deliberately wrong reference, the same rules for both
schemes:

  1d, blockcoord, blocknorm,  the same table, tau or target at the
  signorm                     neighbouring logn (8 -> 9, 9 -> 10, 10 -> 9;
                              signorm: the exact mean of the same target
                              under the neighbouring logn's tables);
  1d, component               the reference with mass m moved from its
                              most probable outcome to the second (a
                              threshold moved), m = 12 / sqrt(N (1/p_1 +
                              1/p_2)), noncentrality about 144.

(The E8 component masses are within 1/16 +- 3.5e-4 at these sigma_sign,
so a permutation of the components is not a usable control: it changes
the distribution too little to be seen, and the coset mapping is checked
by verification and the debug build's coset and parity checks instead.)

A control is eligible when its noncentrality (N sum (p - q)^2 / q over its
cells, p the true and q the wrong reference; for signorm the squared
z-score of the true mean) is at least 100; every eligible control must be
rejected. The same samples are also tested against the ideal Gaussians
(1d, component, blockcoord), for information only.

Usage (normally through tools/validate.py):
    distribution_tests.py --hist FILE [--out-dir DIR] [--alpha A]

Writes tests.csv, controls.csv, ideal.csv and signorm.csv in --out-dir
(default e8/results/distribution-tests).
"""

import argparse
import csv
import math
import os
import sys
from fractions import Fraction

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import precision_audit as pa  # noqa: E402

ALPHA_DEFAULT = 1e-3
MIN_EXPECTED = 5.0
CONTROL_LAMBDA = 100.0
NEIGHBOUR = {8: 9, 9: 10, 10: 9}
HAWK_VARIANTS = ("ref", "ref-T", "hot64", "hot32")
E8_VARIANTS = ("ref", "hot64", "hot32")

# ---------------------------------------------------------------------------
# p-values


def gamma_q(a, x):
    """Regularised upper incomplete gamma Q(a, x), a > 0, x >= 0."""
    if x <= 0:
        return 1.0
    lead = a * math.log(x) - x - math.lgamma(a)
    if x < a + 1:
        # Series for P(a, x).
        term = 1.0 / a
        total = term
        k = a
        for _ in range(100000):
            k += 1
            term *= x / k
            total += term
            if abs(term) < abs(total) * 1e-17:
                break
        return max(0.0, 1.0 - total * math.exp(lead))
    # Continued fraction for Q(a, x) (modified Lentz).
    tiny = 1e-300
    b = x + 1 - a
    c = 1 / tiny
    d = 1 / b
    h = d
    for i in range(1, 100000):
        an = -i * (i - a)
        b += 2
        d = an * d + b
        d = tiny if abs(d) < tiny else d
        c = b + an / c
        c = tiny if abs(c) < tiny else c
        d = 1 / d
        delta = d * c
        h *= delta
        if abs(delta - 1) < 1e-16:
            break
    return math.exp(lead) * h


def chi2_sf(stat, df):
    return gamma_q(df / 2.0, stat / 2.0)


def normal_two_sided(z):
    return math.erfc(abs(z) / math.sqrt(2.0))


def holm(pvals):
    """Holm-Bonferroni adjusted p-values, in input order."""
    m = len(pvals)
    order = sorted(range(m), key=lambda i: pvals[i])
    adj = [0.0] * m
    running = 0.0
    for rank, i in enumerate(order):
        running = max(running, min(1.0, (m - rank) * pvals[i]))
        adj[i] = running
    return adj


# ---------------------------------------------------------------------------
# Goodness of fit


def pool(ref, obs, n):
    """
    Cells (expected, observed) over the sorted union of the reference
    support and the observed values; returns (cells, impossible), the
    latter the observed count outside the support.
    """
    keys = sorted(set(k for k, p in ref.items() if p > 0) | set(obs))
    impossible = sum(c for k, c in obs.items() if ref.get(k, 0) <= 0)
    cells = []
    e_acc = 0.0
    o_acc = 0
    for k in keys:
        p = ref.get(k, 0)
        if p <= 0:
            continue
        e_acc += n * float(p)
        o_acc += obs.get(k, 0)
        if e_acc >= MIN_EXPECTED:
            cells.append([e_acc, o_acc])
            e_acc = 0.0
            o_acc = 0
    if e_acc > 0 or o_acc > 0:
        if cells:
            cells[-1][0] += e_acc
            cells[-1][1] += o_acc
        else:
            cells.append([e_acc, o_acc])
    return cells, impossible


def gof(ref, obs):
    """(chi2, g, df, p_chi2, p_g, cells, impossible, n)."""
    n = sum(obs.values())
    cells, impossible = pool(ref, obs, n)
    df = len(cells) - 1
    if impossible:
        return (math.inf, math.inf, df, 0.0, 0.0, len(cells), impossible, n)
    chi2 = sum((o - e) ** 2 / e for e, o in cells)
    g = 2.0 * sum(o * math.log(o / e) for e, o in cells if o > 0)
    if df < 1:
        return (chi2, g, df, 1.0, 1.0, len(cells), 0, n)
    return (chi2, g, df, chi2_sf(chi2, df), chi2_sf(g, df), len(cells), 0,
            n)


def noncentrality(true, wrong, n):
    """
    N sum (p - q)^2 / q over the cells that the test pools under the wrong
    reference q, p the true reference; infinite if the true distribution
    puts an expected count of at least 5 where q is 0 (the test then sees
    impossible outcomes).
    """
    keys = sorted(set(k for k, q in wrong.items() if q > 0)
                  | set(k for k, p in true.items() if p > 0))
    outside = 0.0
    cells = []
    e_acc = t_acc = 0.0
    for k in keys:
        q = float(wrong.get(k, 0))
        p = float(true.get(k, 0))
        if q <= 0:
            outside += n * p
            continue
        e_acc += n * q
        t_acc += n * p
        if e_acc >= MIN_EXPECTED:
            cells.append((e_acc, t_acc))
            e_acc = t_acc = 0.0
    if cells and (e_acc > 0 or t_acc > 0):
        e, t = cells.pop()
        cells.append((e + e_acc, t + t_acc))
    if outside >= MIN_EXPECTED:
        return math.inf
    return sum((t - e) ** 2 / e for e, t in cells)


# ---------------------------------------------------------------------------
# Exact references


class References:
    def __init__(self, hawk_src):
        self.configs, self.geom, _, _ = pa.load(hawk_src)
        self._blocknorm = {}
        self._coordnorm = {}
        self._conv = {}

    def cfg(self, scheme, variant, logn):
        if scheme == "hawk" and variant == "ref-T":
            variant = "ref"
        return self.configs[(scheme, variant, logn)]

    def onedim(self, scheme, variant, logn, ident, ideal=False):
        c = self.cfg(scheme, variant, logn)
        pair = c.pairs[ident] if scheme == "hawk" else c.classes[ident]
        return pair.ideal if ideal else pair.impl

    def component(self, variant, logn, tau, ideal=False):
        pair = self.cfg("e8", variant, logn).comps[tau]
        return pair.ideal if ideal else pair.impl

    # Block norms: sum over the eight coordinates of
    # (32 x - num32)^2 / 32 = 2 (4 x - k)^2, num32 = 8 k.

    def _coord_norm(self, variant, logn, cls):
        key = (variant, logn, cls)
        if key not in self._coordnorm:
            num32 = self.geom.centers[cls]
            impl = self.cfg("e8", variant, logn).classes[cls].impl
            e = max(p.denominator.bit_length() - 1 for p in impl.values())
            dist = {}
            for x, p in impl.items():
                w = (32 * x - num32) ** 2 // 32
                dist[w] = dist.get(w, 0) + p.numerator * (
                    (1 << e) // p.denominator)
            self._coordnorm[key] = (dist, e)
        return self._coordnorm[key]

    def _conv_classes(self, variant, logn, classes):
        """Exact distribution of the sum for a sorted class tuple: (dict, e)."""
        key = (variant, logn, classes)
        if key in self._conv:
            return self._conv[key]
        if len(classes) == 1:
            out = self._coord_norm(variant, logn, classes[0])
        else:
            a, ea = self._conv_classes(variant, logn, classes[:-1])
            b, eb = self._coord_norm(variant, logn, classes[-1])
            out_d = {}
            for wa, p_a in a.items():
                for wb, p_b in b.items():
                    w = wa + wb
                    out_d[w] = out_d.get(w, 0) + p_a * p_b
            out = (out_d, ea + eb)
        self._conv[key] = out
        return out

    def blocknorm(self, variant, logn, tau):
        """Exact block norm distribution given tau: value -> Fraction."""
        key = (variant, logn, tau)
        if key in self._blocknorm:
            return self._blocknorm[key]
        comp = self.component(variant, logn, tau)
        parts = []
        e_max = 0
        for c, pc in comp.items():
            classes = tuple(sorted(self.geom.coord_class[tau][c]))
            d, e = self._conv_classes(variant, logn, classes)
            ec = pc.denominator.bit_length() - 1
            parts.append((d, e + ec, pc.numerator))
            e_max = max(e_max, e + ec)
        out = {}
        for d, e, num in parts:
            scale = num << (e_max - e)
            for w, p in d.items():
                out[w] = out.get(w, 0) + p * scale
        total = sum(out.values())
        if total != 1 << e_max:
            raise ValueError("block norm distribution does not sum to 1")
        res = {w: Fraction(p, 1 << e_max) for w, p in out.items()}
        self._blocknorm[key] = res
        return res

    def unit_cumulants(self, scheme, variant, logn, ident):
        """Exact cumulants 1..4 of x^2 (HAWK, parity) or the block norm (E8, tau)."""
        if scheme == "hawk":
            dist = {}
            for x, p in self.onedim(scheme, variant, logn, ident).items():
                dist[x * x] = dist.get(x * x, Fraction(0)) + p
        else:
            dist = self.blocknorm(variant, logn, ident)
        m = [sum((p * Fraction(w) ** k for w, p in dist.items()),
                 Fraction(0)) for k in range(5)]
        k1 = m[1]
        c2 = m[2] - k1 ** 2
        c3 = m[3] - 3 * k1 * m[2] + 2 * k1 ** 3
        c4m = (m[4] - 4 * k1 * m[3] + 6 * k1 ** 2 * m[2] - 3 * k1 ** 4)
        k4 = c4m - 3 * c2 ** 2
        return (k1, c2, c3, k4)


def float_ref(ref):
    return {k: float(v) for k, v in ref.items()}


# ---------------------------------------------------------------------------
# The tests


def read_hist(paths):
    hist = {}
    for path in paths:
        with open(path) as f:
            for row in csv.DictReader(f):
                key = (row["kind"], row["scheme"], row["variant"],
                       int(row["logn"]), int(row["id"]))
                d = hist.setdefault(key, {})
                v = int(row["value"])
                d[v] = d.get(v, 0) + int(row["count"])
    return hist


def moved_threshold(ref, n):
    """The reference with mass m moved from its first to its second value."""
    order = sorted(ref, key=lambda k: -ref[k])
    a, b = order[0], order[1]
    p_a, p_b = float(ref[a]), float(ref[b])
    m = 12.0 / math.sqrt(n * (1.0 / p_a + 1.0 / p_b))
    out = float_ref(ref)
    out[a] -= m
    out[b] += m
    return out, m / p_a


def signorm_stats(obs):
    n = sum(obs.values())
    s1 = sum(v * c for v, c in obs.items())
    s2 = sum(v * v * c for v, c in obs.items())
    mean = Fraction(s1, n)
    var = Fraction(s2, n) - mean * mean
    return n, mean, var * n / (n - 1)


def run_tests(refs, hist, alpha):
    tests = []          # dicts
    controls = []
    ideal = []

    def add_gof(lst, family, kind, scheme, variant, logn, ident, ref, obs,
                extra=None):
        chi2, g, df, p1, p2, cells, imp, n = gof(ref, obs)
        for stat, val, p in (("chi2", chi2, p1), ("g", g, p2)):
            row = dict(family=family, kind=kind, scheme=scheme,
                       variant=variant, logn=logn, id=ident, test=stat,
                       n=n, cells=cells, df=df, statistic=val, p=p,
                       impossible=imp)
            if extra:
                row.update(extra)
            lst.append(row)

    signorm_keys = []
    for key in sorted(hist):
        kind, scheme, variant, logn, ident = key
        obs = hist[key]
        n = sum(obs.values())
        if kind == "1d":
            ref = refs.onedim(scheme, variant, logn, ident)
            add_gof(tests, "main", kind, scheme, variant, logn, ident, ref,
                    obs)
            nb = refs.onedim(scheme, variant, NEIGHBOUR[logn], ident)
            add_gof(controls, "neighbour_logn", kind, scheme, variant, logn,
                    ident, nb, obs,
                    dict(noncentrality=noncentrality(ref, nb, n)))
            mv, rel = moved_threshold(ref, n)
            add_gof(controls, "moved_threshold", kind, scheme, variant,
                    logn, ident, mv, obs,
                    dict(noncentrality=noncentrality(float_ref(ref), mv, n),
                         moved_rel=rel))
            add_gof(ideal, "ideal", kind, scheme, variant, logn, ident,
                    refs.onedim(scheme, variant, logn, ident, True), obs)
        elif kind == "component":
            ref = refs.component(variant, logn, ident)
            add_gof(tests, "main", kind, scheme, variant, logn, ident, ref,
                    obs)
            mv, rel = moved_threshold(ref, n)
            add_gof(controls, "moved_threshold", kind, scheme, variant,
                    logn, ident, mv, obs,
                    dict(noncentrality=noncentrality(float_ref(ref), mv, n),
                         moved_rel=rel))
            add_gof(ideal, "ideal", kind, scheme, variant, logn, ident,
                    refs.component(variant, logn, ident, True), obs)
        elif kind == "blocknorm":
            ref = refs.blocknorm(variant, logn, ident)
            add_gof(tests, "main", kind, scheme, variant, logn, ident, ref,
                    obs)
            nb = refs.blocknorm(variant, NEIGHBOUR[logn], ident)
            add_gof(controls, "neighbour_logn", kind, scheme, variant, logn,
                    ident, nb, obs,
                    dict(noncentrality=noncentrality(ref, nb, n)))
        elif kind == "blockcoord":
            ref = refs.onedim("e8", variant, logn, ident)
            add_gof(tests, "main", kind, scheme, variant, logn, ident, ref,
                    obs)
            nb = refs.onedim("e8", variant, NEIGHBOUR[logn], ident)
            add_gof(controls, "neighbour_logn", kind, scheme, variant, logn,
                    ident, nb, obs,
                    dict(noncentrality=noncentrality(ref, nb, n)))
            add_gof(ideal, "ideal", kind, scheme, variant, logn, ident,
                    refs.onedim("e8", variant, logn, ident, True), obs)
        elif kind == "signorm":
            signorm_keys.append(key)
        elif kind != "signorm_target":
            raise ValueError("unknown histogram kind %s" % kind)

    signorm = []
    for key in signorm_keys:
        _, scheme, variant, logn, _ = key
        counts = {}
        for (k2, s2, v2, l2, i2), d in hist.items():
            if k2 == "signorm_target" and (s2, v2, l2) == \
                    (scheme, variant, logn):
                counts[i2] = sum(d.values())
        units = (2 << logn) if scheme == "hawk" else (1 << (logn - 2))
        if sum(counts.values()) != units:
            raise ValueError("signorm target of %s %s logn=%d has %d units,"
                             " not %d" % (scheme, variant, logn,
                                          sum(counts.values()), units))
        cum = [Fraction(0)] * 4
        cum_alt = Fraction(0)
        for ident, cnt in counts.items():
            kk = refs.unit_cumulants(scheme, variant, logn, ident)
            for j in range(4):
                cum[j] += cnt * kk[j]
            cum_alt += cnt * refs.unit_cumulants(scheme, variant,
                                                 NEIGHBOUR[logn], ident)[0]
        n, mean, var = signorm_stats(hist[key])
        se_mean = math.sqrt(float(cum[1]) / n)
        z_mean = float(mean - cum[0]) / se_mean
        se_var = math.sqrt(float(cum[3] + 2 * cum[1] ** 2) / n)
        z_var = float(var - cum[1]) / se_var
        logn_n = 1 << logn
        if scheme == "hawk":
            s = float(pa.gt.HAWK_SIGMA_SIGN[logn])
            mean_c = 2 * logn_n * 4 * s * s
            var_c = 2 * logn_n * 32 * s ** 4
        else:
            s = float(pa.gt.E8_SIGMA_SIGN[logn])
            mean_c = 2 * logn_n * s * s
            var_c = 4 * logn_n * s ** 4
        base = dict(family="main", kind="signorm", scheme=scheme,
                    variant=variant, logn=logn, id=0, n=n, cells="", df="",
                    impossible=0)
        tests.append(dict(base, test="mean_z", statistic=z_mean,
                          p=normal_two_sided(z_mean)))
        tests.append(dict(base, test="var_z", statistic=z_var,
                          p=normal_two_sided(z_var)))
        z_alt = float(mean - cum_alt) / se_mean
        controls.append(dict(base, family="neighbour_logn",
                             test="mean_z", statistic=z_alt,
                             p=normal_two_sided(z_alt),
                             noncentrality=(float(cum[0] - cum_alt)
                                            / se_mean) ** 2))
        signorm.append(dict(scheme=scheme, variant=variant, logn=logn, n=n,
                            mean=float(mean), mean_exact=float(cum[0]),
                            var=float(var), var_exact=float(cum[1]),
                            z_mean=z_mean, z_var=z_var,
                            mean_continuous=mean_c, var_continuous=var_c,
                            mean_rel_continuous=float(mean) / mean_c - 1,
                            var_rel_continuous=float(var) / var_c - 1))

    for lst in (tests, controls, ideal):
        adj = holm([r["p"] for r in lst])
        for r, a in zip(lst, adj):
            r["p_holm"] = a
            r["rejected"] = int(a <= alpha)
    for r in controls:
        r["eligible"] = int(r["noncentrality"] >= CONTROL_LAMBDA)
    return tests, controls, ideal, signorm


def fmt(v):
    if isinstance(v, float):
        if math.isinf(v):
            return "inf"
        return "%.6g" % v
    return str(v)


def write_rows(path, rows, fields):
    with open(path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(fields)
        for r in rows:
            w.writerow([fmt(r.get(k, "")) for k in fields])


TEST_FIELDS = ["family", "kind", "scheme", "variant", "logn", "id", "test",
               "n", "cells", "df", "statistic", "p", "p_holm", "rejected",
               "impossible"]
CONTROL_FIELDS = TEST_FIELDS + ["noncentrality", "eligible", "moved_rel"]
SIGNORM_FIELDS = ["scheme", "variant", "logn", "n", "mean", "mean_exact",
                  "var", "var_exact", "z_mean", "z_var", "mean_continuous",
                  "var_continuous", "mean_rel_continuous",
                  "var_rel_continuous"]


def summarise(tests, controls, ideal):
    """Per (scheme, variant, logn) distribution-test figures."""
    out = {}
    for r in tests:
        k = (r["scheme"], r["variant"], r["logn"])
        s = out.setdefault(k, dict(tests=0, tests_rejected=0, impossible=0,
                                   min_p=1.0, min_p_holm=1.0, controls=0, controls_eligible=0,
                                   controls_rejected=0,
                                   eligible_not_rejected=0,
                                   ideal_tests=0, ideal_rejected=0))
        s["tests"] += 1
        s["tests_rejected"] += r["rejected"]
        s["impossible"] += r["impossible"]
        s["min_p"] = min(s["min_p"], r["p"])
        s["min_p_holm"] = min(s["min_p_holm"], r["p_holm"])
    for r in controls:
        s = out[(r["scheme"], r["variant"], r["logn"])]
        s["controls"] += 1
        s["controls_eligible"] += r["eligible"]
        s["controls_rejected"] += r["rejected"]
        s["eligible_not_rejected"] += r["eligible"] and not r["rejected"]
    for r in ideal:
        s = out[(r["scheme"], r["variant"], r["logn"])]
        s["ideal_tests"] += 1
        s["ideal_rejected"] += r["rejected"]
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--hist", action="append", required=True)
    ap.add_argument("--out-dir", default=os.path.join(
        HERE, "..", "results", "distribution-tests"))
    ap.add_argument("--alpha", type=float, default=ALPHA_DEFAULT)
    ap.add_argument("--hawk-src", default=os.path.join(
        HERE, "..", "..", "src", "hawk_sign.c"))
    args = ap.parse_args()
    refs = References(args.hawk_src)
    hist = read_hist(args.hist)
    tests, controls, ideal, signorm = run_tests(refs, hist, args.alpha)
    os.makedirs(args.out_dir, exist_ok=True)
    for name, rows, fields in (("tests", tests, TEST_FIELDS),
                               ("controls", controls, CONTROL_FIELDS),
                               ("ideal", ideal, TEST_FIELDS),
                               ("signorm", signorm, SIGNORM_FIELDS)):
        write_rows(os.path.join(args.out_dir, name + ".csv"), rows, fields)
    bad = sum(r["rejected"] for r in tests)
    missed = sum(1 for r in controls if r["eligible"] and not r["rejected"])
    print("distribution tests: %d tests, %d rejected (Holm, alpha %g);"
          " smallest p %.3g, smallest Holm-adjusted p %.3g"
          % (len(tests), bad, args.alpha, min(r["p"] for r in tests),
             min(r["p_holm"] for r in tests)))
    print("negative controls: %d, %d eligible, %d rejected, %d eligible"
          " not rejected" % (len(controls),
                              sum(r["eligible"] for r in controls),
                              sum(r["rejected"] for r in controls), missed))
    return 0 if bad == 0 and missed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
