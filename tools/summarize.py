#!/usr/bin/env python3
"""Compact table from one or more bench result CSVs (standard library only).

    python3 tools/summarize.py results/diag_ryzen.csv [more.csv ...] [--include-smoke] [--bin 2,3,4]

Rows with warmup=1 are always ignored; rows with smoke=1 (pilot / diagnostic runs) only with --include-smoke.
One line per (algo, list, threads, objective, bin):
  batch_ms   median over repetitions of the summed search time of all queries in the bin
  vs_seq     sequential batch time / this batch time   (> 1 means faster than the sequential baseline)
  vs_T1      same list at 1 thread / this batch time   (self-relative speedup)
  exp/q      mean node expansions per query            (extra work shows up here)
  reopen%    share of expansions that re-expanded a node
  stale/q, idle/q, cas/q   mean stale pops, idle polls of an empty list, lost CAS races per query
  add_ns, pop_ns   mean list-operation latency from the sampled operations (needs --latency when running bench)
  bad        rows whose result differs from the golden cost or whose path check failed
"""
import argparse
import csv
import statistics
import sys
from collections import defaultdict


def load(paths, include_smoke):
    rows = []
    for p in paths:
        with open(p) as f:
            for r in csv.DictReader(f):
                if r["warmup"] == "1" or (r["smoke"] == "1" and not include_smoke):
                    continue
                rows.append(r)
    return rows


def i(r, k):
    return int(r[k]) if r[k] != "" else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--include-smoke", action="store_true")
    ap.add_argument("--bin", default="", help="comma separated bins to show (default all)")
    a = ap.parse_args()
    rows = load(a.csv, a.include_smoke)
    if not rows:
        sys.exit("no rows (smoke rows need --include-smoke)")
    keep_bins = {int(x) for x in a.bin.split(",") if x} or None
    groups = defaultdict(list)
    for r in rows:
        if keep_bins is None or int(r["bin"]) in keep_bins:
            groups[(r["platform"], r["algo"], r["list"], int(r["threads"]), r["obj"], r["rho_permille"], r["w_percent"], int(r["bin"]))].append(r)

    def batch_ms(rs):
        per_rep = defaultdict(int)
        for r in rs:
            per_rep[r["rep"]] += i(r, "search_ns")
        return statistics.median(per_rep.values()) / 1e6

    stats = {}
    for key, rs in groups.items():
        n = len(rs)
        exp = sum(i(r, "expansions") for r in rs)
        add_s, pop_s = sum(i(r, "list_add_samples") for r in rs), sum(i(r, "list_pop_samples") for r in rs)
        stats[key] = {
            "batch": batch_ms(rs), "exp_q": exp / n,
            "reopen": 100.0 * sum(i(r, "reopens") for r in rs) / exp if exp else 0.0,
            "stale_q": sum(i(r, "stale") for r in rs) / n, "idle_q": sum(i(r, "idle_spins") for r in rs) / n,
            "cas_q": sum(i(r, "cas_fail") for r in rs) / n,
            "add_ns": sum(i(r, "list_add_ns") for r in rs) / add_s if add_s else None,
            "pop_ns": sum(i(r, "list_pop_ns") for r in rs) / pop_s if pop_s else None,
            "bad": sum(1 for r in rs if r["path_ok"] != "1" or (r["gap_ppm"] not in ("", "0"))),
            "rss": max(i(r, "peak_rss_kb") for r in rs),
        }
    seq = {(k[0], k[4], k[5], k[6], k[7]): v["batch"] for k, v in stats.items() if k[1] == "seq"}
    t1 = {(k[0], k[2], k[4], k[5], k[6], k[7]): v["batch"] for k, v in stats.items() if k[1] == "par" and k[3] == 1}

    print("%-10s %-4s %-17s %2s %-4s %3s | %9s %7s %7s | %9s %7s %8s %8s %7s | %7s %7s | %3s" % (
        "platform", "algo", "list", "T", "obj", "bin", "batch_ms", "vs_seq", "vs_T1", "exp/q", "reopen%", "stale/q", "idle/q", "cas/q",
        "add_ns", "pop_ns", "bad"))
    for key in sorted(groups, key=lambda k: (k[0], k[4], k[7], k[1] != "seq", k[2], k[3])):
        plat, algo, lst, T, obj, rho, w, b = key
        s = stats[key]
        vs_seq = seq.get((plat, obj, rho, w, b))
        vs_t1 = t1.get((plat, lst, obj, rho, w, b)) if algo == "par" else None
        fmt = lambda x, p=2: "-" if x is None else ("%.*f" % (p, x))
        print("%-10s %-4s %-17s %2d %-4s %3d | %9.2f %7s %7s | %9.0f %7.2f %8.1f %8.1f %7.1f | %7s %7s | %3d" % (
            plat[:10], algo, lst, T, obj, b, s["batch"], fmt(vs_seq / s["batch"] if vs_seq else None),
            fmt(vs_t1 / s["batch"] if vs_t1 else None), s["exp_q"], s["reopen"], s["stale_q"], s["idle_q"], s["cas_q"],
            fmt(s["add_ns"], 0), fmt(s["pop_ns"], 0), s["bad"]))
    total_bad = sum(s["bad"] for s in stats.values())
    print("\nrows differing from golden / failing the path check: %d" % total_bad)


if __name__ == "__main__":
    main()
