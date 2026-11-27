#!/usr/bin/env python3
"""Compact summary of the windowed-list sweeps (tools/run_experiments.py --grid window / pareto-window).

Reads results/<platform>/window/*.csv (scalar) and results/<platform>/pareto_window/*.csv (vector); only configurations marked
"done" in the manifest count; warm-up and smoke rows are ignored. Writes results/<platform>/analysis/window_summary.md:
one table per sweep with, for every configuration, the median batch time per distance bin (sum of the search times of the queries
of one bin in one repetition) and the speedup against the sequential heap of the same sweep (values below 1 = slower).
This is small enough to paste into a message; the raw CSVs of a sweep are large.
Usage: python3 tools/analyze_window.py --platform ryzen-wsl2
"""
import argparse
import csv
import glob
import json
import os
import re
import statistics
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(dir_path):
    """{config: {'rows': n, 'bad': n, 'batch': {bin: [ms per rep]}, 'expansions': per query list}}"""
    done = None
    man = os.path.join(dir_path, "manifest.json")
    if os.path.exists(man):
        done = {n for n, c in json.load(open(man))["configs"].items() if c["status"] == "done"}
    out = {}
    for path in sorted(glob.glob(os.path.join(dir_path, "*.csv"))):
        name = os.path.basename(path)[:-4]
        if done is not None and name not in done:
            continue
        per = defaultdict(lambda: defaultdict(int))
        exp, rows, bad = [], 0, 0
        rss = 0  # peak resident memory of the process, kB (largest over the rows of this configuration)
        worst = 0.0  # slowest single query repetition / median of that query's repetitions (stall detector)
        by_query = defaultdict(list)
        with open(path) as f:
            for r in csv.DictReader(f):
                if r["warmup"] == "1" or r["smoke"] == "1":
                    continue
                rows += 1
                if r["path_ok"] != "1":
                    bad += 1
                per[int(r["bin"])][int(r["rep"])] += int(r["search_ns"])
                per["all"][int(r["rep"])] += int(r["search_ns"])
                exp.append(int(r["expansions"]))
                rss = max(rss, int(r.get("peak_rss_kb") or 0))
                by_query[r["query_id"]].append(int(r["search_ns"]))
        if rows:
            stalls = sum(1 for v in by_query.values() for x in v if len(v) >= 3 and x > 2 * statistics.median(v) and x > 20e6)
            out[name] = {"rows": rows, "bad": bad, "batch": {b: statistics.median(v / 1e6 for v in reps.values()) for b, reps in per.items()},
                         "worst_rep": max(per["all"].values()) / 1e6, "stalls": stalls, "exp": statistics.mean(exp), "rss_mb": rss / 1024.0}
    return out


def describe(name):
    m = re.match(r"(?:pareto_)?(seq|par)(?:_(.+?)_T(\d+)_(pin|free)(?:_wd(\d+)_wb(\d+)(?:_sl(\d+))?(_se)?)?)?_(?:time|dist|vector)_", name)
    if not m:
        return name, 0, "", "", ""
    algo, lst, threads, pin, wd, wb, sl, se = m.groups()
    if algo == "seq":
        return "seq (heap)", 1, "", "", ""
    return lst, int(threads), wd or "", wb or "", ("" if wd is None else (sl if sl is not None else "off") + (" +skip" if se else ""))


def table(title, data, lines):
    if not data:
        lines.append("## %s\n\nno finished configurations found.\n" % title)
        return
    seq = next((v for n, v in data.items() if describe(n)[0] == "seq (heap)"), None)
    bins = sorted(b for b in next(iter(data.values()))["batch"] if b != "all") + ["all"]
    lines.append("## %s\n" % title)
    lines.append("Batch time in ms (median over repetitions); in brackets the speedup vs the sequential heap of this sweep. "
                 "`stalls` = query repetitions that took more than twice that query's median (and over 20 ms); `worst rep` = slowest repetition of the whole batch, ms; `peak MB` = peak resident memory of the whole process (graph included).\n")
    lines.append("| list | threads | window div | batch | slack | " + " | ".join("bin %s" % b for b in bins) + " | worst rep | stalls | exp/query | peak MB | path fails |")
    lines.append("|" + "---|" * (len(bins) + 10))
    def key(item):
        lst, t, wd, wb, sl = describe(item[0])
        return (lst != "seq (heap)", lst, t, int(wd or 0), int(wb or 0), -1 if sl in ("", "off") or sl.startswith("off") else int(sl.split()[0]), "+skip" in sl)
    for name, v in sorted(data.items(), key=key):
        lst, t, wd, wb, sl = describe(name)
        cells = []
        for b in bins:
            ms = v["batch"].get(b)
            ref = seq["batch"].get(b) if seq else None
            cells.append("" if ms is None else ("%.1f (%.2f)" % (ms, ref / ms) if ref else "%.1f" % ms))
        lines.append("| %s | %d | %s | %s | %s | %s | %.0f | %d | %.0f | %.0f | %d |" % (lst, t, wd, wb, sl, " | ".join(cells), v["worst_rep"], v["stalls"], v["exp"], v["rss_mb"], v["bad"]))
    lines.append("")


def unfinished(dir_path):
    """[(config, status, note)] of configurations that did not finish (timeout, failed, incorrect), from the manifest."""
    man = os.path.join(dir_path, "manifest.json")
    if not os.path.exists(man):
        return []
    return sorted((n, c["status"], c.get("note", "")) for n, c in json.load(open(man))["configs"].items() if c["status"] != "done")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", required=True)
    ap.add_argument("--results", default=os.path.join(ROOT, "results"))
    a = ap.parse_args()
    base = os.path.join(a.results, a.platform)
    lines = ["# %s: windowed-list sweeps (generated by tools/analyze_window.py; do not edit)\n" % a.platform]
    found = False
    for sub, title in (("window", "Scalar search (time objective, exact heuristic)"), ("pareto_window", "Vector (Pareto) search")):
        if os.path.isdir(os.path.join(base, sub)):
            found = True
            table(title, load(os.path.join(base, sub)), lines)
            bad = unfinished(os.path.join(base, sub))
            if bad:
                lines.append("Configurations that did NOT finish (they are missing from the table above; a timeout means the configuration was "
                             "slower than the limit, not that it is wrong):\n")
                lines.append("| config | status | note |\n|---|---|---|")
                lines.extend("| %s | %s | %s |" % (n, st, note.replace("|", "/")) for n, st, note in bad)
                lines.append("")
    if not found:
        sys.exit("no window/ or pareto_window/ directory under %s" % base)
    out_dir = os.path.join(base, "analysis")
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "window_summary.md")
    open(path, "w").write("\n".join(lines))
    print("\n".join(lines))
    print("wrote", path)


if __name__ == "__main__":
    main()
