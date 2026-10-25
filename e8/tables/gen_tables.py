#!/usr/bin/env python3
"""
Offline generator of the integer sampler tables of every HAWK and HAWK-E8-CM
sampler variant (Python standard library only).

One discrete Gaussian on Z, D(c, s): weight w(x) = exp(-(x - c)^2 / (2 s^2))
on the integers within 20 s + 32 of c (the support rule of the former E8
runtime builder; the mass outside is below 2^-1000), normalised by the sum
of the weights. Weights are evaluated with decimal arithmetic at PREC
significant digits from the exact rational exponent; every threshold is the
floor of an exact-to-PREC-digits value, and the generator fails if any value
lies within 10^-(PREC-30) of an integer, so the floors are exact.

Hot tables (variants hot64 and hot32, B = 64 or 32 bits, K = 6):

  - the K highest-weight values of the support ("tiny" values), in weight
    order (ties: lower value first), with cumulative thresholds
        q_i = min(floor(prefix_i / total * 2^B), 2^B - 1);
    a value whose threshold does not exceed the previous one is skipped
    (it stays in the tail); if the tail is empty, the last q is 2^B - 1;
  - the tail: the remaining support values in weight order, with
    thresholds of their cumulative mass relative to the tail mass,
        r_j = min(floor(tail_prefix_j / tail_total * 2^B), 2^B - 1),
    the last one forced to 2^B - 1 and the list truncated after the first
    entry equal to 2^B - 1 (later entries are unreachable);
  - sampling: u = next B-bit word; the first i with u <= q_i is the value;
    otherwise u' = next B-bit word, and the first j with u' <= r_j.

  E8 also has per-tau component CDFs over the 16 RM(1,3) components,
        c_i = min(floor(prefix_i / total * 2^B), 2^B - 1), c_15 = 2^B - 1,
  with component mass the product of the eight 1D normalisers.

Reference tables (variant ref, 78 bits, constant-time full scans):

  - HAWK: upstream sig_gauss() tables, T[2k] = floor(P0(|X0| >= 2(k+1))
    2^78), T[2k+1] = floor(P1(|X1| >= 2k+3) 2^78), split into 15-bit high
    and 63-bit low parts; the generator checks them against upstream
    ../src/hawk_sign.c, which it reads but never writes.
  - E8: two-sided survival tables per centre residue (centres are k/4, so
    the four residues r/4, r = 0..3, give every class up to an integer
    shift): T[k] = floor(P(X > lo + k) 2^78) for k = 0 .. hi - lo - 1, where
    lo is the largest v with floor(P(X < v) 2^78) = 0 and hi the smallest v
    with floor(P(X > v) 2^78) = 0; the value is lo + #{k : u < T[k]} for a
    78-bit uniform u. Component selection uses per-tau canonical slots:
    the 16 components sorted by mass type (counts of coordinates with
    residue 0, 1/4 or 3/4, and 1/2), then by index; tau selects one of the
    row classes (the distinct sorted type lists) and a permutation from
    slot to component, and S[j] = floor(P(slot > j) 2^78), j = 0..14.

Parameters: E8 sigma_sign = 3.592, 3.631, 3.669 (logn 8, 9, 10), 1D sigma
sigma_sign / sqrt(32), centres num/32; HAWK sigma_sign = 1.010, 1.278,
1.299 (upstream's constants), x in 2Z + t drawn as 2y + t with
y ~ D(-t/2, sigma_sign). Decimal parameters are used exactly.

Usage:  gen_tables.py [--out DIR] [--hawk-src FILE]
"""

import argparse
import decimal
import os
import struct
import sys
from fractions import Fraction

PREC = 160
MARGIN = decimal.Decimal(10) ** -(PREC - 30)
decimal.getcontext().prec = PREC
D = decimal.Decimal

K_HOT = 6
E8_SIGMA_SIGN = {8: "3.592", 9: "3.631", 10: "3.669"}
HAWK_SIGMA_SIGN = {8: "1.010", 9: "1.278", 10: "1.299"}
TAIL_SIGMAS = 20
TAIL_FLOOR = 32

CMD = "python3 tables/gen_tables.py --out tables"

# ---------------------------------------------------------------------------
# Exact arithmetic helpers


def dec_of_fraction(f):
    return D(f.numerator) / D(f.denominator)


NEAR_INTEGER = []


def floor_scaled(num, den, bits, what=""):
    """
    floor(num / den * 2^bits) for decimals 0 <= num <= den. A value within
    MARGIN of an integer is taken as that integer: this happens only for
    exact symmetries (for example P(X > 0) = 1/2 for centre 1/2), which
    decimal sums cannot resolve; every such case is recorded.
    """
    v = num / den * (D(2) ** bits)
    near = int(v.to_integral_value(rounding=decimal.ROUND_HALF_EVEN))
    if abs(v - near) < MARGIN:
        if num != den and num != 0:
            NEAR_INTEGER.append((what, bits, near))
        return near
    return int(v.to_integral_value(rounding=decimal.ROUND_FLOOR))


def double_bits(text):
    return struct.unpack("<Q", struct.pack("<d", float(text)))[0]


class DGauss:
    """Discrete Gaussian on Z with rational centre c and s^2 = s2."""

    def __init__(self, c, s2):
        self.c = c
        self.s2 = s2
        s = D(s2.numerator).sqrt() / D(s2.denominator).sqrt()
        radius = TAIL_SIGMAS * s + TAIL_FLOOR
        cd = dec_of_fraction(c)
        self.lo = int((cd - radius).to_integral_value(rounding=decimal.ROUND_FLOOR))
        self.hi = int((cd + radius).to_integral_value(rounding=decimal.ROUND_CEILING))
        self.w = {}
        for x in range(self.lo, self.hi + 1):
            e = (Fraction(x) - c) ** 2 / (2 * s2)
            self.w[x] = (-dec_of_fraction(e)).exp()
        self.total = sum(self.w.values(), D(0))

    def weight_order(self):
        # Weight decreases with |x - c|; ties: lower value first.
        return sorted(self.w, key=lambda x: (abs(Fraction(x) - self.c), x))


def hot_row(dg, bits):
    """Hot table of one distribution: (tiny [(value, q)], tail [(value, r)])."""
    order = dg.weight_order()
    full = (1 << bits) - 1
    tiny = []
    mass = D(0)
    for x in order[:K_HOT]:
        prefix = mass + dg.w[x]
        q = min(floor_scaled(prefix, dg.total, bits, "tiny"), full)
        if tiny and q <= tiny[-1][1]:
            continue
        tiny.append((x, q))
        mass = prefix
    used = set(x for x, _ in tiny)
    rest = [x for x in order if x not in used]
    tail_total = dg.total - mass
    tail = []
    if rest and tail_total > 0:
        tprefix = D(0)
        for x in rest:
            tprefix += dg.w[x]
            r = min(floor_scaled(tprefix, tail_total, bits, "tail"),
                    full)
            tail.append((x, r))
            if r == full:
                break
        tail[-1] = (tail[-1][0], full)
    else:
        tiny[-1] = (tiny[-1][0], full)
    return tiny, tail


def survival_table(dg, bits=78):
    """Two-sided 78-bit survival table: (lo, [T[k]])."""
    scale = D(2) ** bits
    xs = list(range(dg.lo, dg.hi + 1))
    below = {}
    acc = D(0)
    for x in xs:
        below[x] = acc          # P(X < x) * total
        acc += dg.w[x]
    lo = max(x for x in xs if int(below[x] / dg.total * scale) == 0)
    above = {}
    acc = D(0)
    for x in reversed(xs):
        above[x] = acc          # P(X > x) * total
        acc += dg.w[x]
    hi = min(x for x in xs if int(above[x] / dg.total * scale) == 0)
    table = [floor_scaled(above[v], dg.total, bits, "ref 1D c=%s" % dg.c)
             for v in range(lo, hi)]
    assert all(t > 0 for t in table)
    assert all(table[i] > table[i + 1] for i in range(len(table) - 1))
    return lo, table


# ---------------------------------------------------------------------------
# E8 geometry (mirrors e8_sampler.c)

PRODUCT_BASIS = [
    [-4, 0, 0, 0, 0, 0, 0, -4], [-4, 0, 0, 0, 0, 0, 0, 4],
    [0, -4, 0, 0, -4, 0, 0, 0], [0, -4, 0, 0, 4, 0, 0, 0],
    [0, 0, -4, 0, 0, -4, 0, 0], [0, 0, -4, 0, 0, 4, 0, 0],
    [0, 0, 0, -4, 0, 0, -4, 0], [0, 0, 0, -4, 0, 0, 4, 0],
]


def shift_get(a, u, s):
    if s == 0:
        return a[u]
    if u >= s:
        return a[u - s]
    return -a[4 + u - s]


def apply_p(z):
    z0, z1 = z[:4], z[4:]
    x = [0] * 8
    for u in range(4):
        x[u] = (2 * (z0[u] + shift_get(z0, u, 1)) + z1[u]
                - shift_get(z1, u, 1) + shift_get(z1, u, 2)
                + shift_get(z1, u, 3))
        x[u + 4] = (z1[u] + shift_get(z1, u, 1) + shift_get(z1, u, 2)
                    + shift_get(z1, u, 3))
    return x


def rm13_rep(c):
    b1 = (c >> 1) & 1
    b2 = (c >> 2) & 1
    b4 = (c >> 3) & 1
    b0 = (c & 1) ^ b1 ^ b2 ^ b4
    return b0 | (b1 << 1) | (b2 << 2) | (b4 << 4)


def bits8(v):
    return [(v >> u) & 1 for u in range(8)]


class E8Geometry:
    def __init__(self):
        self.centers = []           # class -> centre numerator (over 32)
        index = {}
        self.coord_class = [[None] * 16 for _ in range(256)]
        self.base_z = [[None] * 16 for _ in range(256)]
        self.base_norm2 = [[None] * 16 for _ in range(256)]
        self.center_num32 = [[None] * 16 for _ in range(256)]
        for tau in range(256):
            for comp in range(16):
                pt = apply_p(bits8(tau))
                pr = apply_p(bits8(rm13_rep(comp)))
                off = [pt[u] + 2 * pr[u] for u in range(8)]
                bz = [bits8(tau)[u] + 2 * bits8(rm13_rep(comp))[u]
                      for u in range(8)]
                assert apply_p(bz) == off
                self.base_z[tau][comp] = bz
                self.base_norm2[tau][comp] = sum(v * v for v in off)
                cls = []
                nums = []
                for coord in range(8):
                    c = -sum(off[u] * PRODUCT_BASIS[coord][u]
                             for u in range(8))
                    if c not in index:
                        index[c] = len(self.centers)
                        self.centers.append(c)
                    cls.append(index[c])
                    nums.append(c)
                self.coord_class[tau][comp] = cls
                self.center_num32[tau][comp] = nums
        for c in self.centers:
            assert c % 8 == 0


def e8_type(nums):
    """Mass type of a component: counts of residues 0, 1/4 or 3/4, 1/2."""
    res = [(c // 8) % 4 for c in nums]
    return (res.count(0), res.count(1) + res.count(3), res.count(2))


# ---------------------------------------------------------------------------
# Output helpers


def header_open(path, title, extra):
    f = open(path, "w")
    guard = os.path.basename(path).upper().replace(".", "_") + "__"
    f.write("/*\n * %s\n *\n" % title)
    f.write(" * Generated by e8/tables/gen_tables.py; do not edit.\n")
    f.write(" * Regenerate: %s (from e8/), or `make tables`,\n" % CMD)
    f.write(" * which fails if the committed tables differ.\n")
    for line in extra:
        f.write((" * " + line).rstrip() + "\n")
    f.write(" */\n\n#ifndef %s\n#define %s\n\n" % (guard, guard))
    return f, guard


def header_close(f, guard):
    f.write("#endif /* %s */\n" % guard)
    f.close()


def fmt_list(vals, fmt, per_line, indent):
    out = []
    for i in range(0, len(vals), per_line):
        out.append(indent + ", ".join(fmt(v) for v in vals[i:i + per_line]))
    return ",\n".join(out)


def hexw(bits):
    digits = (bits + 3) // 4
    return lambda v: "0x%0*X" % (digits, v)


# ---------------------------------------------------------------------------
# E8 tables


def e8_dists(geom, logn):
    s2 = Fraction(E8_SIGMA_SIGN[logn]) ** 2 / 32
    return [DGauss(Fraction(c, 32), s2) for c in geom.centers]


def write_e8_geom(out, geom):
    f, g = header_open(os.path.join(out, "e8_tables_geom.h"),
        "HAWK-E8-CM sampler geometry (independent of sigma and variant).",
        ["",
         "E8_TAB_CLASSES centre classes in order of first occurrence over",
         "(tau, component, coordinate); class j has centre",
         "e8_tab_center_num32[j] / 32. For each tau (internal coset label)",
         "and RM(1,3) component: the class of each product coordinate, the",
         "base block z = tau + 2 rep(component) and ||P z||^2.",
         "Requires <stdint.h>."])
    nc = len(geom.centers)
    f.write("#define E8_TAB_CLASSES   %d\n\n" % nc)
    f.write("static const int32_t e8_tab_center_num32[E8_TAB_CLASSES] = {\n")
    f.write(fmt_list(geom.centers, str, 13, "\t") + "\n};\n\n")
    f.write("static const uint8_t e8_tab_coord_class[256][16][8] = {\n")
    rows = []
    for tau in range(256):
        rows.append("\t{ /* tau %d */\n" % tau + ",\n".join(
            "\t\t{ " + ", ".join(str(v) for v in geom.coord_class[tau][c])
            + " }" for c in range(16)) + "\n\t}")
    f.write(",\n".join(rows) + "\n};\n\n")
    f.write("static const int8_t e8_tab_base_z[256][16][8] = {\n")
    rows = []
    for tau in range(256):
        rows.append("\t{ /* tau %d */\n" % tau + ",\n".join(
            "\t\t{ " + ", ".join(str(v) for v in geom.base_z[tau][c])
            + " }" for c in range(16)) + "\n\t}")
    f.write(",\n".join(rows) + "\n};\n\n")
    f.write("static const int32_t e8_tab_base_norm2[256][16] = {\n")
    f.write(",\n".join("\t{ " + ", ".join(str(v) for v in geom.base_norm2[t])
                       + " }" for t in range(256)) + "\n};\n\n")
    header_close(f, g)


def write_e8_hot(out, geom, bits, dists_by_logn):
    name = "hot%d" % bits
    T = "uint%d_t" % bits
    hx = hexw(bits)
    rows_by_logn = {}
    tail_max = 0
    for logn, dists in dists_by_logn.items():
        rows = [hot_row(dg, bits) for dg in dists]
        rows_by_logn[logn] = rows
        tail_max = max(tail_max, max(len(t) for _, t in rows))
    f, g = header_open(os.path.join(out, "e8_tables_%s.h" % name),
        "HAWK-E8-CM %s sampler tables (%d-bit thresholds)." % (name, bits),
        ["",
         "Per logn: one hot row per centre class (e8_tables_geom.h) and the",
         "component CDF of every tau. Distributions: centre num/32, sigma",
         "sigma_sign / sqrt(32), sigma_sign = %s, %s, %s (logn 8, 9, 10);"
         % (E8_SIGMA_SIGN[8], E8_SIGMA_SIGN[9], E8_SIGMA_SIGN[10]),
         "K = %d hot values. Rules: see gen_tables.py." % K_HOT,
         "Requires <stdint.h> and e8_tables_geom.h."])
    U = name.upper()
    f.write("#define E8_%s_TINY_LEN       %d\n" % (U, K_HOT))
    f.write("#define E8_%s_TAIL_LEN_MAX   %d\n\n" % (U, tail_max))
    f.write("/*\n * One centre class: tiny_len hot values and thresholds, then"
            " tail_len\n * tail values and thresholds (the last one is"
            " the maximum).\n */\n")
    f.write("typedef struct {\n\tuint8_t tiny_len;\n\tuint8_t tail_len;\n"
            "\tint8_t tiny_value[E8_%s_TINY_LEN];\n"
            "\tint8_t tail_value[E8_%s_TAIL_LEN_MAX];\n"
            "\t%s tiny_cdf[E8_%s_TINY_LEN];\n"
            "\t%s tail_cdf[E8_%s_TAIL_LEN_MAX];\n} e8_%s_row;\n\n"
            % (U, U, T, U, T, U, name))
    f.write("typedef struct {\n\tuint64_t sigma_sign_bits;\n"
            "\te8_%s_row rows[E8_TAB_CLASSES];\n"
            "\t%s component_cdf[256][16];\n} e8_%s_tables;\n\n"
            % (name, T, name))
    for logn in sorted(rows_by_logn):
        rows = rows_by_logn[logn]
        dists = dists_by_logn[logn]
        f.write("static const e8_%s_tables e8_%s_tables_logn%d = {\n"
                % (name, name, logn))
        f.write("\t0x%016XU, /* sigma_sign %s, IEEE-754 bits */\n"
                % (double_bits(E8_SIGMA_SIGN[logn]), E8_SIGMA_SIGN[logn]))
        f.write("\t{\n")
        rtxt = []
        for j, (tiny, tail) in enumerate(rows):
            tv = [v for v, _ in tiny] + [0] * (K_HOT - len(tiny))
            tq = [q for _, q in tiny] + [0] * (K_HOT - len(tiny))
            lv = [v for v, _ in tail] + [0] * (tail_max - len(tail))
            lq = [q for _, q in tail] + [0] * (tail_max - len(tail))
            rtxt.append(
                "\t\t{ /* class %d, centre %d/32 */\n" % (j, geom.centers[j])
                + "\t\t\t%d, %d,\n" % (len(tiny), len(tail))
                + "\t\t\t{ " + ", ".join(str(v) for v in tv) + " },\n"
                + "\t\t\t{ " + ", ".join(str(v) for v in lv) + " },\n"
                + "\t\t\t{\n" + fmt_list(tq, hx, 3, "\t\t\t\t") + "\n\t\t\t},\n"
                + "\t\t\t{\n" + fmt_list(lq, hx, 3, "\t\t\t\t") + "\n\t\t\t}\n"
                + "\t\t}")
        f.write(",\n".join(rtxt) + "\n\t},\n\t{\n")
        comp = []
        totals = [dg.total for dg in dists]
        full = (1 << bits) - 1
        for tau in range(256):
            masses = []
            for c in range(16):
                m = D(1)
                for cls in geom.coord_class[tau][c]:
                    m *= totals[cls]
                masses.append(m)
            total = sum(masses, D(0))
            acc = D(0)
            qs = []
            for c in range(16):
                acc += masses[c]
                qs.append(min(floor_scaled(acc, total, bits,
                    "component tau=%d" % tau), full))
            qs[15] = full
            comp.append("\t\t{ /* tau %d */\n" % tau
                        + fmt_list(qs, hx, 4 if bits == 64 else 6, "\t\t\t")
                        + "\n\t\t}")
        f.write(",\n".join(comp) + "\n\t}\n};\n\n")
    header_close(f, g)


def write_e8_ref(out, geom, dists_by_logn):
    f, g = header_open(os.path.join(out, "e8_tables_ref.h"),
        "HAWK-E8-CM ref sampler tables (78-bit, constant-time full scans).",
        ["",
         "Per logn: a two-sided survival table per centre residue r/4",
         "(class with centre num/32 = k/4 uses residue k mod 4, shifted by",
         "floor(k/4)), padded with zeros to E8_REF_1D_LEN, and the",
         "canonical-slot component tables (row classes, per-tau row class",
         "and slot-to-component permutation, 4 bits per slot). 78-bit",
         "values are split as hi (15 bits) || lo (63 bits), as in HAWK.",
         "Requires <stdint.h>."])
    per_logn = {}
    m_max = 0
    for logn, _ in dists_by_logn.items():
        s2 = Fraction(E8_SIGMA_SIGN[logn]) ** 2 / 32
        res = []
        for r in range(4):
            lo, tab = survival_table(DGauss(Fraction(r, 4), s2))
            res.append((lo, tab))
            m_max = max(m_max, len(tab))
        per_logn[logn] = res
    # Component row classes (independent of sigma).
    types = [[e8_type(geom.center_num32[t][c]) for c in range(16)]
             for t in range(256)]
    row_classes = []
    row_of_tau = []
    perm_of_tau = []
    for t in range(256):
        order = sorted(range(16), key=lambda c: (types[t][c], c))
        key = tuple(types[t][c] for c in order)
        if key not in row_classes:
            row_classes.append(key)
        row_of_tau.append(row_classes.index(key))
        perm = 0
        for slot, c in enumerate(order):
            perm |= c << (4 * slot)
        perm_of_tau.append(perm)
    f.write("#define E8_REF_1D_LEN       %d\n" % m_max)
    f.write("#define E8_REF_ROW_CLASSES  %d\n\n" % len(row_classes))
    f.write("typedef struct {\n\tuint64_t sigma_sign_bits;\n"
            "\tint8_t lo_value[4];\n"
            "\tuint16_t hi[4][E8_REF_1D_LEN];\n"
            "\tuint64_t lo[4][E8_REF_1D_LEN];\n"
            "\tuint16_t slot_hi[E8_REF_ROW_CLASSES][15];\n"
            "\tuint64_t slot_lo[E8_REF_ROW_CLASSES][15];\n"
            "} e8_ref_tables;\n\n")
    f.write("/* Row class of each tau. */\n")
    f.write("static const uint8_t e8_ref_row_class[256] = {\n")
    f.write(fmt_list(row_of_tau, str, 16, "\t") + "\n};\n\n")
    f.write("/* Slot-to-component permutation of each tau (slot j in bits"
            " 4j..4j+3). */\n")
    f.write("static const uint64_t e8_ref_slot_perm[256] = {\n")
    f.write(fmt_list(perm_of_tau, hexw(64), 4, "\t") + "\n};\n\n")
    for logn in sorted(per_logn):
        res = per_logn[logn]
        # Normalisers by residue type (0, 1/4 ~ 3/4, 1/2).
        s2 = Fraction(E8_SIGMA_SIGN[logn]) ** 2 / 32
        z = {0: DGauss(Fraction(0), s2).total,
             1: DGauss(Fraction(1, 4), s2).total,
             2: DGauss(Fraction(1, 2), s2).total}
        z3 = DGauss(Fraction(3, 4), s2).total
        if abs(z3 - z[1]) > MARGIN * z[1]:
            sys.exit("gen_tables: residue 1/4 and 3/4 masses differ")
        slot_hi = []
        slot_lo = []
        for key in row_classes:
            masses = [z[0] ** a * z[1] ** b * z[2] ** c for (a, b, c) in key]
            total = sum(masses, D(0))
            surv = []
            for j in range(15):
                surv.append(floor_scaled(sum(masses[j + 1:], D(0)), total,
                    78, "ref slot"))
            slot_hi.append([v >> 63 for v in surv])
            slot_lo.append([v & ((1 << 63) - 1) for v in surv])
        f.write("static const e8_ref_tables e8_ref_tables_logn%d = {\n" % logn)
        f.write("\t0x%016XU, /* sigma_sign %s, IEEE-754 bits */\n"
                % (double_bits(E8_SIGMA_SIGN[logn]), E8_SIGMA_SIGN[logn]))
        f.write("\t{ " + ", ".join(str(lo) for lo, _ in res)
                + " }, /* lowest value per residue */\n")
        his = []
        los = []
        for lo, tab in res:
            tab = tab + [0] * (m_max - len(tab))
            his.append("\t\t{ " + ", ".join("0x%04X" % (v >> 63) for v in tab)
                       + " }")
            los.append("\t\t{\n" + fmt_list([v & ((1 << 63) - 1) for v in tab],
                                            hexw(64), 4, "\t\t\t") + "\n\t\t}")
        f.write("\t{\n" + ",\n".join(his) + "\n\t},\n")
        f.write("\t{\n" + ",\n".join(los) + "\n\t},\n")
        f.write("\t{\n" + ",\n".join(
            "\t\t{ " + ", ".join("0x%04X" % v for v in row) + " }"
            for row in slot_hi) + "\n\t},\n")
        f.write("\t{\n" + ",\n".join(
            "\t\t{\n" + fmt_list(row, hexw(64), 4, "\t\t\t") + "\n\t\t}"
            for row in slot_lo) + "\n\t}\n};\n\n")
    header_close(f, g)


# ---------------------------------------------------------------------------
# HAWK tables


def hawk_dists(logn):
    s2 = Fraction(HAWK_SIGMA_SIGN[logn]) ** 2
    # x = 2y + p, y ~ D(-p/2, sigma_sign).
    return [DGauss(Fraction(-p, 2), s2) for p in (0, 1)]


def hawk_ref_table(logn):
    d0, d1 = hawk_dists(logn)
    scale = D(2) ** 78
    out = []
    k = 0
    while True:
        # P0(|X0| >= 2(k+1)): |2y| >= 2k+2  <=> |y| >= k+1.
        p0 = sum((w for y, w in d0.w.items() if abs(2 * y) >= 2 * (k + 1)),
                 D(0))
        # P1(|X1| >= 2k+3): |2y+1| >= 2k+3.
        p1 = sum((w for y, w in d1.w.items() if abs(2 * y + 1) >= 2 * k + 3),
                 D(0))
        t0 = floor_scaled(p0, d0.total, 78)
        t1 = floor_scaled(p1, d1.total, 78)
        if t0 == 0 and t1 == 0:
            break
        out += [t0, t1]
        k += 1
    hi = [v >> 63 for v in out]
    lo = [v & ((1 << 63) - 1) for v in out]
    hi_len = len(hi)
    while hi_len > 0 and hi[hi_len - 1] == 0 and hi[hi_len - 2] == 0:
        hi_len -= 2
    return hi[:hi_len], lo


def parse_upstream(path):
    import re
    text = open(path).read()
    tabs = {}
    for logn, n in ((8, 256), (9, 512), (10, 1024)):
        for part in ("hi", "lo"):
            m = re.search(r"sig_gauss_%s_Hawk_%d\[\] = \{(.*?)\};" % (part, n),
                          text, re.S)
            if m is None:
                sys.exit("gen_tables: cannot find upstream table")
            tabs[(logn, part)] = [int(v, 16) for v in
                                  re.findall(r"0x[0-9A-Fa-f]+", m.group(1))]
    return tabs


def write_hawk_ref(out, upstream):
    f, g = header_open(os.path.join(out, "hawk_tables_ref.h"),
        "HAWK ref sampler tables (upstream sig_gauss() format, 78 bits).",
        ["",
         "T[2k] = floor(P0(|X0| >= 2(k+1)) 2^78), T[2k+1] = floor(P1(|X1| >=",
         "2k+3) 2^78), X0 in 2Z, X1 in 2Z + 1, sigma 2 sigma_sign with",
         "sigma_sign = %s, %s, %s; split as hi (15 bits) || lo (63 bits)."
         % (HAWK_SIGMA_SIGN[8], HAWK_SIGMA_SIGN[9], HAWK_SIGMA_SIGN[10]),
         "Equal to upstream's tables (checked by the generator and by",
         "test_hawk_variants)."])
    for logn in (8, 9, 10):
        hi, lo = hawk_ref_table(logn)
        if upstream is not None and (hi != upstream[(logn, "hi")]
                                     or lo != upstream[(logn, "lo")]):
            sys.exit("gen_tables: HAWK ref table logn=%d differs from"
                     " upstream" % logn)
        f.write("static const uint16_t hawk_ref_hi_logn%d[%d] = {\n"
                % (logn, len(hi)))
        f.write(fmt_list(hi, lambda v: "0x%04X" % v, 2, "\t") + "\n};\n")
        f.write("static const uint64_t hawk_ref_lo_logn%d[%d] = {\n"
                % (logn, len(lo)))
        f.write(fmt_list(lo, hexw(64), 2, "\t") + "\n};\n\n")
    header_close(f, g)


def write_hawk_hot(out, bits):
    name = "hot%d" % bits
    hx = hexw(bits)
    rows_by_logn = {}
    tail_max = 0
    for logn in (8, 9, 10):
        rows = []
        for p, dg in enumerate(hawk_dists(logn)):
            tiny, tail = hot_row(dg, bits)
            rows.append(([(2 * y + p, q) for y, q in tiny],
                         [(2 * y + p, q) for y, q in tail]))
        rows_by_logn[logn] = rows
        tail_max = max(tail_max, max(len(t) for _, t in rows))
    f, g = header_open(os.path.join(out, "hawk_tables_%s.h" % name),
        "HAWK %s sampler tables (%d-bit thresholds)." % (name, bits),
        ["",
         "Per logn: one hot row per parity p of the target (x in 2Z + p,",
         "x = 2y + p with y ~ D(-p/2, sigma_sign), sigma_sign = %s, %s, %s);"
         % (HAWK_SIGMA_SIGN[8], HAWK_SIGMA_SIGN[9], HAWK_SIGMA_SIGN[10]),
         "values are x. K = %d hot values; rules: see gen_tables.py."
         % K_HOT,
         "Requires <stdint.h>."])
    U = name.upper()
    T = "uint%d_t" % bits
    f.write("#define HAWK_%s_TINY_LEN       %d\n" % (U, K_HOT))
    f.write("#define HAWK_%s_TAIL_LEN_MAX   %d\n\n" % (U, tail_max))
    f.write("typedef struct {\n\tuint8_t tiny_len;\n\tuint8_t tail_len;\n"
            "\tint8_t tiny_value[HAWK_%s_TINY_LEN];\n"
            "\tint8_t tail_value[HAWK_%s_TAIL_LEN_MAX];\n"
            "\t%s tiny_cdf[HAWK_%s_TINY_LEN];\n"
            "\t%s tail_cdf[HAWK_%s_TAIL_LEN_MAX];\n} hawk_%s_row;\n\n"
            % (U, U, T, U, T, U, name))
    f.write("typedef struct {\n\thawk_%s_row rows[2];\n} hawk_%s_tables;\n\n"
            % (name, name))
    for logn in (8, 9, 10):
        f.write("static const hawk_%s_tables hawk_%s_tables_logn%d = { {\n"
                % (name, name, logn))
        rtxt = []
        for p, (tiny, tail) in enumerate(rows_by_logn[logn]):
            tv = [v for v, _ in tiny] + [0] * (K_HOT - len(tiny))
            tq = [q for _, q in tiny] + [0] * (K_HOT - len(tiny))
            lv = [v for v, _ in tail] + [0] * (tail_max - len(tail))
            lq = [q for _, q in tail] + [0] * (tail_max - len(tail))
            rtxt.append(
                "\t{ /* parity %d */\n" % p
                + "\t\t%d, %d,\n" % (len(tiny), len(tail))
                + "\t\t{ " + ", ".join(str(v) for v in tv) + " },\n"
                + "\t\t{ " + ", ".join(str(v) for v in lv) + " },\n"
                + "\t\t{\n" + fmt_list(tq, hx, 3, "\t\t\t") + "\n\t\t},\n"
                + "\t\t{\n" + fmt_list(lq, hx, 3, "\t\t\t") + "\n\t\t}\n"
                + "\t}")
        f.write(",\n".join(rtxt) + "\n} };\n\n")
    header_close(f, g)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--hawk-src", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "..", "src",
        "hawk_sign.c"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    upstream = parse_upstream(args.hawk_src) if os.path.exists(args.hawk_src) \
        else None
    if upstream is None:
        sys.exit("gen_tables: upstream %s not found" % args.hawk_src)
    geom = E8Geometry()
    dists = {logn: e8_dists(geom, logn) for logn in (8, 9, 10)}
    write_e8_geom(args.out, geom)
    write_e8_hot(args.out, geom, 64, dists)
    write_e8_hot(args.out, geom, 32, dists)
    write_e8_ref(args.out, geom, dists)
    write_hawk_ref(args.out, upstream)
    write_hawk_hot(args.out, 64)
    write_hawk_hot(args.out, 32)
    kinds = {}
    for what, bits, _ in NEAR_INTEGER:
        key = (what.split(" tau=")[0], bits)
        kinds[key] = kinds.get(key, 0) + 1
    for (what, bits), count in sorted(kinds.items()):
        print("gen_tables: %d exact %d-bit thresholds (%s)"
              % (count, bits, what))


if __name__ == "__main__":
    main()
