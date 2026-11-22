#!/usr/bin/env python3
"""
Small-degree experiments for the Galois key recovery attack on HAWK-E8-CM
(paper Sec. 5.4, sec:attack; docs/galois_attack.md). Python standard
library only, exact integer arithmetic throughout.

The attack (Straznickas and Weis, "HAWK-n Key Recovery Reduces to SVP in
Dimension n/2+1", 2026) builds from the public form Q the cocycle lattice

    Lambda(Q) = { Y in M_2(R_n) : tau(Y) = adj Y,  Q Y = sigma(Y)^T tau(Q) }

with the form Q^tau(Y) = Tr_F(det Y), finds V_tau = B^-1 tau(B) as a
shortest vector, and descends to a key. For Q_E8 = B^* S_n B this script
checks, from the public key alone:

  - L12 = P sl2(O8) P^-1 (Frobenius coefficient norm) is D6 + D6
    (prop:cocycleclass (3));
  - Lambda(Q_E8) is sqrt(n/4) (Z + sqrt2 Z^(n/4-1) + D6^(n/8)): determinant
    2^(n/2-1) after scaling by 4/n, one pair of norm 1 (= +-V_tau), 8n-2
    vectors of norm 2 whose root systems are (n/4-1) A1 + (n/8) D6, and the
    determinant of the spanned sublattice equals the lattice's (a class
    certificate); for the HAWK form B^* B of the same basis, n/2+1 pairs of
    norm 1;
  - the descent lattice M_V = {x : tau(x) = V_tau^-1 x} under T/(4n),
    T(x, x') = Tr(x^* Q_E8 x'), has determinant 1 and (n <= 32) n/8
    orthogonal E8 root systems (lem:descent);
  - the tau-tower (attack, Lemma D.4): an O'-basis of M_V by a Bezout step
    (an integer linear system), size reduced under the Q_E8 form, gives a
    HAWK-E8-CM key of degree n/2; recursing to degree 4 (rank 8, 2E8,
    solved from its 240 minimal vectors) recovers B' in GL2(R_n) with
    B'^* S_n B' = Q_E8. No descent lattice is reduced.

Keys: f, g centred binomial (sum of eta differences of two bits), F, G by
an exact NTRUSolve (q = 1), i.e. HawkKeyGen's structure at small n (HAWK
has no parameter sets below n = 256).

Usage:
  python3 -I galois_attack.py --self-test
  python3 -I galois_attack.py --n 8,16,32 --keys 5 --out-dir results/galois_attack
  python3 -I galois_attack.py --n 64 --keys 2 --no-certificate ...

Exits non-zero if any prediction fails.
"""

import argparse
import csv
import itertools
import math
import os
import random
import sys
import time
from collections import Counter
from fractions import Fraction as Fr

# ---------------------------------------------------------------------------
# Arithmetic in R_n = Z[X]/(X^n + 1) (lists of n coefficients) and K_n.


def mul(a, b):
    n = len(a)
    r = [0] * n
    for i, x in enumerate(a):
        if x == 0:
            continue
        for j, y in enumerate(b):
            if y == 0:
                continue
            t = i + j
            if t < n:
                r[t] += x * y
            else:
                r[t - n] -= x * y
    return r


def add(a, b):
    return [x + y for x, y in zip(a, b)]


def sub(a, b):
    return [x - y for x, y in zip(a, b)]


def neg(a):
    return [-x for x in a]


def scal(c, a):
    return [c * x for x in a]


def one(n):
    return [1] + [0] * (n - 1)


def zero(n):
    return [0] * n


def adj(a):
    """The involution X -> X^-1 (the paper's star)."""
    n = len(a)
    r = [0] * n
    r[0] = a[0]
    for i in range(1, n):
        r[n - i] = -a[i]
    return r


def tau(a):
    """X -> -X."""
    return [x if i % 2 == 0 else -x for i, x in enumerate(a)]


def sigma(a):
    return adj(tau(a))


def monom(n, e, c=1):
    r = [0] * n
    e %= 2 * n
    if e < n:
        r[e] = c
    else:
        r[e - n] = -c
    return r


def galois_norm(a):
    """N_{K_n/K_{n/2}}(a) = a(x) a(-x), as a polynomial in x^2."""
    return mul(a, tau(a))[0::2]


def lift(a):
    """a(x) -> a(x^2), R_{n/2} -> R_n."""
    r = [0] * (2 * len(a))
    r[0::2] = a
    return r


def even_part(a):
    """An element of O' = Z[X^2] in R_n, as an element of R_{n/2}."""
    assert not any(a[1::2])
    return a[0::2]


def inv_K(a):
    """Inverse in K_n (Fractions), through the tower of field norms."""
    if len(a) == 1:
        return [Fr(1) / Fr(a[0])]
    return mul(tau(a), lift(inv_K(galois_norm(a))))


def is_unit(a):
    while len(a) > 1:
        a = galois_norm(a)
    return abs(a[0]) == 1


def xgcd(a, b):
    x0, y0, x1, y1 = 1, 0, 0, 1
    while b:
        q = a // b
        a, b = b, a - q * b
        x0, x1 = x1, x0 - q * x1
        y0, y1 = y1, y0 - q * y1
    return a, x0, y0


# 2 x 2 matrices over R_n.


def mmul(A, B):
    return [[add(mul(A[i][0], B[0][j]), mul(A[i][1], B[1][j])) for j in range(2)]
            for i in range(2)]


def mstar(A):
    return [[adj(A[j][i]) for j in range(2)] for i in range(2)]


def mmap(f, A):
    return [[f(A[i][j]) for j in range(2)] for i in range(2)]


def mT(A):
    return [[A[j][i] for j in range(2)] for i in range(2)]


def madj(A):
    return [[A[1][1], neg(A[0][1])], [neg(A[1][0]), A[0][0]]]


def mdet(A):
    return sub(mul(A[0][0], A[1][1]), mul(A[0][1], A[1][0]))


def mvec(A, x):
    return [add(mul(A[i][0], x[0]), mul(A[i][1], x[1])) for i in range(2)]


def herm(Q, x, y):
    """x^* Q y in R_n."""
    Qy = mvec(Q, y)
    return add(mul(adj(x[0]), Qy[0]), mul(adj(x[1]), Qy[1]))


def P_matrix(n):
    """P_n of eq. (Pn): Y = X^(n/4)."""
    k = n // 4
    Y = lambda e: monom(n, k * e)
    a = scal(2, add(Y(0), Y(1)))
    b = add(add(sub(Y(0), Y(1)), Y(2)), Y(3))
    c = add(add(add(Y(0), Y(1)), Y(2)), Y(3))
    return [[a, b], [zero(n), c]]


def S_matrix(n):
    P = P_matrix(n)
    return mmul(mstar(P), P)


# ---------------------------------------------------------------------------
# Key generation (HawkKeyGen's structure, q = 1).


def ntru_reduce(f, g, F, G):
    while True:
        den = add(mul(f, adj(f)), mul(g, adj(g)))
        num = add(mul(F, adj(f)), mul(G, adj(g)))
        k = [round(x) for x in mul(num, inv_K(den))]
        if not any(k):
            return F, G
        F = sub(F, mul(k, f))
        G = sub(G, mul(k, g))


def ntru_solve(f, g):
    """F, G with f G - g F = 1, or None."""
    if len(f) == 1:
        d, u, v = xgcd(f[0], g[0])
        if abs(d) != 1:
            return None
        return [-v * d], [u * d]
    r = ntru_solve(galois_norm(f), galois_norm(g))
    if r is None:
        return None
    Fp, Gp = r
    return ntru_reduce(f, g, mul(lift(Fp), tau(g)), mul(lift(Gp), tau(f)))


def keygen(n, seed, eta=2):
    rng = random.Random(seed)
    cbd = lambda: sum(rng.randint(0, 1) - rng.randint(0, 1) for _ in range(eta))
    while True:
        f = [cbd() for _ in range(n)]
        g = [cbd() for _ in range(n)]
        r = ntru_solve(f, g)
        if r is None:
            continue
        F, G = r
        assert sub(mul(f, G), mul(g, F)) == one(n)
        return [[f, F], [g, G]]


# ---------------------------------------------------------------------------
# Lattice tools: integer kernel, integral LLL on a Gram matrix (Cohen,
# Alg. 2.6.7), Fincke-Pohst enumeration, exact determinants.


def int_kernel(A):
    """Z-basis of {x in Z^m : A x = 0}, A a list of integer rows."""
    r, m = len(A), len(A[0])
    rows = [[A[i][j] for i in range(r)] + [1 if t == j else 0 for t in range(m)]
            for j in range(m)]
    piv = 0
    for col in range(r):
        while True:
            nz = [i for i in range(piv, m) if rows[i][col] != 0]
            if not nz:
                break
            i0 = min(nz, key=lambda i: abs(rows[i][col]))
            rows[piv], rows[i0] = rows[i0], rows[piv]
            done = True
            for i in range(piv + 1, m):
                if rows[i][col]:
                    q = rows[i][col] // rows[piv][col]
                    rows[i] = [a - q * b for a, b in zip(rows[i], rows[piv])]
                    if rows[i][col]:
                        done = False
            if done:
                piv += 1
                break
    return [row[r:] for row in rows[piv:]]


def lll_gram(G, dnum=99, dden=100):
    """LLL (delta = dnum/dden) on an integer positive definite Gram matrix.
    Returns (H, G') with rows of H the new basis in old coordinates."""
    n = len(G)
    G = [list(r) for r in G]
    H = [[1 if i == j else 0 for j in range(n)] for i in range(n)]
    d = [0] * (n + 1)
    lam = [[0] * n for _ in range(n)]
    d[0], d[1] = 1, G[0][0]
    k, kmax = 1, 0

    def incgs(k):
        for j in range(k + 1):
            u = G[k][j]
            for i in range(j):
                u = (d[i + 1] * u - lam[k][i] * lam[j][i]) // d[i]
            if j < k:
                lam[k][j] = u
            else:
                d[k + 1] = u

    def red(k, l):
        if 2 * abs(lam[k][l]) > d[l + 1]:
            q = (2 * lam[k][l] + d[l + 1]) // (2 * d[l + 1])
            H[k] = [a - q * b for a, b in zip(H[k], H[l])]
            gkk = G[k][k] - 2 * q * G[k][l] + q * q * G[l][l]
            for t in range(n):
                if t != k:
                    G[k][t] -= q * G[l][t]
                    G[t][k] = G[k][t]
            G[k][k] = gkk
            lam[k][l] -= q * d[l + 1]
            for i in range(l):
                lam[k][i] -= q * lam[l][i]

    def swap(k):
        H[k], H[k - 1] = H[k - 1], H[k]
        G[k], G[k - 1] = G[k - 1], G[k]
        for r in G:
            r[k], r[k - 1] = r[k - 1], r[k]
        for j in range(k - 1):
            lam[k][j], lam[k - 1][j] = lam[k - 1][j], lam[k][j]
        l = lam[k][k - 1]
        B = (d[k - 1] * d[k + 1] + l * l) // d[k]
        for i in range(k + 1, kmax + 1):
            t = lam[i][k]
            lam[i][k] = (d[k + 1] * lam[i][k - 1] - l * t) // d[k]
            lam[i][k - 1] = (B * t + l * lam[i][k]) // d[k + 1]
        d[k] = B

    while k < n:
        if k > kmax:
            kmax = k
            incgs(k)
        while True:
            red(k, k - 1)
            if dden * d[k + 1] * d[k - 1] < dnum * d[k] * d[k] - dden * lam[k][k - 1] ** 2:
                swap(k)
                k = max(1, k - 1)
            else:
                for l in range(k - 2, -1, -1):
                    red(k, l)
                k += 1
                break
    return H, G


def gso(G):
    n = len(G)
    mu = [[Fr(0)] * n for _ in range(n)]
    B = [Fr(0)] * n
    for i in range(n):
        for j in range(i):
            mu[i][j] = (G[i][j] - sum(mu[j][l] * mu[i][l] * B[l] for l in range(j))) / B[j]
        B[i] = G[i][i] - sum(mu[i][l] ** 2 * B[l] for l in range(i))
    return mu, B


def short_vectors(G, bound):
    """All x != 0 with x^T G x <= bound, one of each pair +-x, as
    (norm, x). G: an (LLL reduced) Gram matrix of Fractions or ints. Floats
    prune, the final test is exact."""
    n = len(G)
    mu, B = gso([[Fr(v) for v in r] for r in G])
    muf = [[float(v) for v in r] for r in mu]
    Bf = [float(v) for v in B]
    found, x = [], [0] * n

    def rec(i, rem):
        c = -sum(muf[j][i] * x[j] for j in range(i + 1, n))
        r = math.sqrt(max(rem, 0) / Bf[i])
        for v in range(math.ceil(c - r - 1e-9), math.floor(c + r + 1e-9) + 1):
            x[i] = v
            nr = rem - Bf[i] * (v - c) ** 2
            if nr < -1e-9:
                continue
            if i == 0:
                if any(x):
                    found.append(list(x))
            else:
                rec(i - 1, nr)
        x[i] = 0

    rec(n - 1, float(bound) + 1e-9)
    out = []
    for v in found:
        nv = sum(v[i] * G[i][j] * v[j] for i in range(n) if v[i] for j in range(n) if v[j])
        if nv <= bound and next(t for t in v if t) > 0:
            out.append((nv, v))
    return out


def det_exact(M):
    M = [[Fr(x) for x in r] for r in M]
    n, d = len(M), Fr(1)
    for i in range(n):
        p = next((r for r in range(i, n) if M[r][i] != 0), None)
        if p is None:
            return Fr(0)
        if p != i:
            M[i], M[p] = M[p], M[i]
            d = -d
        d *= M[i][i]
        for r in range(i + 1, n):
            f = M[r][i] / M[i][i]
            if f:
                M[r] = [a - f * b for a, b in zip(M[r], M[i])]
    return d


def rank_exact(vs):
    M = [[Fr(x) for x in v] for v in vs]
    r = 0
    for c in range(len(M[0]) if M else 0):
        p = next((i for i in range(r, len(M)) if M[i][c] != 0), None)
        if p is None:
            continue
        M[r], M[p] = M[p], M[r]
        for i in range(len(M)):
            if i != r and M[i][c] != 0:
                f = M[i][c] / M[r][c]
                M[i] = [a - f * b for a, b in zip(M[i], M[r])]
        r += 1
    return r


def root_systems(G, vecs):
    """Split the vectors (both signs given) into components of the graph
    'nonzero inner product'; return Counter of (size, rank)."""
    n = len(G)
    GV = [[sum(G[i][j] * u[j] for j in range(n) if u[j]) for i in range(n)] for u in vecs]
    N = len(vecs)
    par = list(range(N))

    def find(x):
        while par[x] != x:
            par[x] = par[par[x]]
            x = par[x]
        return x

    for i in range(N):
        for j in range(i + 1, N):
            if sum(a * b for a, b in zip(GV[i], vecs[j]) if a) != 0:
                par[find(i)] = find(j)
    comps = {}
    for i in range(N):
        comps.setdefault(find(i), []).append(vecs[i])
    return Counter((len(c), rank_exact(c)) for c in comps.values())


# Root lattice determinants of the irreducible simply laced systems that
# can occur here, by (number of roots, rank): A1, D6, E8.
ROOT_DET = {(2, 1): 2, (60, 6): 4, (240, 8): 1}


# ---------------------------------------------------------------------------
# The attack's lattices.


def cocycle_lattice(Q):
    """Basis (as matrices) and integer Gram of (4/n) Q^tau of Lambda(Q).
    Unknowns: a (n coefficients), the odd coefficients of b and c; d = tau(a)
    makes (C1) hold, (C2) is solved as an integer kernel."""
    n = len(Q[0][0])
    m = 2 * n

    def Yof(v):
        a = list(v[:n])
        b, c = [0] * n, [0] * n
        for t in range(n // 2):
            b[2 * t + 1] = v[n + t]
            c[2 * t + 1] = v[n + n // 2 + t]
        return [[a, b], [c, tau(a)]]

    tQ = mmap(tau, Q)
    cols = []
    for u in range(m):
        e = [0] * m
        e[u] = 1
        Y = Yof(e)
        D = [[sub(x, y) for x, y in zip(r1, r2)]
             for r1, r2 in zip(mmul(Q, Y), mmul(mT(mmap(sigma, Y)), tQ))]
        cols.append([x for i in range(2) for j in range(2) for x in D[i][j]])
    A = [[cols[u][r] for u in range(m)] for r in range(4 * n)]
    Ys = [Yof(v) for v in int_kernel(A)]

    def form2(Y1, Y2):  # 2 * (4/n) * bilinear form of Tr_F det
        (a, b), (c, d) = Y1
        (a2, b2), (c2, d2) = Y2
        return sub(add(mul(a, d2), mul(a2, d)), add(mul(b, c2), mul(b2, c)))[0]

    G2 = [[form2(x, y) for y in Ys] for x in Ys]
    if all(v % 2 == 0 for r in G2 for v in r):
        return Ys, [[v // 2 for v in r] for r in G2], 1
    return Ys, G2, 2  # Gram = G2 / 2


def combine(coeffs, Ys, n):
    M = [[zero(n), zero(n)], [zero(n), zero(n)]]
    for c, Y in zip(coeffs, Ys):
        if c:
            M = [[add(M[i][j], scal(c, Y[i][j])) for j in range(2)] for i in range(2)]
    return M


def is_identity_mod2(M):
    return all(x % 2 == (1 if (i == j and t == 0) else 0)
               for i in range(2) for j in range(2) for t, x in enumerate(M[i][j]))


def find_vtau(Q):
    """Public: LLL on the cocycle lattice, then all vectors of minimal norm
    n/4 (norm 1 after scaling) that pass the parity test Y = I mod 2."""
    n = len(Q[0][0])
    Ys, G, den = cocycle_lattice(Q)
    H, Gr = lll_gram(G)
    rank = len(G)
    sv = short_vectors(Gr, den)  # scaled norm <= 1
    cands = []
    for _, u in sv:
        coeffs = [sum(u[i] * H[i][j] for i in range(rank)) for j in range(rank)]
        M = combine(coeffs, Ys, n)
        if is_identity_mod2(M):
            cands.append(M)
    return cands, len(sv)


def descent_lattice(Q, V):
    """M_V = {x : tau(x) = V^-1 x}; Gram of T/(4n) = (x^* Q x')[0] / 4."""
    n = len(Q[0][0])
    Vi = madj(V)
    cols = []
    for u in range(2 * n):
        x = [zero(n), zero(n)]
        x[u // n][u % n] = 1
        vx = mvec(Vi, x)
        cols.append(sub(tau(x[0]), vx[0]) + sub(tau(x[1]), vx[1]))
    A = [[cols[u][r] for u in range(2 * n)] for r in range(2 * n)]
    xs = [[v[:n], v[n:]] for v in int_kernel(A)]
    G = [[herm(Q, x, y)[0] for y in xs] for x in xs]
    assert all(v % 4 == 0 for r in G for v in r)
    return xs, [[v // 4 for v in r] for r in G]


def bezout_basis(Q, V):
    """Attack, Lemma D.4: M_V is generated over O' = Z[X^2] by the columns of
    (I + V)/2 and (I - V)/(2X); fix x among them, solve 1 = sum c_j mu(x, g_j)
    over O' as an integer linear system, y = sum c_j g_j. Then size reduce
    (x, y) under the Q-Hermitian form. Returns U = [x y], det U = 1."""
    n = len(Q[0][0])
    I = [[one(n), zero(n)], [zero(n), one(n)]]
    Xinv = monom(n, -1)
    Pp = [[add(I[i][j], V[i][j]) for j in range(2)] for i in range(2)]
    Pm = [[sub(I[i][j], V[i][j]) for j in range(2)] for i in range(2)]
    assert all(x % 2 == 0 for r in Pp + Pm for e in r for x in e)
    P1 = mmap(lambda e: [x // 2 for x in e], Pp)
    P2 = mmap(lambda e: mul([x // 2 for x in e], Xinv), Pm)
    gens = [[P[0][j], P[1][j]] for P in (P1, P2) for j in range(2)]
    mu = lambda x, y: sub(mul(x[0], y[1]), mul(x[1], y[0]))
    half = n // 2
    cands = gens + [[add(g[0], h[0]), add(g[1], h[1])] for g, h in itertools.combinations(gens, 2)]
    for x in cands:
        ms = [mu(x, g) for g in gens]
        assert not any(any(m[1::2]) for m in ms)  # mu takes values in O'
        cols, meta = [], []
        for j, m in enumerate(ms):
            for e in range(half):
                cols.append(mul(m, monom(n, 2 * e))[0::2])
                meta.append((j, e))
        A = [[c[r] for c in cols] + [-(1 if r == 0 else 0)] for r in range(half)]
        K = int_kernel(A)
        g, comb = 0, [0] * len(K)
        for i, l in enumerate(k[-1] for k in K):
            if l == 0:
                continue
            if g == 0:
                g, comb = l, [0] * len(K)
                comb[i] = 1
                continue
            dd, u, v = xgcd(g, l)
            comb = [u * c for c in comb]
            comb[i] += v
            g = dd
        if abs(g) != 1:
            continue  # x is not unimodular; try the next candidate
        sol = [sum(comb[i] * K[i][t] for i in range(len(K))) * g for t in range(len(cols) + 1)]
        y = [zero(n), zero(n)]
        for (j, e), c in zip(meta, sol[:-1]):
            if c:
                y = [add(y[0], scal(c, mul(monom(n, 2 * e), gens[j][0]))),
                     add(y[1], scal(c, mul(monom(n, 2 * e), gens[j][1])))]
        assert mu(x, y) == one(n)
        nrm = lambda u: herm(Q, u, u)[0]
        for _ in range(500):
            a, b = (x, y) if nrm(x) <= nrm(y) else (y, x)
            q = mul(herm(Q, a, b), inv_K(herm(Q, a, a)))
            k = [round(t) if i % 2 == 0 else 0 for i, t in enumerate(q)]
            b2 = [sub(b[0], mul(k, a[0])), sub(b[1], mul(k, a[1]))]
            if not any(k) or nrm(b2) >= nrm(b):
                break
            if a is x:
                y = b2
            else:
                x = b2
        assert mu(x, y) == one(n)
        return [[x[0], y[0]], [x[1], y[1]]]
    raise RuntimeError('no unimodular generator found')


def solve_bottom(Q4):
    """Degree 4 (O8, X = Y): U with U^* Q4 U = S_4 from the 240 minimal
    vectors of the rank-8 lattice (O8^2, Q4) ~ 2E8."""
    S = S_matrix(4)
    basis = [[monom(4, e) if i == 0 else zero(4), monom(4, e) if i == 1 else zero(4)]
             for i in range(2) for e in range(4)]
    G = [[herm(Q4, x, y)[0] for y in basis] for x in basis]
    H, Gr = lll_gram(G)
    vecs = []
    for _, u in short_vectors(Gr, 8):
        c = [sum(u[i] * H[i][j] for i in range(8)) for j in range(8)]
        vecs += [[c[:4], c[4:]], [neg(c[:4]), neg(c[4:])]]
    assert len(vecs) == 240
    for u1 in vecs:
        if herm(Q4, u1, u1) != S[0][0]:
            continue
        for u2 in vecs:
            if herm(Q4, u2, u2) == S[1][1] and herm(Q4, u1, u2) == S[0][1]:
                U = [[u1[0], u2[0]], [u1[1], u2[1]]]
                if is_unit(mdet(U)):
                    return U
    raise RuntimeError('degree-4 base case failed')


def recover(Q, log):
    """B' in GL2(R_n) with B'^* S_n B' = Q, by the tau-tower."""
    n = len(Q[0][0])
    if n == 4:
        U = solve_bottom(Q)
        dinv = [int(t) for t in inv_K(mdet(U))]
        return mmap(lambda e: mul(e, dinv), madj(U))
    t0 = time.time()
    cands, nmin = find_vtau(Q)
    if not cands:
        raise RuntimeError('no parity-test survivor at n=%d' % n)
    U0 = bezout_basis(Q, cands[0])
    # The parity test cannot tell V_tau from -V_tau. For -V_tau the kernel is
    # B^-1 (X O')^2 and (X^-1 x, X y) is a det-1 basis of B^-1 O'^2.
    z, zi = monom(n, 1), monom(n, -1)
    U1 = [[mul(zi, U0[0][0]), mul(z, U0[0][1])], [mul(zi, U0[1][0]), mul(z, U0[1][1])]]
    for sign, U in (('+', U0), ('-', U1)):
        Qp = mmul(mmul(mstar(U), Q), U)
        if any(any(e[1::2]) for r in Qp for e in r):
            continue
        log('    n=%d: %d minimal pair(s), %d parity survivor(s), branch %s (%.1fs)'
            % (n, nmin, len(cands), sign, time.time() - t0))
        try:
            Bh = recover(mmap(even_part, Qp), log)
        except RuntimeError as ex:
            log('    n=%d: branch %s failed below (%s)' % (n, sign, ex))
            continue
        return mmul(mmap(lift, Bh), madj(U))  # det U = 1
    raise RuntimeError('both signs failed at n=%d' % n)


# ---------------------------------------------------------------------------
# Checks.


def check_L12():
    """prop:cocycleclass (3): L12 = P sl2(O8) P^-1 is D6 + D6."""
    P = P_matrix(4)
    dinv = inv_K(mdet(P))
    Pinv = mmap(lambda e: mul(e, dinv), madj(P))
    basis = []
    for j in range(4):
        e = monom(4, j)
        basis += [[[e, zero(4)], [zero(4), neg(e)]], [[zero(4), e], [zero(4), zero(4)]],
                  [[zero(4), zero(4)], [e, zero(4)]]]
    W = [mmul(mmul(P, Z), Pinv) for Z in basis]
    ip = lambda A, B: sum(Fr(x) * Fr(y) for i in range(2) for j in range(2)
                          for x, y in zip(A[i][j], B[i][j]))
    G = [[ip(a, b) for b in W] for a in W]
    assert all(v.denominator == 1 for r in G for v in r), 'L12 not integral'
    G = [[int(v) for v in r] for r in G]
    _, Gr = lll_gram(G)
    sv = short_vectors(Gr, 2)
    roots = [u for nv, u in sv if nv == 2]
    roots += [[-x for x in u] for u in roots]
    comps = root_systems(Gr, roots)
    ok = (det_exact(G) == 16 and not any(nv == 1 for nv, _ in sv)
          and comps == Counter({(60, 6): 2}))
    return ok, {'det': int(det_exact(G)), 'roots': len(roots), 'systems': dict(comps)}


def run_key(n, seed, certificate=True, hawk=False, log=print):
    row = {'n': n, 'seed': seed}
    ok = True
    t0 = time.time()
    B = keygen(n, seed)
    S = S_matrix(n)
    QE = mmul(mmul(mstar(B), S), B)
    Vt = mmul(madj(B), mmap(tau, B))
    Ys, G, den = cocycle_lattice(QE)
    H, Gr = lll_gram(G)
    rank = len(G)
    d = det_exact(G) / Fr(den) ** rank
    row['cocycle_rank'] = rank
    row['cocycle_det'] = str(d)
    ok &= rank == n and d == 2 ** (n // 2 - 1)
    row['lll_first_is_vtau'] = combine(H[0], Ys, n) in (Vt, mmap(neg, Vt))
    if certificate:
        sv = short_vectors(Gr, 2 * den)
        n1 = [u for nv, u in sv if nv == den]
        roots = [u for nv, u in sv if nv == 2 * den]
        row['norm1_pairs'] = len(n1)
        row['norm2_vectors'] = 2 * len(roots)
        mins = [combine([sum(u[i] * H[i][j] for i in range(rank)) for j in range(rank)], Ys, n)
                for u in n1]
        row['norm1_is_vtau'] = all(M in (Vt, mmap(neg, Vt)) for M in mins)
        comps = root_systems(Gr, roots + [[-x for x in u] for u in roots])
        row['root_systems'] = ' '.join('%dx(%d roots, rank %d)' % (c, s, r)
                                       for (s, r), c in sorted(comps.items()))
        expect = Counter({(2, 1): n // 4 - 1, (60, 6): n // 8}) if n >= 8 else None
        sub_det = 1
        for (s, r), c in comps.items():
            sub_det *= ROOT_DET.get((s, r), 0) ** c
        full = rank_exact(n1 + roots) == rank
        ok &= (len(n1) == 1 and row['norm1_is_vtau'] and 2 * len(roots) == 8 * n - 2
               and comps == expect and full and sub_det == d)
    if hawk:
        QH = mmul(mstar(B), B)
        Ys_h, Gh, denh = cocycle_lattice(QH)
        _, Ghr = lll_gram(Gh)
        nh = sum(1 for nv, _ in short_vectors(Ghr, denh) if nv == denh)
        row['hawk_norm1_pairs'] = nh
        ok &= nh == n // 2 + 1
    if n >= 16:
        xs, GK = descent_lattice(QE, Vt)
        HK, GKr = lll_gram(GK)
        row['descent_det'] = str(det_exact(GK))
        ok &= det_exact(GK) == 1
        if certificate and n <= 32:
            sv = short_vectors(GKr, 2)
            r2 = [u for nv, u in sv if nv == 2]
            comps = root_systems(GKr, r2 + [[-x for x in u] for u in r2])
            row['descent_root_systems'] = ' '.join('%dx(%d roots, rank %d)' % (c, s, r)
                                                   for (s, r), c in sorted(comps.items()))
            ok &= comps == Counter({(240, 8): n // 8}) and not any(nv == 1 for nv, _ in sv)
    t1 = time.time()
    Bp = recover(QE, log)
    row['recovered'] = mmul(mmul(mstar(Bp), S), Bp) == QE and is_unit(mdet(Bp))
    row['recovered_equals_B'] = Bp in (B, mmap(neg, B))
    row['recovery_seconds'] = round(time.time() - t1, 1)
    row['total_seconds'] = round(time.time() - t0, 1)
    ok &= row['recovered']
    row['ok'] = bool(ok)
    return row


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--self-test', action='store_true',
                    help='L12 and one key at n = 8 and 16 (seconds)')
    ap.add_argument('--n', default='8,16,32', help='comma-separated degrees (>= 8)')
    ap.add_argument('--keys', type=int, default=5, help='keys per degree')
    ap.add_argument('--first-seed', type=int, default=1)
    ap.add_argument('--no-certificate', action='store_true',
                    help='skip the enumeration-based class certificates (for n >= 64)')
    ap.add_argument('--hawk', action='store_true',
                    help='also count the minimal vectors of the HAWK cocycle lattice (first key per n)')
    ap.add_argument('--out-dir', default=None, help='write galois_attack.csv here')
    ap.add_argument('--quiet', action='store_true')
    a = ap.parse_args()
    log = (lambda *s: None) if a.quiet else (lambda *s: print(*s, flush=True))

    ok, info = check_L12()
    print('L12 = P sl2(O8) P^-1: det %d, %d roots, root systems %s, no norm-1 vector -> D6+D6: %s'
          % (info['det'], info['roots'], info['systems'], ok), flush=True)
    all_ok = ok
    if a.self_test:
        plan = [(8, 1, True, True), (16, 1, True, True)]
    else:
        ns = [int(x) for x in a.n.split(',')]
        plan = [(n, a.first_seed + i, not a.no_certificate, a.hawk and i == 0)
                for n in ns for i in range(a.keys)]
    rows = []
    for n, seed, cert, hawk in plan:
        if n < 8 or n & (n - 1):
            sys.exit('n must be a power of two >= 8')
        row = run_key(n, seed, certificate=cert, hawk=hawk, log=log)
        rows.append(row)
        all_ok &= row['ok']
        print('n=%d seed=%d: %s' % (n, seed, ', '.join('%s=%s' % (k, v) for k, v in row.items()
                                                    if k not in ('n', 'seed'))), flush=True)
    if a.out_dir:
        os.makedirs(a.out_dir, exist_ok=True)
        keys = []
        for r in rows:
            keys += [k for k in r if k not in keys]
        path = os.path.join(a.out_dir, 'galois_attack.csv')
        with open(path, 'w', newline='') as fh:
            w = csv.DictWriter(fh, fieldnames=keys)
            w.writeheader()
            w.writerows(rows)
        print('wrote', path)
    print('galois-attack: all predictions hold' if all_ok else 'galois-attack: PREDICTION FAILED')
    sys.exit(0 if all_ok else 1)


if __name__ == '__main__':
    main()
