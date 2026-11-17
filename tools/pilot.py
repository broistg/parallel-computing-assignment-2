#!/usr/bin/env python3
"""Pilot: measure how long queries actually take on THIS machine and derive the workload sizes
(pairs per bin, repetitions, projected wall-clock budget).  Pilot output is for planning only:
tagged smoke=1 and never used in the report.

    python3 tools/pilot.py --snapshot data/snapshot_hcmc --platform ryzen-wsl2 [--threads 1,2,4,8]

Rules it applies (docs/PARAMETERS.md):
  * every measured (configuration, bin) must accumulate >= MIN_MS of search time, otherwise the
    timer noise dominates -> pairs_per_bin is raised until the shortest bin reaches it (cap 50);
  * projected budget = sum over configs of (reps+1) * sum of query times, checked against a
    per-platform budget (default 5 h).
"""
import argparse
import csv
import json
import os
import statistics
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MIN_MS = 20.0


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit("command failed: %s\n%s" % (" ".join(cmd), r.stderr))
    return r.stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--snapshot", required=True)
    ap.add_argument("--platform", required=True)
    ap.add_argument("--profile", default=os.path.join(ROOT, "config", "profile.csv"))
    ap.add_argument("--bench", default=os.path.join(ROOT, "build", "bench"))
    ap.add_argument("--bins-km", default="1,3,8,15,25,1000")
    ap.add_argument("--sample-per-bin", type=int, default=6)
    ap.add_argument("--threads", default="1,2,4,8")
    ap.add_argument("--lists", default="coarse_ttas,coarse_ticket,coarse_std_mutex")
    ap.add_argument("--reps", type=int, default=10)
    ap.add_argument("--budget-hours", type=float, default=5.0)
    ap.add_argument("--out", default=os.path.join(ROOT, "results"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp()
    q = os.path.join(tmp, "q.csv")
    run([sys.executable, os.path.join(HERE, "make_queries.py"), "--snapshot", a.snapshot, "--per-bin",
         str(a.sample_per_bin), "--bins-km", a.bins_km, "--seed", "99", "--out", q])
    configs = [("seq", 1, "pq")] + [("par", int(t), l) for l in a.lists.split(",") for t in a.threads.split(",")]
    per_cfg = {}
    for algo, t, lst in configs:
        res = os.path.join(tmp, "r_%s_%d_%s.csv" % (algo, t, lst))
        run([a.bench, "--snapshot", a.snapshot, "--profile", a.profile, "--queries", q, "--algo", algo,
             "--threads", str(t), "--list", lst if algo == "par" else "coarse_ttas", "--reps", "2",
             "--platform", a.platform, "--smoke", "--out", res])
        by_bin = {}
        for r in csv.DictReader(open(res)):
            if r["warmup"] == "0":
                by_bin.setdefault(int(r["bin"]), []).append(int(r["search_ns"]) / 1e6)
        per_cfg[(algo, t, lst)] = {b: statistics.median(v) for b, v in by_bin.items()}
        print("%-4s T=%-2d %-14s median ms per bin: %s" % (algo, t, lst, {b: round(v, 3) for b, v in sorted(per_cfg[(algo, t, lst)].items())}))

    seq = per_cfg[("seq", 1, "pq")]
    PAIRS_CAP = 50
    # A (config, bin) batch = all pairs of the bin per repetition.  It must reach MIN_MS.
    floor_limited = {b: round(PAIRS_CAP * v, 3) for b, v in sorted(seq.items()) if PAIRS_CAP * v < MIN_MS}
    pairs = PAIRS_CAP
    total_s = sum((a.reps + 1) * pairs * sum(d.values()) / 1000.0 for d in per_cfg.values())  # ms -> s
    slowdown = {}
    for (algo, t, lst), d in per_cfg.items():
        if algo == "par":
            slowdown["%s_T%d" % (lst, t)] = round(statistics.median(d[b] / seq[b] for b in seq if b in d), 2)
    rec = {
        "platform": a.platform,
        "median_seq_ms_per_bin": {str(b): round(v, 4) for b, v in sorted(seq.items())},
        "recommended_pairs_per_bin": pairs,
        "bins_below_%dms_batch_floor_with_%d_pairs" % (MIN_MS, pairs): floor_limited,
        "floor_advice": "drop or merge these bins (short queries measure the timer, not the algorithm), or use a larger graph",
        "par_time_over_seq_time_median_over_bins": slowdown,
        "reps": a.reps,
        "projected_hours_for_this_grid": round(total_s / 3600, 3),
        "budget_hours": a.budget_hours,
        "fits_budget": total_s / 3600 <= a.budget_hours,
        "note": "pilot numbers are for planning only; smoke runs are excluded from the report",
    }
    path = os.path.join(a.out, "pilot_%s.json" % a.platform)
    json.dump(rec, open(path, "w"), indent=2)
    print(json.dumps(rec, indent=2))


if __name__ == "__main__":
    main()
