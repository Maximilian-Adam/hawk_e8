#!/usr/bin/env python3
"""
Precision audit of every HAWK and HAWK-E8-CM sampler variant (Python
standard library only).

Reads the committed integer tables (e8/tables/*.h), computes the
implemented probability of every outcome exactly (Fractions with
power-of-two denominators), and compares it with the ideal discrete
Gaussian of e8/tables/gen_tables.py (DGauss, the same parameters and
support rule, evaluated here at PREC digits). One set of measures for both
schemes, per draw, per unit (HAWK coordinate, E8 block given tau) and per
signature:

  - statistical distance SD = 1/2 sum |P_impl - P_ideal|;
  - Renyi divergence as hawk-spec.pdf defines and uses it (section 2,
    eq. (5), PDF page 7; section 6.5, eqs. (57) and (58), PDF pages 61-62):
        R_a(P_impl || P_ideal) = (sum_x P_impl(x)^a / P_ideal(x)^(a-1))^(1/(a-1)),
    at the orders --orders (default: the spec's 513, 4181 and 37286), at
    infinity (max P_impl / P_ideal), at a = 2 lambda + 1 for the condition
    of eq. (6) (PDF page 8), and at the order minimising the loss factor
    R_a(P_1 || P_2) 2^(lambda / (a - 1)) of eq. (57), with
    R_a(P_1 || P_2) = R_a(unit)^(units q_s) as in eq. (58);
  - the maximum relative error |P_impl / P_ideal - 1| over the implemented
    support, the ideal mass outside it, and the tail probability of the hot
    tables.

Implemented distributions:

  - hot tables (B = 64 or 32): P(v_i) = (q_i - q_{i-1}) / 2^B with
    q_{-1} = -1, P(tail) = (2^B - 1 - q_last) / 2^B, P(w_j) = P(tail)
    (r_j - r_{j-1}) / 2^B with r_{-1} = -1; E8 component CDFs likewise;
  - HAWK ref: P(r >= k + 1) = T[2k + p] / 2^78, value +-(2r + p) with a
    uniform sign bit (both signs give 0 for p = 0, r = 0);
  - E8 ref 1D: value lo_value[r] + j with probability (T[j-1] - T[j]) /
    2^78, T[-1] = 2^78, T[j] = 0 past the table; the class with centre k/4
    uses residue k mod 4 shifted by floor(k/4);
  - E8 ref component: slot s with probability (S[s-1] - S[s]) / 2^78,
    S[-1] = 2^78, S[15] = 0, component = 4-bit field s of the tau's
    slot permutation.

E8 block given tau: (component, eight product coordinates) is a bijection
onto the block, and the block weight exp(-||P z||^2 / (2 sigma_sign^2)) is
the product of the eight 1D weights of the component's classes (checked:
32 base_norm2 = sum num32^2 for every tau and component), so the ideal
block is the ideal component (mass = product of the eight 1D normalisers)
times the eight ideal 1D draws, and the implemented block likewise. The
block Renyi sum factorises exactly,
    sum_block P^a Q^(1-a) = sum_c P_c^a Q_c^(1-a) prod_v S_a(class(tau, c, v)),
with S_a the 1D sums; the block SD is the bound
SD(component) + max_c sum_v SD(class(tau, c, v)). Per signature (one
accepted attempt): HAWK 2n coordinates, E8 n/4 blocks; Renyi divergence is
multiplicative over the independent draws, SD uses the union bound
units * max SD(unit). Worst case: every unit at the worst parity / tau;
"uniform": half of each parity, every tau equally often (the geometric
mean of the 256 block divergences).

lambda and q_s per logn are the spec's (Table 4, PDF page 16, and the aux
script generate_C_tables.py: lambda = 2^(logn - 2) = 64, 128, 256;
q_s = 2^32, 2^64, 2^64), used for both schemes.

Ideal support: DGauss truncates to 20 s + 32 around the centre and
renormalises; the true ideal mass outside is bounded per distribution
(trunc_bound_log2, below 2^-1000). The true ideal differs from the
truncated one by at most that much in SD, and R_a(P || Q_true) =
R_a(P || Q_trunc) / (1 - eps) for P supported inside the truncation.

Usage:  precision_audit.py [--out-dir DIR] [--orders A,B,...] [--self-test]
                           [--c-moments FILE] [--hawk-src FILE]
"""

import argparse
import csv
import decimal
import os
import re
import sys
from fractions import Fraction

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
TABLES = os.path.join(HERE, "..", "tables")
sys.path.insert(0, TABLES)
import gen_tables as gt  # noqa: E402

PREC = 220
decimal.getcontext().prec = PREC
decimal.getcontext().Emax = 10 ** 15
decimal.getcontext().Emin = -10 ** 15
D = decimal.Decimal
LN2 = D(2).ln()
# Differences below NOISE are rounding of the PREC-digit ideal: reported as
# exact zeros (they occur where the ideal is exactly representable, e.g. a
# uniform E8 component distribution).
NOISE = D(10) ** -(PREC - 20)

ORDERS_DEFAULT = (513, 4181, 37286)
SPEC_ORDER = 513
LAMBDA = {8: 64, 9: 128, 10: 256}
QS_LOG2 = {8: 32, 9: 64, 10: 64}
A_MIN, A_MAX = 2, 1 << 20

VARIANTS = (
    ("hawk", "ref", 78, "ct_full_scan"),
    ("hawk", "ref-T", 78, "ct_full_scan"),
    ("hawk", "hot64", 64, "vt_hot_table"),
    ("hawk", "hot32", 32, "vt_hot_table"),
    ("e8", "ref", 78, "ct_full_scan"),
    ("e8", "hot64", 64, "vt_hot_table"),
    ("e8", "hot32", 32, "vt_hot_table"),
)

# ---------------------------------------------------------------------------
# Header parsing


TOKEN = re.compile(r"\{|\}|-?(?:0[xX][0-9A-Fa-f]+|\d+)[uUlL]*")


def parse_initializer(text, pos):
    """Nested lists of ints from the brace initializer starting at pos."""
    stack = [[]]
    for m in TOKEN.finditer(text, pos):
        tok = m.group(0)
        if tok == "{":
            stack.append([])
        elif tok == "}":
            done = stack.pop()
            stack[-1].append(done)
            if len(stack) == 1:
                return stack[0][0]
        else:
            stack[-1].append(int(tok.rstrip("uUlL"), 0))
    raise ValueError("unterminated initializer")


def parse_header(path):
    """(defines, arrays): #define name -> int, static const name -> list."""
    text = open(path).read()
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    defines = {m.group(1): int(m.group(2), 0) for m in re.finditer(
        r"^#define\s+(\w+)\s+(-?(?:0[xX][0-9A-Fa-f]+|\d+))\s*$", text, re.M)}
    arrays = {}
    for m in re.finditer(r"static\s+const\s+\w+\s+(\w+)\s*(?:\[[^\]]*\]\s*)*=",
                         text):
        arrays[m.group(1)] = parse_initializer(text, m.end())
    return defines, arrays


# ---------------------------------------------------------------------------
# Exact helpers


def dec(f):
    return D(f.numerator) / D(f.denominator)


def log2d(x):
    """log2 of a non-negative Decimal or Fraction, as Decimal (None for 0)."""
    if isinstance(x, Fraction):
        x = dec(x)
    if x <= 0:
        return None
    return x.ln() / LN2


def log2_rm1(lnr):
    """log2(R - 1) for R = exp(lnr) (None if R <= 1)."""
    if lnr is None:
        return D("Infinity")
    if lnr <= NOISE:
        return None
    if lnr > 100:
        return lnr / LN2
    return log2d(lnr.exp() - 1)


def fmt_log2(v):
    if v is None:
        return "-inf"
    if v.is_infinite():
        return "inf"
    return "%.3f" % v


def fmt_sci(v):
    if isinstance(v, Fraction):
        v = dec(v)
    if v == 0:
        return "0"
    return "{:.6e}".format(v)


def trunc_bound(dg):
    """Upper bound on the ideal mass outside DGauss's support (relative)."""
    two_s2 = 2 * dec(dg.s2)
    s2 = dec(dg.s2)
    c = dec(dg.c)
    total = D(0)
    for d in (c - (dg.lo - 1), (dg.hi + 1) - c):
        # sum_{k >= 0} exp(-(d + k)^2 / 2s^2) <= exp(-d^2/2s^2) / (1 - exp(-d/s^2))
        total += (-(d * d) / two_s2).exp() / (1 - (-d / s2).exp())
    return total / dg.total


# ---------------------------------------------------------------------------
# One draw: implemented vs ideal


class Pair:
    """
    One draw: implemented distribution impl (value -> Fraction, exact) and
    ideal distribution ideal (value -> Decimal, truncated support, sums to
    1), with the truncation bound eps.
    """

    def __init__(self, impl, ideal, eps=D(0), bits=None, p_tail=None,
                 zero_entries=0, entries=0):
        self.impl_all = impl
        self.impl = {x: p for x, p in impl.items() if p != 0}
        self.ideal = ideal
        self.eps = eps
        self.bits = bits
        self.p_tail = p_tail
        self.zero_entries = zero_entries
        self.entries = entries
        if sum(self.impl.values(), Fraction(0)) != 1:
            raise ValueError("implemented distribution does not sum to 1")
        self.outside = sorted(x for x in self.impl if x not in ideal)
        self.infinite = bool(self.outside)
        keys = set(self.impl) | set(ideal)
        self.sd = sum((abs(dec(self.impl.get(x, Fraction(0)))
                           - ideal.get(x, D(0))) for x in keys), D(0)) / 2
        if self.sd < NOISE:
            self.sd = D(0)
        self.out_mass = sum((q for x, q in ideal.items()
                             if x not in self.impl), D(0))
        self.terms = []         # (P, ln P - ln Q)
        self.ln_rmax = None
        self.ln_rmin = None
        if not self.infinite:
            for x in sorted(self.impl):
                p = dec(self.impl[x])
                lr = p.ln() - ideal[x].ln()
                self.terms.append((p, lr))
            self.ln_rmax = max(lr for _, lr in self.terms)
            self.ln_rmin = min(lr for _, lr in self.terms)
        self._sums = {}

    def renyi_sum(self, a):
        """sum_x P^a Q^(1-a) = exp((a - 1) ln R_a)."""
        if self.infinite:
            return None
        if a not in self._sums:
            am1 = D(a) - 1
            self._sums[a] = sum((p * (am1 * lr).exp()
                                 for p, lr in self.terms), D(0))
        return self._sums[a]

    def ln_renyi(self, a):
        """ln R_a(impl || ideal); a = 'inf' for order infinity."""
        if self.infinite:
            return None
        if a == "inf":
            return self.ln_rmax
        return self.renyi_sum(a).ln() / (D(a) - 1)

    def max_rel_err(self):
        if self.infinite:
            return D("Infinity")
        e = max(self.ln_rmax.exp() - 1, 1 - self.ln_rmin.exp())
        return D(0) if e < NOISE else e

    def moments(self):
        m1 = sum((x * p for x, p in self.impl.items()), Fraction(0))
        m2 = sum((x * x * p for x, p in self.impl.items()), Fraction(0))
        return m1, m2 - m1 * m1

    def reverse_infinite(self):
        """R_a(ideal || impl) is infinite iff the ideal has mass outside."""
        return self.out_mass > 0


def ideal_of(dg, xmap=lambda y: y):
    return {xmap(y): w / dg.total for y, w in dg.w.items()}


def hot_impl(row, bits):
    """Implemented distribution of a hot row [tiny_len, tail_len, tv, lv, tq, lq]."""
    tiny_len, tail_len, tv, lv, tq, lq = row
    full = (1 << bits) - 1
    scale = 1 << bits
    impl = {}
    zero = 0
    prev = -1
    for i in range(tiny_len):
        if tq[i] < prev or tq[i] > full:
            raise ValueError("hot thresholds not monotone")
        p = Fraction(tq[i] - prev, scale)
        zero += p == 0
        if tv[i] in impl:
            raise ValueError("repeated hot value")
        impl[tv[i]] = p
        prev = tq[i]
    p_tail = Fraction(full - prev, scale)
    if tail_len == 0:
        if prev != full:
            raise ValueError("last hot threshold is not the maximum")
    else:
        if lq[tail_len - 1] != full:
            raise ValueError("last tail threshold is not the maximum")
        prev = -1
        for j in range(tail_len):
            if lq[j] < prev:
                raise ValueError("tail thresholds not monotone")
            p = p_tail * Fraction(lq[j] - prev, scale)
            zero += p == 0
            if lv[j] in impl:
                raise ValueError("repeated tail value")
            impl[lv[j]] = p
            prev = lq[j]
    return impl, p_tail, zero, tiny_len + tail_len


def cdf_impl(cdf, bits):
    """Component distribution of a hot component CDF (index -> Fraction)."""
    full = (1 << bits) - 1
    if cdf[-1] != full:
        raise ValueError("last component threshold is not the maximum")
    out = {}
    prev = -1
    for c, q in enumerate(cdf):
        if q < prev:
            raise ValueError("component CDF not monotone")
        out[c] = Fraction(q - prev, 1 << bits)
        prev = q
    return out


def survival_impl(lo_value, table, bits=78):
    """value lo_value + j: (T[j-1] - T[j]) / 2^bits, T[-1] = 2^bits, T[len] = 0."""
    scale = 1 << bits
    t = [scale] + list(table) + [0]
    for j in range(len(t) - 1):
        if t[j] < t[j + 1]:
            raise ValueError("survival table not monotone")
    return {lo_value + j: Fraction(t[j] - t[j + 1], scale)
            for j in range(len(t) - 1)}


def join78(hi, lo):
    if hi >= 1 << 15 or lo >= 1 << 63:
        raise ValueError("78-bit split out of range")
    return (hi << 63) | lo


def hawk_ref_impl(hi, lo, p):
    """HAWK sig_gauss() distribution of x for parity p."""
    scale = 1 << 78
    if len(lo) % 2 or len(hi) > len(lo):
        raise ValueError("HAWK ref table shape")
    T = [join78(hi[i] if i < len(hi) else 0, lo[i]) for i in range(len(lo))]
    surv = [scale] + [T[2 * k + p] for k in range(len(lo) // 2)] + [0]
    impl = {}
    zero = 0
    for r in range(len(surv) - 1):
        if surv[r] < surv[r + 1]:
            raise ValueError("HAWK ref table not monotone")
        pr = Fraction(surv[r] - surv[r + 1], scale)
        zero += pr == 0
        a = 2 * r + p
        if a == 0:
            impl[0] = impl.get(0, Fraction(0)) + pr
        else:
            impl[a] = impl.get(a, Fraction(0)) + pr / 2
            impl[-a] = impl.get(-a, Fraction(0)) + pr / 2
    return impl, zero, len(surv) - 1


# ---------------------------------------------------------------------------
# Loading every variant


class HawkConfig:
    """HAWK, one variant and logn: the unit is one coordinate, 2n per signature."""

    unit_name = "coordinate"

    def __init__(self, variant, bits, logn, pairs):
        self.scheme = "hawk"
        self.variant = variant
        self.bits = bits
        self.logn = logn
        self.pairs = pairs              # parity -> Pair
        self.units = 2 << logn

    def draws(self):
        return [("1d", p, self.pairs[p]) for p in (0, 1)]

    def unit_ln_renyi(self, a):
        """(worst, uniform) ln R_a per unit."""
        v = [self.pairs[p].ln_renyi(a) for p in (0, 1)]
        return max(v), (v[0] + v[1]) / 2

    def unit_sd(self):
        return max(self.pairs[p].sd for p in (0, 1))

    def unit_max_rel_err(self):
        return max(self.pairs[p].max_rel_err() for p in (0, 1))

    def unit_out_mass(self):
        return max(self.pairs[p].out_mass for p in (0, 1))


class E8Config:
    """E8, one variant and logn: the unit is one block given tau, n/4 per signature."""

    unit_name = "block"

    def __init__(self, variant, bits, logn, classes, comps, geom):
        self.scheme = "e8"
        self.variant = variant
        self.bits = bits
        self.logn = logn
        self.classes = classes          # class -> Pair
        self.comps = comps              # tau -> Pair over components
        self.geom = geom
        self.units = 1 << (logn - 2)
        self._block = {}

    def draws(self):
        return ([("1d", j, p) for j, p in enumerate(self.classes)]
                + [("component", t, p) for t, p in enumerate(self.comps)])

    def block_sums(self, a):
        """sum over the block of P^a Q^(1-a), per tau (exact factorisation)."""
        if a in self._block:
            return self._block[a]
        if a == "inf":
            ln_s = [c.ln_rmax for c in self.classes]
            out = []
            for tau in range(256):
                vals = []
                for c, (p_c, lr_c) in self._comp_terms(tau):
                    vals.append(lr_c + sum(ln_s[k] for k in
                                           self.geom.coord_class[tau][c]))
                out.append(max(vals))
            self._block[a] = out
            return out
        s = [c.renyi_sum(a) for c in self.classes]
        am1 = D(a) - 1
        out = []
        for tau in range(256):
            tot = D(0)
            for c, (p_c, lr_c) in self._comp_terms(tau):
                prod = p_c * (am1 * lr_c).exp()
                for k in self.geom.coord_class[tau][c]:
                    prod *= s[k]
                tot += prod
            out.append(tot)
        self._block[a] = out
        return out

    def _comp_terms(self, tau):
        comp = self.comps[tau]
        return [(c, t) for c, t in zip(sorted(comp.impl), comp.terms)]

    def block_ln_renyi(self, a):
        """ln R_a(block | tau) for every tau."""
        sums = self.block_sums(a)
        if a == "inf":
            return sums
        am1 = D(a) - 1
        return [x.ln() / am1 for x in sums]

    def unit_ln_renyi(self, a):
        """(worst, uniform) ln R_a per block."""
        if a == "inf":
            v = self.block_sums(a)
            return max(v), sum(v, D(0)) / 256
        sums = self.block_sums(a)
        am1 = D(a) - 1
        worst = max(sums).ln() / am1
        uniform = sum((x.ln() for x in sums), D(0)) / am1 / 256
        return worst, uniform

    def block_sd(self, tau):
        comp = self.comps[tau]
        return comp.sd + max(sum(self.classes[k].sd for k in
                                 self.geom.coord_class[tau][c])
                             for c in comp.impl)

    def block_max_rel_err(self, tau):
        comp = self.comps[tau]
        hi = lo = None
        for c, (p_c, lr_c) in self._comp_terms(tau):
            ks = self.geom.coord_class[tau][c]
            a = lr_c + sum(self.classes[k].ln_rmax for k in ks)
            b = lr_c + sum(self.classes[k].ln_rmin for k in ks)
            hi = a if hi is None else max(hi, a)
            lo = b if lo is None else min(lo, b)
        return max(hi.exp() - 1, 1 - lo.exp())

    def block_out_mass(self, tau):
        comp = self.comps[tau]
        inside = D(0)
        for c in comp.impl:
            m = comp.ideal[c]
            for k in self.geom.coord_class[tau][c]:
                m *= 1 - self.classes[k].out_mass
            inside += m
        return 1 - inside

    def block_trunc_bound(self):
        return 16 * max(c.eps for c in self.classes)

    def unit_sd(self):
        return max(self.block_sd(t) for t in range(256))

    def unit_max_rel_err(self):
        return max(self.block_max_rel_err(t) for t in range(256))

    def unit_out_mass(self):
        return max(self.block_out_mass(t) for t in range(256))


class Geometry:
    def __init__(self, arrays):
        self.centers = arrays["e8_tab_center_num32"]
        self.coord_class = arrays["e8_tab_coord_class"]
        self.base_norm2 = arrays["e8_tab_base_norm2"]


def e8_ideal_classes(gdists):
    return [ideal_of(dg) for dg in gdists]


def e8_ideal_component(geom, gdists, tau):
    totals = [dg.total for dg in gdists]
    masses = []
    for c in range(16):
        m = D(1)
        for k in geom.coord_class[tau][c]:
            m *= totals[k]
        masses.append(m)
    total = sum(masses, D(0))
    return {c: m / total for c, m in enumerate(masses)}


def load(hawk_src):
    """Every configuration: dict (scheme, variant, logn) -> config."""
    tdir = TABLES
    _, geo_a = parse_header(os.path.join(tdir, "e8_tables_geom.h"))
    geom = Geometry(geo_a)
    ggeom = gt.E8Geometry()
    hdr = {}
    for name in ("e8_tables_hot64.h", "e8_tables_hot32.h", "e8_tables_ref.h",
                 "hawk_tables_ref.h", "hawk_tables_hot64.h",
                 "hawk_tables_hot32.h"):
        hdr[name] = parse_header(os.path.join(tdir, name))
    configs = {}
    for logn in (8, 9, 10):
        # HAWK
        hd = gt.hawk_dists(logn)
        hideal = {p: ideal_of(hd[p], lambda y, p=p: 2 * y + p) for p in (0, 1)}
        heps = {p: trunc_bound(hd[p]) for p in (0, 1)}
        _, ra = hdr["hawk_tables_ref.h"]
        pairs = {}
        for p in (0, 1):
            impl, zero, entries = hawk_ref_impl(ra["hawk_ref_hi_logn%d" % logn],
                                                ra["hawk_ref_lo_logn%d" % logn], p)
            pairs[p] = Pair(impl, hideal[p], heps[p], 78, None, zero, entries)
        configs[("hawk", "ref", logn)] = HawkConfig("ref", 78, logn, pairs)
        for bits in (64, 32):
            _, ha = hdr["hawk_tables_hot%d.h" % bits]
            rows = ha["hawk_hot%d_tables_logn%d" % (bits, logn)][0]
            pairs = {}
            for p in (0, 1):
                impl, p_tail, zero, entries = hot_impl(rows[p], bits)
                pairs[p] = Pair(impl, hideal[p], heps[p], bits, p_tail, zero,
                                entries)
            configs[("hawk", "hot%d" % bits, logn)] = \
                HawkConfig("hot%d" % bits, bits, logn, pairs)
        # E8
        gd = gt.e8_dists(ggeom, logn)
        cideal = e8_ideal_classes(gd)
        ceps = [trunc_bound(dg) for dg in gd]
        comp_ideal = [e8_ideal_component(geom, gd, t) for t in range(256)]
        comp_eps = 8 * max(ceps)
        for bits in (64, 32):
            _, ea = hdr["e8_tables_hot%d.h" % bits]
            tab = ea["e8_hot%d_tables_logn%d" % (bits, logn)]
            sigma_bits, rows, ccdf = tab
            if sigma_bits != gt.double_bits(gt.E8_SIGMA_SIGN[logn]):
                raise ValueError("E8 hot%d sigma_sign bits" % bits)
            classes = []
            for j, row in enumerate(rows):
                impl, p_tail, zero, entries = hot_impl(row, bits)
                classes.append(Pair(impl, cideal[j], ceps[j], bits, p_tail,
                                    zero, entries))
            comps = []
            for t in range(256):
                impl = cdf_impl(ccdf[t], bits)
                comps.append(Pair(impl, comp_ideal[t], comp_eps, bits, None,
                                  sum(1 for p in impl.values() if p == 0),
                                  16))
            configs[("e8", "hot%d" % bits, logn)] = \
                E8Config("hot%d" % bits, bits, logn, classes, comps, geom)
        rdef, rarr = hdr["e8_tables_ref.h"]
        tab = rarr["e8_ref_tables_logn%d" % logn]
        sigma_bits, lo_value, his, los, slot_hi, slot_lo = tab
        if sigma_bits != gt.double_bits(gt.E8_SIGMA_SIGN[logn]):
            raise ValueError("E8 ref sigma_sign bits")
        res_impl = []
        for r in range(4):
            T = [join78(h, l) for h, l in zip(his[r], los[r])]
            while T and T[-1] == 0:
                T.pop()
            res_impl.append(survival_impl(lo_value[r], T))
        classes = []
        for j, num in enumerate(geom.centers):
            k = num // 8
            if num % 8:
                raise ValueError("E8 centre is not k/4")
            shift = k // 4
            impl = {x + shift: p for x, p in res_impl[k % 4].items()}
            classes.append(Pair(impl, cideal[j], ceps[j], 78, None,
                                sum(1 for p in impl.values() if p == 0),
                                len(impl)))
        row_class = rarr["e8_ref_row_class"]
        slot_perm = rarr["e8_ref_slot_perm"]
        comps = []
        for t in range(256):
            perm = [(slot_perm[t] >> (4 * s)) & 15 for s in range(16)]
            if sorted(perm) != list(range(16)):
                raise ValueError("E8 ref slot permutation tau=%d" % t)
            rc = row_class[t]
            S = [join78(h, l) for h, l in zip(slot_hi[rc], slot_lo[rc])]
            slots = survival_impl(0, S)
            impl = {perm[s]: p for s, p in slots.items()}
            comps.append(Pair(impl, comp_ideal[t], comp_eps, 78, None,
                              sum(1 for p in impl.values() if p == 0), 16))
        configs[("e8", "ref", logn)] = E8Config("ref", 78, logn, classes,
                                               comps, geom)
    return configs, geom, ggeom, hdr


# ---------------------------------------------------------------------------
# Per-signature and per-transcript figures


def sig_ln_renyi(cfg, a):
    """(worst, uniform) ln R_a per signature."""
    w, u = cfg.unit_ln_renyi(a)
    return w * cfg.units, u * cfg.units


def loss_log2(cfg, a, lam=None, qs_log2=None):
    """log2 of R_a(P_1 || P_2) 2^(lambda / (a - 1)), eqs. (57)-(58)."""
    lam = LAMBDA[cfg.logn] if lam is None else lam
    qs_log2 = QS_LOG2[cfg.logn] if qs_log2 is None else qs_log2
    w, _ = sig_ln_renyi(cfg, a)
    return D(lam) / (D(a) - 1) + w * (D(2) ** qs_log2) / LN2


def optimise_order(cfg):
    """Integer order in [A_MIN, A_MAX] minimising loss_log2 (ternary search)."""
    cache = {}

    def f(a):
        if a not in cache:
            cache[a] = loss_log2(cfg, a)
        return cache[a]

    lo, hi = A_MIN, A_MAX
    while hi - lo > 2:
        m1 = lo + (hi - lo) // 3
        m2 = hi - (hi - lo) // 3
        if f(m1) <= f(m2):
            hi = m2
        else:
            lo = m1
    best = min(range(lo, hi + 1), key=lambda a: (f(a), a))
    return best, f(best)


def fmt_num(v):
    if abs(v) >= 10 ** 6:
        return "{:.4e}".format(v)
    return "%.4f" % v


def fmt_loss(l2):
    if l2 > 13:
        return "{:.3e}".format((l2 * LN2).exp())
    return "%.4f" % (l2 * LN2).exp()


SUMMARY_FIELDS = [
    "scheme", "variant", "threshold_bits", "technique", "logn", "n",
    "unit", "units_per_sig", "lambda", "qs_log2",
    "draw_sd_max_log2", "draw_rd513_m1_max_log2", "draw_rdinf_m1_max_log2",
    "draw_max_rel_err", "draw_ideal_out_mass_max_log2", "draw_p_tail_max",
    "unit_sd_bound_log2", "unit_rd513_m1_log2", "unit_rdinf_m1_log2",
    "sig_sd_bound_log2", "sig_rd513_m1_log2", "sig_rd513_m1_uniform_log2",
    "sig_rdinf_m1_log2", "cond6_order", "sig_cond6_m1_log2", "cond6_ok",
    "loss_a513_log2", "a_opt", "loss_opt_log2", "loss_opt",
]


def summary_row(cfg, scheme, variant, bits, technique):
    logn = cfg.logn
    lam = LAMBDA[logn]
    draws = [p for _, _, p in cfg.draws()]
    tails = [p.p_tail for p in draws if p.p_tail is not None]
    a6 = 2 * lam + 1
    w513, u513 = sig_ln_renyi(cfg, SPEC_ORDER)
    winf, _ = sig_ln_renyi(cfg, "inf")
    w6, _ = sig_ln_renyi(cfg, a6)
    m6 = log2_rm1(w6)
    a_opt, l_opt = optimise_order(cfg)
    usd = cfg.unit_sd()
    return {
        "scheme": scheme, "variant": variant, "threshold_bits": bits,
        "technique": technique, "logn": logn, "n": 1 << logn,
        "unit": cfg.unit_name, "units_per_sig": cfg.units, "lambda": lam,
        "qs_log2": QS_LOG2[logn],
        "draw_sd_max_log2": fmt_log2(log2d(max(p.sd for p in draws))),
        "draw_rd513_m1_max_log2": fmt_log2(log2_rm1(
            max(p.ln_renyi(SPEC_ORDER) for p in draws))),
        "draw_rdinf_m1_max_log2": fmt_log2(log2_rm1(
            max(p.ln_rmax for p in draws))),
        "draw_max_rel_err": fmt_sci(max(p.max_rel_err() for p in draws)),
        "draw_ideal_out_mass_max_log2": fmt_log2(log2d(
            max(p.out_mass for p in draws))),
        "draw_p_tail_max": fmt_sci(max(tails)) if tails else "",
        "unit_sd_bound_log2": fmt_log2(log2d(usd)),
        "unit_rd513_m1_log2": fmt_log2(log2_rm1(
            cfg.unit_ln_renyi(SPEC_ORDER)[0])),
        "unit_rdinf_m1_log2": fmt_log2(log2_rm1(cfg.unit_ln_renyi("inf")[0])),
        "sig_sd_bound_log2": fmt_log2(log2d(usd * cfg.units)),
        "sig_rd513_m1_log2": fmt_log2(log2_rm1(w513)),
        "sig_rd513_m1_uniform_log2": fmt_log2(log2_rm1(u513)),
        "sig_rdinf_m1_log2": fmt_log2(log2_rm1(winf)),
        "cond6_order": a6,
        "sig_cond6_m1_log2": fmt_log2(m6),
        "cond6_ok": "yes" if m6 is None or m6 <= -(2 + QS_LOG2[logn])
                    else "no",
        "loss_a513_log2": fmt_num(loss_log2(cfg, SPEC_ORDER)),
        "a_opt": a_opt,
        "loss_opt_log2": fmt_num(l_opt),
        "loss_opt": fmt_loss(l_opt),
    }


def draw_fields(orders):
    return (["scheme", "variant", "threshold_bits", "logn", "kind", "id",
             "support", "zero_entries", "p_tail", "sd", "sd_log2"]
            + ["rd%d_m1_log2" % a for a in orders]
            + ["rdinf_m1_log2", "max_rel_err", "ideal_out_mass_log2",
               "trunc_bound_log2"])


def draw_row(cfg, kind, ident, pr, orders):
    row = {"scheme": cfg.scheme, "variant": cfg.variant,
           "threshold_bits": cfg.bits, "logn": cfg.logn, "kind": kind,
           "id": ident, "support": len(pr.impl),
           "zero_entries": pr.zero_entries,
           "p_tail": fmt_sci(pr.p_tail) if pr.p_tail is not None else "",
           "sd": fmt_sci(pr.sd), "sd_log2": fmt_log2(log2d(pr.sd)),
           "rdinf_m1_log2": fmt_log2(log2_rm1(pr.ln_rmax)),
           "max_rel_err": fmt_sci(pr.max_rel_err()),
           "ideal_out_mass_log2": fmt_log2(log2d(pr.out_mass)),
           "trunc_bound_log2": fmt_log2(log2d(pr.eps))}
    for a in orders:
        row["rd%d_m1_log2" % a] = fmt_log2(log2_rm1(pr.ln_renyi(a)))
    return row


def block_rows(cfg, orders):
    lnr = {a: cfg.block_ln_renyi(a) for a in orders}
    lninf = cfg.block_ln_renyi("inf")
    tb = fmt_log2(log2d(cfg.block_trunc_bound()))
    rows = []
    for tau in range(256):
        sd = cfg.block_sd(tau)
        row = {"scheme": cfg.scheme, "variant": cfg.variant,
               "threshold_bits": cfg.bits, "logn": cfg.logn, "kind": "block",
               "id": tau, "support": "", "zero_entries": "", "p_tail": "",
               "sd": fmt_sci(sd), "sd_log2": fmt_log2(log2d(sd)),
               "rdinf_m1_log2": fmt_log2(log2_rm1(lninf[tau])),
               "max_rel_err": fmt_sci(cfg.block_max_rel_err(tau)),
               "ideal_out_mass_log2": fmt_log2(log2d(
                   cfg.block_out_mass(tau))),
               "trunc_bound_log2": tb}
        for a in orders:
            row["rd%d_m1_log2" % a] = fmt_log2(log2_rm1(lnr[a][tau]))
        rows.append(row)
    return rows


def dist_rows(cfg):
    """Exact implemented distributions, for the validation's goodness-of-fit tests."""
    out = []
    for kind, ident, pr in cfg.draws():
        for x in sorted(set(pr.impl_all) | set(pr.ideal)):
            p = pr.impl_all.get(x, Fraction(0))
            den = p.denominator
            out.append({"scheme": cfg.scheme, "variant": cfg.variant,
                        "logn": cfg.logn, "kind": kind, "id": ident,
                        "value": x, "p_impl_num": p.numerator,
                        "p_impl_den_log2": den.bit_length() - 1,
                        "p_impl": fmt_sci(p),
                        "p_ideal": fmt_sci(pr.ideal.get(x, D(0)))})
    return out


DIST_FIELDS = ["scheme", "variant", "logn", "kind", "id", "value",
               "p_impl_num", "p_impl_den_log2", "p_impl", "p_ideal"]


def write_csv(path, fields, rows):
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow(r)


# ---------------------------------------------------------------------------
# Self-tests


def check(cond, what):
    if not cond:
        sys.exit("precision_audit: self-test failed: " + what)


def close(a, b, rel=D("1e-150")):
    return abs(a - b) <= rel * max(abs(a), abs(b), D(1))


def self_test_toy():
    # Identical distributions.
    q = {0: D(1) / 4, 1: D(3) / 4}
    p = Pair({0: Fraction(1, 4), 1: Fraction(3, 4)}, q)
    check(p.sd == 0, "toy identical SD")
    for a in (2, 513, 37286):
        check(abs(p.ln_renyi(a)) < D("1e-200"), "toy identical R_%d" % a)
    check(abs(p.ln_rmax) < D("1e-200"), "toy identical R_inf")
    # Two points: P = (1/2, 1/2), Q = (1/4, 3/4): SD 1/4, R_2 = 4/3,
    # R_3 = sqrt(20/9), R_inf = 2.
    p = Pair({0: Fraction(1, 2), 1: Fraction(1, 2)}, q)
    check(close(p.sd, D(1) / 4), "toy SD")
    check(close(p.ln_renyi(2), (D(4) / 3).ln()), "toy R_2")
    check(close(p.ln_renyi(3), (D(20) / 9).sqrt().ln()), "toy R_3")
    check(close(p.ln_rmax, D(2).ln()), "toy R_inf")
    check(close(p.max_rel_err(), D(1)), "toy max relative error")
    # Support of P not inside the support of Q: infinite.
    p = Pair({0: Fraction(1, 2), 2: Fraction(1, 2)}, q)
    check(p.infinite and p.ln_renyi(2) is None
          and log2_rm1(p.ln_renyi(2)).is_infinite(), "toy infinite")
    # The factorised block sum equals the brute-force product sum.
    p1 = Pair({0: Fraction(1, 3), 1: Fraction(2, 3)},
              {0: D(1) / 2, 1: D(1) / 2})
    p2 = Pair({-1: Fraction(1, 8), 0: Fraction(7, 8)},
              {-1: D(1) / 5, 0: D(4) / 5})
    pc = Pair({0: Fraction(3, 4), 1: Fraction(1, 4)},
              {0: D(2) / 3, 1: D(1) / 3})
    coords = {0: (p1, p2), 1: (p2, p2)}
    for a in (2, 7):
        fact = sum((dec(pc.impl[c]) ** a * pc.ideal[c] ** (1 - a)
                    * coords[c][0].renyi_sum(a) * coords[c][1].renyi_sum(a)
                    for c in (0, 1)), D(0))
        brute = D(0)
        for c in (0, 1):
            for x, px in coords[c][0].impl.items():
                for y, py in coords[c][1].impl.items():
                    pp = dec(pc.impl[c] * px * py)
                    qq = (pc.ideal[c] * coords[c][0].ideal[x]
                          * coords[c][1].ideal[y])
                    brute += pp ** a * qq ** (1 - a)
        check(close(fact, brute), "toy block factorisation a=%d" % a)
    print("self-test: SD and Renyi formulas on toy distributions (identical,"
          " two-point, disjoint support, block factorisation): ok")


def self_test_tables(configs, geom, ggeom, hdr, hawk_src):
    # Geometry header = generator geometry; block weight identity.
    check(geom.centers == ggeom.centers, "geometry centres")
    check(geom.coord_class == ggeom.coord_class, "geometry classes")
    check(geom.base_norm2 == ggeom.base_norm2, "geometry base norms")
    for t in range(256):
        for c in range(16):
            check(32 * ggeom.base_norm2[t][c]
                  == sum(x * x for x in ggeom.center_num32[t][c]),
                  "block norm identity tau=%d c=%d" % (t, c))
    print("self-test: geometry header = generator; 32 ||P base||^2 ="
          " sum num32^2 for every (tau, component), so the ideal block"
          " = ideal component x eight ideal 1D draws: ok")
    # Sums, supports, reverse direction.
    ndraws = 0
    full_comps = 0
    for cfg in configs.values():
        for kind, ident, pr in cfg.draws():
            ndraws += 1
            if kind == "component":
                full_comps += not pr.reverse_infinite()
            check(sum(pr.impl_all.values(), Fraction(0)) == 1,
                  "sum %s %s %d %s %s" % (cfg.scheme, cfg.variant, cfg.logn,
                                          kind, ident))
            check(not pr.infinite, "support %s %s %d %s %s: %s"
                  % (cfg.scheme, cfg.variant, cfg.logn, kind, ident,
                     pr.outside))
            check(abs(sum(pr.ideal.values(), D(0)) - 1) < D("1e-200"),
                  "ideal normalisation")
            check(kind == "component" or pr.reverse_infinite(),
                  "reverse direction finite %s %s %d %s %s"
                  % (cfg.scheme, cfg.variant, cfg.logn, kind, ident))
    print("self-test: %d implemented distributions sum to exactly 1 and lie"
          " in the ideal support; every 1D table truncates it, so"
          " R_a(ideal || impl) = infinity per coordinate, block and"
          " signature (%d of %d E8 component draws have full support): ok"
          % (ndraws, full_comps, 9 * 256))
    # HAWK ref from the generated header = upstream.
    up = gt.parse_upstream(hawk_src)
    for logn in (8, 9, 10):
        for p in (0, 1):
            a, _, _ = hawk_ref_impl(up[(logn, "hi")], up[(logn, "lo")], p)
            check(a == configs[("hawk", "ref", logn)].pairs[p].impl_all,
                  "HAWK ref logn=%d parity=%d differs from upstream"
                  % (logn, p))
    print("self-test: HAWK ref distributions from hawk_tables_ref.h ="
          " upstream src/hawk_sign.c: ok")


def exact_moments(configs):
    """(scheme, variant, logn, id) -> (mean, var) for every 1D table."""
    out = {}
    for (scheme, variant, logn), cfg in configs.items():
        for kind, ident, pr in cfg.draws():
            if kind == "1d":
                out[(scheme, variant, logn, ident)] = pr.moments()
    return out


def self_test_moments(configs, path):
    if path is None:
        print("self-test: C moments not given (--c-moments); skipped")
        return
    ours = exact_moments(configs)
    seen = set()
    tol = Fraction(1, 10 ** 15)
    for line in open(path):
        f = line.strip().split(",")
        if not f or f[0] != "moments":
            continue
        scheme, variant = f[1], f[2]
        if scheme == "hawk" and variant == "ref-T":
            variant = "ref"
        key = (scheme, variant, int(f[3]), int(f[4]))
        check(key in ours, "C moments for unknown table %s" % (key,))
        m, v = ours[key]
        cm, cv = Fraction(D(f[5])), Fraction(D(f[6]))
        for exact, got, what in ((m, cm, "mean"), (v, cv, "variance")):
            check(abs(exact - got) <= tol * max(abs(exact), 1),
                  "%s %s: exact %.18e, C %.18e" % (what, key, float(exact),
                                                   float(got)))
        seen.add(key)
    check(seen == set(ours), "C moments missing for %s"
          % sorted(set(ours) - seen))
    print("self-test: exact mean and variance of all %d 1D tables = the C"
          " moments of test_e8_tables and test_hawk_variants (relative"
          " 1e-15): ok" % len(ours))


def sanity(configs):
    """Not proofs: SD against entries * 2^-B, and the hot tail probabilities."""
    worst = 0
    for cfg in configs.values():
        for kind, ident, pr in cfg.draws():
            bound = D(pr.entries + 1) * D(2) ** -pr.bits
            r = pr.sd / bound
            worst = max(worst, r)
    tails = {}
    for (scheme, variant, logn), cfg in configs.items():
        t = [pr.p_tail for _, _, pr in cfg.draws() if pr.p_tail is not None]
        if t:
            tails.setdefault(scheme, []).extend(t)
    print("sanity: max SD / ((entries + 1) 2^-B) = %.3f over every draw"
          % worst)
    for scheme, t in sorted(tails.items()):
        print("sanity: %s hot P(tail) per draw in [%.3e, %.3e]"
              % (scheme, min(t), max(t)))
    check(worst <= 1, "SD above (entries + 1) 2^-B")


# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=os.path.join(HERE, "..", "results",
                                                  "precision"))
    ap.add_argument("--orders", default=",".join(str(a) for a in
                                                 ORDERS_DEFAULT))
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--c-moments")
    ap.add_argument("--hawk-src", default=os.path.join(
        HERE, "..", "..", "src", "hawk_sign.c"))
    args = ap.parse_args()
    orders = [int(a) for a in args.orders.split(",")]
    if any(a < 2 for a in orders):
        sys.exit("precision_audit: orders must be >= 2")

    if args.self_test:
        self_test_toy()
    configs, geom, ggeom, hdr = load(args.hawk_src)
    if args.self_test:
        self_test_tables(configs, geom, ggeom, hdr, args.hawk_src)
        self_test_moments(configs, args.c_moments)
        sanity(configs)

    # Reproduce the spec's HAWK ref figures (section 6.5, PDF page 61).
    for logn, a in ((9, 37286), (10, 4181)):
        cfg = configs[("hawk", "ref", logn)]
        l2 = loss_log2(cfg, a)
        print("spec check: HAWK-%d ref, lambda = %d, q_s = 2^%d: loss factor"
              " at a = %d is %.4f (spec: %s)"
              % (1 << logn, LAMBDA[logn], QS_LOG2[logn], a,
                 (l2 * LN2).exp(), "1.07" if logn == 9 else "1.17"))
    for logn in (9, 10):
        cfg = configs[("hawk", "ref", logn)]
        w = max(cfg.pairs[p].ln_renyi(SPEC_ORDER) for p in (0, 1))
        print("spec check: HAWK-%d ref per-coefficient R_513 - 1 = 2^%s"
              " (spec: < 2^-78)" % (1 << logn, fmt_log2(log2_rm1(w))))

    os.makedirs(args.out_dir, exist_ok=True)
    rows = []
    drows = []
    for (scheme, variant, logn), cfg in sorted(configs.items()):
        for kind, ident, pr in cfg.draws():
            rows.append(draw_row(cfg, kind, ident, pr, orders))
        if scheme == "e8":
            rows.extend(block_rows(cfg, orders))
        drows.extend(dist_rows(cfg))
    write_csv(os.path.join(args.out_dir, "precision_audit.csv"),
              draw_fields(orders), rows)
    write_csv(os.path.join(args.out_dir, "precision_audit_dists.csv"),
              DIST_FIELDS, drows)

    summary = []
    for logn in (8, 9, 10):
        for scheme, variant, bits, technique in VARIANTS:
            src = "ref" if variant == "ref-T" else variant
            cfg = configs[(scheme, src, logn)]
            summary.append(summary_row(cfg, scheme, variant, bits,
                                       technique))
    write_csv(os.path.join(args.out_dir, "precision_summary.csv"),
              SUMMARY_FIELDS, summary)
    print("%-5s %-6s %5s %9s %9s %9s %9s %9s %7s %9s"
          % ("", "", "logn", "drawSD", "drawRD-1", "sigSD", "sigRD-1",
             "sigRDinf", "a_opt", "loss"))
    for r in summary:
        print("%-5s %-6s %5d %9s %9s %9s %9s %9s %7d %9s"
              % (r["scheme"], r["variant"], r["logn"],
                 r["draw_sd_max_log2"], r["draw_rd513_m1_max_log2"],
                 r["sig_sd_bound_log2"], r["sig_rd513_m1_log2"],
                 r["sig_rdinf_m1_log2"], r["a_opt"], r["loss_opt"]))
    print("wrote precision_audit.csv precision_audit_dists.csv"
          " precision_summary.csv in %s" % os.path.normpath(args.out_dir))


if __name__ == "__main__":
    main()
