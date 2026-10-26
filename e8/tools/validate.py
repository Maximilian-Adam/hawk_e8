#!/usr/bin/env python3
"""
Step 6 validation driver (e8/docs/validation.md; Python standard library
only): signature correctness of every sampler variant of both schemes,
the E8 debug-build run, the sampler histograms and the distribution tests.

  validate.py [--quick] [--jobs N] [--out-dir DIR] [--bin-dir DIR]
              [--seed X] [--check FILE]

Parts (every process single-threaded unless stated, at most --jobs CPU
slots in use, default os.cpu_count()):

  1. correctness  bin/e8_correctness_nodebug (HAWK_E8_DEBUG_CHECKS=0, the
                  timed build's flags) for all 21 (scheme, variant, logn)
                  configurations, SIGS signatures each, in shards of
                  KEYS keys x SIGS_PER_KEY signatures with disjoint key
                  indices; every tenth shard of a variant with a split
                  sampler (HAWK ref-T, hot64, hot32, every E8 variant) runs
                  with 4 threads.
  2. debug        bin/e8_correctness (default build, E8 debug checks on)
                  for every E8 shard of part 1 with the same seed, shard
                  and threads: the debug checks must pass and the
                  signature digests must equal part 1's.
  3. sampling     bin/e8_distribution_sample: every 1D table (HAWK per
                  parity, E8 per class, every variant), E8 blocks for all
                  256 taus, and signature norms for one fixed target.
  4. tests        tools/distribution_tests.py on the merged histograms.

Outputs in --out-dir, with prefix validation_ (validation_quick_ for
--quick): correctness.csv (one row per shard and build), hist.csv (merged
histograms), tests.csv, controls.csv, ideal.csv, signorm.csv, and
summary.csv (one row per scheme, variant, logn; no timings, so equal
seeds give an identical file). With --check FILE the summary must equal
FILE. Exit status 0 if every signature verified, every debug check and
digest matched, every distribution test passed and every eligible
negative control was rejected.
"""

import argparse
import csv
import io
import os
import subprocess
import sys
import tempfile
import time

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import distribution_tests as dt  # noqa: E402

VARIANTS = [("hawk", v) for v in dt.HAWK_VARIANTS] \
    + [("e8", v) for v in dt.E8_VARIANTS]
LOGNS = (8, 9, 10)
E8_CLASSES = 13

FULL = dict(sigs=1000000, keys=10, sigs_per_key=1000, thread_every=10,
            threads=4, draws_1d=100000000, part_1d=10000000,
            blocks=1000000, taus_per_job=16, signorm=1000000,
            part_signorm=100000)
QUICK = dict(sigs=2000, keys=2, sigs_per_key=100, thread_every=5,
             threads=4, draws_1d=1000000, part_1d=1000000,
             blocks=10000, taus_per_job=64, signorm=10000,
             part_signorm=10000)

# Rough single-process costs on the development machine (Ryzen 9 9950X3D,
# WSL), seconds, for the duration estimate only.
COST_SIG = {("hawk", 8): 6e-5, ("hawk", 9): 1.6e-4, ("hawk", 10): 3.4e-4,
            ("e8", 8): 1.9e-4, ("e8", 9): 4.5e-4, ("e8", 10): 1e-3}
DEBUG_FACTOR = 3.0
COST_1D = 4e-8
COST_BLOCK = {"ref": 7e-7, "hot64": 3.3e-7, "hot32": 2.5e-7}
COST_SIGNORM = {("hawk", 8): 1e-5, ("hawk", 9): 2.2e-5, ("hawk", 10): 4.6e-5,
                ("e8", 8): 4e-5, ("e8", 9): 8e-5, ("e8", 10): 1.6e-4}


def threaded_variant(scheme, variant):
    return not (scheme == "hawk" and variant == "ref")


class Job:
    def __init__(self, part, argv, slots=1, meta=None, cost=0.0):
        self.part = part
        self.argv = argv
        self.slots = slots
        self.meta = meta or {}
        self.cost = cost
        self.out = None
        self.err = None
        self.rc = None


def run_jobs(jobs, slots):
    """
    Run jobs in parallel with at most `slots` CPU slots in use (a job
    larger than slots runs alone); stdout and stderr go to temporary files.
    """
    pending = list(jobs)
    running = []
    used = 0
    while pending or running:
        while pending:
            fit = [j for j in pending if used + j.slots <= slots]
            if not fit and running:
                break
            j = fit[0] if fit else pending[0]
            pending.remove(j)
            j.fout = tempfile.TemporaryFile()
            j.ferr = tempfile.TemporaryFile()
            j.proc = subprocess.Popen(j.argv, stdout=j.fout, stderr=j.ferr)
            running.append(j)
            used += j.slots
        done = None
        while done is None:
            for j in running:
                if j.proc.poll() is not None:
                    done = j
                    break
            else:
                time.sleep(0.02)
        running.remove(done)
        used -= done.slots
        done.rc = done.proc.returncode
        for name in ("fout", "ferr"):
            f = getattr(done, name)
            f.seek(0)
            setattr(done, "out" if name == "fout" else "err",
                    f.read().decode())
            f.close()
        if done.rc != 0:
            sys.stderr.write("validate: job failed (%d): %s\n%s"
                             % (done.rc, " ".join(done.argv), done.err))


def correctness_jobs(cfg, bindir, seed):
    jobs = []
    per_shard = cfg["keys"] * cfg["sigs_per_key"]
    shards = cfg["sigs"] // per_shard
    for scheme, variant in VARIANTS:
        for logn in LOGNS:
            for shard in range(shards):
                threads = cfg["threads"] if (
                    threaded_variant(scheme, variant)
                    and shard % cfg["thread_every"]
                    == cfg["thread_every"] - 1) else 1
                builds = ["nodebug"] + (["debug"] if scheme == "e8" else [])
                for build in builds:
                    exe = "e8_correctness" + (
                        "_nodebug" if build == "nodebug" else "")
                    argv = [os.path.join(bindir, exe), "--scheme", scheme,
                            "--variant", variant, "--logn", str(logn),
                            "--keys", str(cfg["keys"]),
                            "--sigs-per-key", str(cfg["sigs_per_key"]),
                            "--seed", str(seed), "--shard", str(shard),
                            "--threads", str(threads), "--header"]
                    cost = per_shard * COST_SIG[(scheme, logn)] * (
                        DEBUG_FACTOR if build == "debug" else 1.0)
                    jobs.append(Job("debug" if build == "debug"
                                    else "correctness", argv,
                                    slots=threads, cost=cost * threads,
                                    meta=dict(build=build)))
    return jobs


def sampling_jobs(cfg, bindir, seed):
    exe = os.path.join(bindir, "e8_distribution_sample")
    jobs = []

    def job(args, cost):
        jobs.append(Job("sampling", [exe] + args
                        + ["--seed", str(seed), "--header"], cost=cost))

    for scheme, variant in VARIANTS:
        ids = range(2) if scheme == "hawk" else range(E8_CLASSES)
        for logn in LOGNS:
            for ident in ids:
                for part in range(cfg["draws_1d"] // cfg["part_1d"]):
                    job(["--kind", "1d", "--scheme", scheme, "--variant",
                         variant, "--logn", str(logn), "--id", str(ident),
                         "--count", str(cfg["part_1d"]),
                         "--part", str(part)], cfg["part_1d"] * COST_1D)
            for part in range(cfg["signorm"] // cfg["part_signorm"]):
                job(["--kind", "signorm", "--scheme", scheme, "--variant",
                     variant, "--logn", str(logn),
                     "--count", str(cfg["part_signorm"]),
                     "--part", str(part)],
                    cfg["part_signorm"] * COST_SIGNORM[(scheme, logn)])
            if scheme != "e8":
                continue
            step = cfg["taus_per_job"]
            for lo in range(0, 256, step):
                job(["--kind", "block", "--scheme", scheme, "--variant",
                     variant, "--logn", str(logn),
                     "--ids", "%d-%d" % (lo, lo + step - 1),
                     "--count", str(cfg["blocks"])],
                    step * cfg["blocks"] * COST_BLOCK[variant])
    return jobs


def parse_rows(text):
    return list(csv.DictReader(io.StringIO(text)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    ap.add_argument("--out-dir", default=os.path.join(HERE, ".."))
    ap.add_argument("--bin-dir", default=os.path.join(HERE, "..", "bin"))
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--alpha", type=float, default=dt.ALPHA_DEFAULT)
    ap.add_argument("--check")
    args = ap.parse_args()
    cfg = QUICK if args.quick else FULL
    prefix = os.path.join(args.out_dir, "validation_quick_" if args.quick
                          else "validation_")

    cjobs = correctness_jobs(cfg, args.bin_dir, args.seed)
    sjobs = sampling_jobs(cfg, args.bin_dir, args.seed)
    est = sum(j.cost for j in cjobs + sjobs) / max(1, args.jobs) * 1.6
    print("validate: %s run, %d correctness/debug shards and %d sampling"
          " jobs on %d CPU slots; expected duration about %.0f minutes"
          " (plus about 1 minute of analysis)"
          % ("quick" if args.quick else "full", len(cjobs), len(sjobs),
             args.jobs, est / 60 + 0.5), flush=True)

    ok = True
    t0 = time.time()
    run_jobs(cjobs, args.jobs)
    t1 = time.time()
    print("validate: correctness and debug runs done in %.0f s" % (t1 - t0),
          flush=True)
    run_jobs(sjobs, args.jobs)
    t2 = time.time()
    print("validate: sampling done in %.0f s" % (t2 - t1), flush=True)
    if any(j.rc != 0 for j in cjobs + sjobs):
        ok = False

    # Correctness rows; debug digests against the no-debug run.
    crow = []
    digests = {}
    for j in cjobs:
        for r in parse_rows(j.out or ""):
            r["build"] = j.meta["build"]
            crow.append(r)
    for r in crow:
        key = (r["scheme"], r["variant"], r["logn"], r["shard"],
               r["threads"])
        digests.setdefault(key, {})[r["build"]] = r["sig_digest"]
    digest_mismatch = sum(1 for d in digests.values()
                          if "debug" in d and d["debug"] != d.get("nodebug"))
    cfields = ["build"] + [f for f in crow[0].keys() if f != "build"] \
        if crow else ["build"]
    with open(prefix + "correctness.csv", "w", newline="") as f:
        w = csv.DictWriter(f, cfields, lineterminator="\n")
        w.writeheader()
        for r in crow:
            w.writerow(r)

    # Merged histograms.
    hist = {}
    for j in sjobs:
        for r in parse_rows(j.out or ""):
            key = (r["kind"], r["scheme"], r["variant"], int(r["logn"]),
                   int(r["id"]), int(r["value"]))
            hist[key] = hist.get(key, 0) + int(r["count"])
    with open(prefix + "hist.csv", "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(["kind", "scheme", "variant", "logn", "id", "value",
                    "count"])
        for key in sorted(hist):
            w.writerow(list(key) + [hist[key]])

    refs = dt.References(os.path.join(HERE, "..", "..", "src",
                                      "hawk_sign.c"))
    tests, controls, ideal, signorm = dt.run_tests(
        refs, dt.read_hist([prefix + "hist.csv"]), args.alpha)
    dt.write_rows(prefix + "tests.csv", tests, dt.TEST_FIELDS)
    dt.write_rows(prefix + "controls.csv", controls, dt.CONTROL_FIELDS)
    dt.write_rows(prefix + "ideal.csv", ideal, dt.TEST_FIELDS)
    dt.write_rows(prefix + "signorm.csv", signorm, dt.SIGNORM_FIELDS)
    dsum = dt.summarise(tests, controls, ideal)
    t3 = time.time()
    print("validate: distribution tests done in %.0f s" % (t3 - t2),
          flush=True)

    # Summary, one row per (scheme, variant, logn).
    fields = ["scheme", "variant", "logn", "signatures",
              "threaded_signatures", "sign_failures", "verify_failures",
              "control_accepts", "attempt_mismatches", "attempts_per_sig",
              "attempts_max", "debug_signatures", "debug_failures",
              "debug_digest_mismatches", "dist_tests", "dist_rejected",
              "impossible_outcomes", "min_p", "min_p_holm",
              "controls", "controls_eligible", "controls_rejected",
              "eligible_not_rejected", "ideal_tests", "ideal_rejected",
              "signorm_z_mean", "signorm_z_var",
              "signorm_mean_rel_continuous"]
    rows = []
    for scheme, variant in VARIANTS:
        for logn in LOGNS:
            mine = [r for r in crow if (r["scheme"], r["variant"],
                                        int(r["logn"])) ==
                    (scheme, variant, logn)]
            nd = [r for r in mine if r["build"] == "nodebug"]
            db = [r for r in mine if r["build"] == "debug"]

            def tot(rs, f):
                return sum(int(r[f]) for r in rs)
            sigs = tot(nd, "signatures")
            good = sigs - tot(nd, "sign_failures")
            s = dsum.get((scheme, variant, logn), {})
            sn = [r for r in signorm if (r["scheme"], r["variant"],
                                         r["logn"]) == (scheme, variant,
                                                        logn)]
            mism = sum(1 for k, d in digests.items()
                       if k[:3] == (scheme, variant, str(logn))
                       and "debug" in d and d["debug"] != d.get("nodebug"))
            rows.append(dict(
                scheme=scheme, variant=variant, logn=logn, signatures=sigs,
                threaded_signatures=sum(int(r["signatures"]) for r in nd
                                        if int(r["threads"]) > 1),
                sign_failures=tot(nd, "sign_failures"),
                verify_failures=tot(nd, "verify_failures"),
                control_accepts=tot(nd, "control_accepts"),
                attempt_mismatches=tot(nd, "attempt_mismatches"),
                attempts_per_sig="%.6f" % (tot(nd, "attempts") / good
                                           if good else 0),
                attempts_max=max((int(r["attempts_max"]) for r in nd),
                                 default=0),
                debug_signatures=tot(db, "signatures"),
                debug_failures=(tot(db, "sign_failures")
                                + tot(db, "verify_failures")
                                + tot(db, "control_accepts")
                                + tot(db, "attempt_mismatches")),
                debug_digest_mismatches=mism,
                dist_tests=s.get("tests", 0),
                dist_rejected=s.get("tests_rejected", 0),
                impossible_outcomes=s.get("impossible", 0),
                min_p="%.4g" % s.get("min_p", 1.0),
                min_p_holm="%.4g" % s.get("min_p_holm", 1.0),
                controls=s.get("controls", 0),
                controls_eligible=s.get("controls_eligible", 0),
                controls_rejected=s.get("controls_rejected", 0),
                eligible_not_rejected=s.get("eligible_not_rejected", 0),
                ideal_tests=s.get("ideal_tests", 0),
                ideal_rejected=s.get("ideal_rejected", 0),
                signorm_z_mean="%.3f" % sn[0]["z_mean"] if sn else "",
                signorm_z_var="%.3f" % sn[0]["z_var"] if sn else "",
                signorm_mean_rel_continuous=("%.3e"
                                             % sn[0]["mean_rel_continuous"]
                                             if sn else "")))
    with open(prefix + "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fields, lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow(r)

    sigs = sum(r["signatures"] for r in rows)
    fails = sum(r["sign_failures"] + r["verify_failures"]
                + r["control_accepts"] + r["attempt_mismatches"]
                for r in rows)
    dbg = sum(r["debug_signatures"] for r in rows)
    dfail = sum(r["debug_failures"] for r in rows)
    rej = sum(r["dist_rejected"] for r in rows)
    missed = sum(r["eligible_not_rejected"] for r in rows)
    print("validate: %d signatures (%d threaded), %d failures; debug build"
          " %d signatures, %d failures, %d digest mismatches"
          % (sigs, sum(r["threaded_signatures"] for r in rows), fails, dbg,
             dfail, digest_mismatch))
    print("validate: %d distribution tests, %d rejected after Holm (alpha"
          " %g), smallest p %.3g, smallest Holm-adjusted p %.3g"
          % (len(tests), rej, args.alpha, min(r["p"] for r in tests),
             min(r["p_holm"] for r in tests)))
    print("validate: %d negative controls, %d eligible, %d rejected, %d"
          " eligible not rejected; ideal (information): %d of %d rejected"
          % (len(controls), sum(r["eligible"] for r in controls),
             sum(r["rejected"] for r in controls), missed,
             sum(r["rejected"] for r in ideal), len(ideal)))
    print("validate: wrote %s{correctness,hist,tests,controls,ideal,"
          "signorm,summary}.csv; total %.0f s"
          % (os.path.relpath(prefix), time.time() - t0))
    if fails or dfail or digest_mismatch or rej or missed:
        ok = False
    if args.check is not None and not os.path.exists(args.check):
        print("validate: %s does not exist (copy the fresh summary there)"
              % args.check)
        ok = False
    elif args.check is not None:
        with open(args.check) as a, open(prefix + "summary.csv") as b:
            if a.read() != b.read():
                print("validate: %s differs from the fresh summary"
                      % args.check)
                ok = False
            else:
                print("validate: committed %s matches" % args.check)
    print("validate: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
