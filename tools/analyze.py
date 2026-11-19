#!/usr/bin/env python3
"""Tables and figures from the experiment CSVs (standard library; matplotlib only for the figures).

    python3 tools/analyze.py --platform ryzen-wsl2 [--platform other ...] [--results results] [--no-figures]

Reads results/<platform>/run*/<config>.csv written by tools/run_experiments.py (only configurations marked "done" in
the run manifest; .partial files, warm-up rows and smoke rows are ignored) and writes results/<platform>/analysis/:

  time_table.csv   every configuration x bin: median, q1, q3 of the batch time (sum of search_ns over the queries of one bin
                   in one repetition) in ms, number of samples. bin "all" = every query.
  speedup.csv      main study only (objective time, exact heuristic): speedup of each parallel configuration against
                   the sequential heap, the sequential sorted list, and the same list at 1 thread (same pinning);
                   efficiency = speedup vs T1 / threads. The band columns use the parallel configuration's IQR only.
  quality.csv      per configuration: mean and max gap to the exact (golden) cost in ppm, share of rows that are not
                   optimal, mean expansions / stale pops per query. Needs the golden files at run time.
  memory.csv       peak resident memory (KB) per configuration (one process per configuration).
  listops.csv      list operation mix per query (adds, duplicate adds, pops, empty pops), idle polls, lost CAS races.
  summary.md       the main numbers of the above as readable tables.
  pareto_*.csv, pareto_summary.md   same for the vector (Pareto) runs in results/<platform>/pareto_run*/ (hypervolume ratio,
                   exact points found, front size, speedups); produced when such runs exist.
  fig_*.png        speedup vs threads, time breakdown, quality vs noise (needs matplotlib).

Every number comes from the CSVs; nothing is typed in by hand. Medians are over repetitions (and passes); the IQR is the
spread of the same samples. Ratios are ratios of medians.
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


def load_platform(results, platform):
    """Rows of all finished configurations of one platform. Returns list of dict (parsed numeric fields)."""
    rows = []
    run_dirs = sorted(glob.glob(os.path.join(results, platform, "run*")))
    if not run_dirs:
        sys.exit("no run* directory under %s" % os.path.join(results, platform))
    for run_dir in run_dirs:
        man_path = os.path.join(run_dir, "manifest.json")
        done = None
        if os.path.exists(man_path):
            manifest = json.load(open(man_path))
            done = {n for n, c in manifest["configs"].items() if c["status"] == "done"}
        for path in sorted(glob.glob(os.path.join(run_dir, "*.csv"))):
            name = os.path.basename(path)[:-4]
            if done is not None and name not in done:
                continue
            m = re.search(r"_s(\d+)$", name)
            with open(path) as f:
                for r in csv.DictReader(f):
                    if r["warmup"] == "1" or r["smoke"] == "1":
                        continue
                    rows.append({
                        "run": os.path.basename(run_dir), "config": name, "seed": int(m.group(1)) if m else 0,
                        "algo": r["algo"], "list": r["list"], "threads": int(r["threads"]), "pinned": int(r["pinned"]),
                        "obj": r["obj"], "rho": int(r["rho_permille"]), "w": int(r["w_percent"]),
                        "query": int(r["query_id"]), "bin": int(r["bin"]), "rep": int(r["rep"]),
                        "search_ns": int(r["search_ns"]), "gap_ppm": int(r["gap_ppm"]) if r["gap_ppm"] != "" else None,
                        "golden": int(r["golden_cost"]), "path_ok": int(r["path_ok"]),
                        "expansions": int(r["expansions"]), "stale": int(r["stale"]), "cas_fail": int(r["cas_fail"]),
                        "idle": int(r["idle_spins"]), "adds": int(r["list_adds"]), "dups": int(r["list_add_dups"]),
                        "pops": int(r["list_pops"]), "pop_empty": int(r["list_pop_empty"]), "rss": int(r["peak_rss_kb"]),
                    })
    return rows


def quartiles(values):
    if len(values) == 1:
        return values[0], values[0], values[0]
    q = statistics.quantiles(values, n=4, method="inclusive")
    return q[1], q[0], q[2]  # median, q1, q3


def meta_of(rs):
    r = rs[0]
    return {k: r[k] for k in ("algo", "list", "threads", "pinned", "obj", "rho", "w", "seed")}


def batch_stats(rows):
    """{config: {bin or 'all': (median_ms, q1_ms, q3_ms, n)}}"""
    sums = defaultdict(lambda: defaultdict(int))  # (config, bin) -> (run, rep) -> ns
    for r in rows:
        key = (r["run"], r["rep"])
        sums[(r["config"], r["bin"])][key] += r["search_ns"]
        sums[(r["config"], "all")][key] += r["search_ns"]
    out = defaultdict(dict)
    for (config, b), per_rep in sums.items():
        med, q1, q3 = quartiles([v / 1e6 for v in per_rep.values()])
        out[config][b] = (med, q1, q3, len(per_rep))
    return out


def fmt(x, p=3):
    return "" if x is None else ("%.*f" % (p, x))


def write_csv(path, header, data):
    with open(path, "w", newline="") as f:
        w = csv.writer(f, lineterminator="\n")
        w.writerow(header)
        w.writerows(data)


def md_table(header, data):
    lines = ["| " + " | ".join(header) + " |", "|" + "|".join("---" for _ in header) + "|"]
    lines += ["| " + " | ".join(str(c) for c in row) + " |" for row in data]
    return "\n".join(lines)


def analyze(platform, rows, out_dir, make_figures):
    by_config = defaultdict(list)
    for r in rows:
        by_config[r["config"]].append(r)
    meta = {c: meta_of(rs) for c, rs in by_config.items()}
    stats = batch_stats(rows)
    bins = sorted({r["bin"] for r in rows}) + ["all"]
    os.makedirs(out_dir, exist_ok=True)

    # ---- time_table
    data = []
    for c in sorted(stats):
        m = meta[c]
        for b in bins:
            if b in stats[c]:
                med, q1, q3, n = stats[c][b]
                data.append([platform, c, m["algo"], m["list"], m["threads"], m["pinned"], m["obj"], m["rho"], m["w"], m["seed"], b,
                             fmt(med), fmt(q1), fmt(q3), n])
    write_csv(os.path.join(out_dir, "time_table.csv"), ["platform", "config", "algo", "list", "threads", "pinned", "obj", "rho", "w", "seed",
                                                         "bin", "median_ms", "q1_ms", "q3_ms", "n"], data)

    # ---- speedup (main study: time objective, exact heuristic)
    main = {c for c, m in meta.items() if m["obj"] == "time" and m["rho"] == 0 and m["w"] == 100}
    heap = next((c for c in main if meta[c]["algo"] == "seq"), None)
    slist = next((c for c in main if meta[c]["algo"] == "seqlist"), None)
    t1 = {}
    for c in main:
        m = meta[c]
        if m["algo"] == "par" and m["threads"] == 1:
            t1[(m["list"], m["pinned"])] = c
    sp_rows = []
    for c in sorted(main, key=lambda c: (meta[c]["list"], meta[c]["pinned"], meta[c]["threads"])):
        m = meta[c]
        if m["algo"] != "par":
            continue
        for b in bins:
            if b not in stats[c]:
                continue
            med, q1, q3, _ = stats[c][b]

            def ratio(ref):
                return stats[ref][b][0] / med if ref and b in stats[ref] else None
            ref1 = t1.get((m["list"], m["pinned"]))
            v_heap, v_list, v_t1 = ratio(heap), ratio(slist), ratio(ref1)
            lo = stats[heap][b][0] / q3 if heap and b in stats[heap] and q3 else None
            hi = stats[heap][b][0] / q1 if heap and b in stats[heap] and q1 else None
            sp_rows.append([platform, m["list"], m["threads"], m["pinned"], b, fmt(med), fmt(v_heap), fmt(lo), fmt(hi), fmt(v_list), fmt(v_t1),
                            fmt(v_t1 / m["threads"] if v_t1 is not None else None)])
    write_csv(os.path.join(out_dir, "speedup.csv"), ["platform", "list", "threads", "pinned", "bin", "median_ms", "speedup_vs_heap",
                                                      "vs_heap_low(q3)", "vs_heap_high(q1)", "speedup_vs_sortedlist", "speedup_vs_T1", "efficiency"], sp_rows)

    # ---- quality + expansions
    q_rows = []
    for c in sorted(by_config):
        rs = by_config[c]
        gaps = [r["gap_ppm"] for r in rs if r["gap_ppm"] is not None]
        m = meta[c]
        q_rows.append([platform, c, m["algo"], m["list"], m["threads"], m["obj"], m["rho"], m["w"], m["seed"], len(gaps),
                       fmt(statistics.mean(gaps) if gaps else None, 1), max(gaps) if gaps else "",
                       fmt(100.0 * sum(1 for g in gaps if g > 0) / len(gaps) if gaps else None, 2),
                       fmt(sum(r["expansions"] for r in rs) / len(rs), 1), fmt(sum(r["stale"] for r in rs) / len(rs), 1),
                       sum(1 for r in rs if r["path_ok"] != 1)])
    write_csv(os.path.join(out_dir, "quality.csv"), ["platform", "config", "algo", "list", "threads", "obj", "rho", "w", "seed", "rows_with_golden",
                                                      "gap_mean_ppm", "gap_max_ppm", "non_optimal_pct", "expansions_per_query", "stale_per_query",
                                                      "path_failures"], q_rows)

    # ---- memory + list ops
    write_csv(os.path.join(out_dir, "memory.csv"), ["platform", "config", "peak_rss_kb"],
              [[platform, c, max(r["rss"] for r in by_config[c])] for c in sorted(by_config)])
    lo_rows = []
    for c in sorted(by_config):
        rs, n = by_config[c], len(by_config[c])
        if meta[c]["algo"] == "seq":
            continue
        lo_rows.append([platform, c, fmt(sum(r["adds"] for r in rs) / n, 1), fmt(sum(r["dups"] for r in rs) / n, 2),
                        fmt(sum(r["pops"] for r in rs) / n, 1), fmt(sum(r["pop_empty"] for r in rs) / n, 1),
                        fmt(sum(r["idle"] for r in rs) / n, 1), fmt(sum(r["cas_fail"] for r in rs) / n, 2)])
    write_csv(os.path.join(out_dir, "listops.csv"), ["platform", "config", "adds_per_query", "dup_adds_per_query", "pops_per_query",
                                                      "empty_pops_per_query", "idle_polls_per_query", "lost_cas_per_query"], lo_rows)

    # ---- summary.md
    md = ["# %s: summary (generated by tools/analyze.py; do not edit)\n" % platform]
    md.append("Batch time = sum of search times of the queries of one bin in one repetition; medians over repetitions. "
              "Speedup = ratio of medians. Values below 1 mean slower than the reference.\n")
    if heap:
        md.append("## Sequential baselines (time objective, exact heuristic), batch ms\n")
        base = []
        for c in (heap, slist):
            if c:
                base.append([meta[c]["algo"]] + [fmt(stats[c][b][0], 2) if b in stats[c] else "" for b in bins])
        md.append(md_table(["version"] + ["bin %s" % b for b in bins], base) + "\n")
        pars = sorted({(meta[c]["list"], meta[c]["pinned"]) for c in main if meta[c]["algo"] == "par"})
        thr = sorted({meta[c]["threads"] for c in main if meta[c]["algo"] == "par"})
        for lst, pinned in pars:
            md.append("## Parallel %s, %s: speedup vs sequential heap, bin = all (efficiency vs T1 in brackets)\n" % (lst, "pinned" if pinned else "unpinned"))
            line = []
            for t in thr:
                cell = next((r for r in sp_rows if r[1] == lst and r[2] == t and r[3] == pinned and r[4] == "all"), None)
                line.append("%s (%s)" % (cell[6], cell[11]) if cell else "")
            md.append(md_table(["threads"] + [str(t) for t in thr], [["speedup (eff.)"] + line]) + "\n")
    sens = [r for r in q_rows if r[7] != 100 or r[6] != 0]
    if sens:
        # parallel rows: only the one (list, threads, pinning) that the grid varies over noise and inflation
        variants = defaultdict(set)
        for r in q_rows:
            m = meta[r[1]]
            if r[5] == "time" and m["algo"] == "par":
                variants[(m["list"], m["threads"], m["pinned"])].add((r[6], r[7]))
        sens_variant = max(variants, key=lambda k: len(variants[k])) if variants else None
        grouped = defaultdict(list)
        for r in q_rows:
            m = meta[r[1]]
            if r[5] == "time" and (m["algo"] == "seq" or (m["algo"] == "par" and (m["list"], m["threads"], m["pinned"]) == sens_variant)):
                grouped[(r[2], r[6], r[7])].append(r)
        md.append("## Heuristic sensitivity (time objective; mean over noise seeds; parallel = %s)\n" % (sens_variant,))
        tab = []
        for (algo, rho, w), rs in sorted(grouped.items()):
            gaps = [float(r[10]) for r in rs if r[10] != ""]
            nonopt = [float(r[12]) for r in rs if r[12] != ""]
            exp = [float(r[13]) for r in rs]
            tab.append([algo, rho, w, len(rs), fmt(statistics.mean(gaps) if gaps else None, 1), fmt(statistics.mean(nonopt) if nonopt else None, 2),
                        fmt(statistics.mean(exp), 0)])
        md.append(md_table(["algo", "noise permille", "inflation %", "configs", "mean gap ppm", "non-optimal %", "expansions/query"], tab) + "\n")
    bad = [r for r in q_rows if r[15] != 0]
    md.append("Path-check failures: %d configuration(s).\n" % len(bad))
    open(os.path.join(out_dir, "summary.md"), "w").write("\n".join(md))

    if make_figures:
        figures(platform, out_dir, stats, meta, main, heap, slist, bins, sp_rows, q_rows)
    return len(by_config)


def load_pareto(results, platform):
    """Rows of the finished vector (Pareto) configurations; same rules as load_platform."""
    rows = []
    for run_dir in sorted(glob.glob(os.path.join(results, platform, "pareto_run*"))):
        man_path = os.path.join(run_dir, "manifest.json")
        done = None
        if os.path.exists(man_path):
            done = {n for n, c in json.load(open(man_path))["configs"].items() if c["status"] == "done"}
        for path in sorted(glob.glob(os.path.join(run_dir, "*.csv"))):
            name = os.path.basename(path)[:-4]
            if done is not None and name not in done:
                continue
            m = re.search(r"_s(\d+)$", name)
            with open(path) as f:
                for r in csv.DictReader(f):
                    if r["warmup"] == "1" or r["smoke"] == "1":
                        continue
                    rows.append({
                        "run": os.path.basename(run_dir), "config": name, "seed": int(m.group(1)) if m else 0, "algo": r["algo"], "list": r["list"],
                        "threads": int(r["threads"]), "pinned": int(r["pinned"]), "obj": "vector", "rho": int(r["rho_permille"]), "w": int(r["w_percent"]),
                        "query": int(r["query_id"]), "bin": int(r["bin"]), "rep": int(r["rep"]), "search_ns": int(r["search_ns"]),
                        "front": int(r["front_size"]), "golden_front": int(r["golden_front_size"]), "found": int(r["exact_points_found"]),
                        "hv": int(r["hv_ratio_ppm"]), "path_ok": int(r["path_ok"]), "overflow": int(r["overflow"]),
                        "expansions": int(r["expansions"]), "labels": int(r["labels_created"]), "evicted": int(r["labels_evicted"]),
                        "rss": int(r["peak_rss_kb"]),
                    })
    return rows


def analyze_pareto(platform, rows, out_dir):
    by_config = defaultdict(list)
    for r in rows:
        by_config[r["config"]].append(r)
    meta = {c: meta_of(rs) for c, rs in by_config.items()}
    stats = batch_stats(rows)
    bins = sorted({r["bin"] for r in rows}) + ["all"]
    os.makedirs(out_dir, exist_ok=True)
    seq = next((c for c in by_config if meta[c]["algo"] == "seq" and meta[c]["rho"] == 0 and meta[c]["w"] == 100), None)
    t1 = {(meta[c]["list"], meta[c]["pinned"]): c for c in by_config if meta[c]["algo"] == "par" and meta[c]["threads"] == 1 and meta[c]["rho"] == 0 and meta[c]["w"] == 100}
    time_rows, speed_rows = [], []
    for c in sorted(stats):
        m = meta[c]
        for b in bins:
            if b not in stats[c]:
                continue
            med, q1, q3, n = stats[c][b]
            time_rows.append([platform, c, m["algo"], m["list"], m["threads"], m["pinned"], m["rho"], m["w"], m["seed"], b, fmt(med), fmt(q1), fmt(q3), n])
            if m["algo"] == "par" and m["rho"] == 0 and m["w"] == 100:
                ref1 = t1.get((m["list"], m["pinned"]))
                v_seq = stats[seq][b][0] / med if seq and b in stats[seq] else None
                v_t1 = stats[ref1][b][0] / med if ref1 and b in stats[ref1] else None
                speed_rows.append([platform, m["list"], m["threads"], m["pinned"], b, fmt(med), fmt(v_seq), fmt(v_t1), fmt(v_t1 / m["threads"] if v_t1 is not None else None)])
    write_csv(os.path.join(out_dir, "pareto_time.csv"), ["platform", "config", "algo", "list", "threads", "pinned", "rho", "w", "seed", "bin", "median_ms", "q1_ms", "q3_ms", "n"], time_rows)
    write_csv(os.path.join(out_dir, "pareto_speedup.csv"), ["platform", "list", "threads", "pinned", "bin", "median_ms", "speedup_vs_heap", "speedup_vs_T1", "efficiency"], speed_rows)
    qual = []
    for c in sorted(by_config):
        rs = [r for r in by_config[c] if r["hv"] >= 0]
        m = meta[c]
        if not rs:
            continue
        qual.append([platform, c, m["algo"], m["list"], m["threads"], m["rho"], m["w"], m["seed"], len(rs),
                     fmt(statistics.mean(r["hv"] for r in rs) / 1e6, 4), fmt(min(r["hv"] for r in rs) / 1e6, 4),
                     fmt(100.0 * sum(r["found"] for r in rs) / max(1, sum(r["golden_front"] for r in rs)), 2),
                     fmt(statistics.mean(r["front"] for r in rs), 1), fmt(statistics.mean(r["golden_front"] for r in rs), 1),
                     fmt(statistics.mean(r["expansions"] for r in by_config[c]), 0), fmt(statistics.mean(r["labels"] for r in by_config[c]), 0),
                     max(r["rss"] for r in by_config[c]), sum(1 for r in by_config[c] if r["path_ok"] != 1 or r["overflow"] != 0)])
    write_csv(os.path.join(out_dir, "pareto_quality.csv"), ["platform", "config", "algo", "list", "threads", "rho", "w", "seed", "rows",
                                                             "hv_ratio_mean", "hv_ratio_min", "exact_points_found_pct", "front_size_mean", "golden_front_size_mean",
                                                             "expansions_per_query", "labels_per_query", "peak_rss_kb", "path_failures_or_overflow"], qual)
    md = ["# %s: vector (Pareto) A*, summary (generated by tools/analyze.py; do not edit)\n" % platform,
          "Batch time = sum of search times of the queries of one bin in one repetition; medians over repetitions. Hypervolume ratio = HV(found)/HV(exact), reference point 1.1 x the largest exact component.\n"]
    if seq:
        md.append("## Sequential heap baseline, batch ms\n")
        md.append(md_table(["version"] + ["bin %s" % b for b in bins], [["seq"] + [fmt(stats[seq][b][0], 2) if b in stats[seq] else "" for b in bins]]) + "\n")
    thr = sorted({m["threads"] for c, m in meta.items() if m["algo"] == "par" and m["rho"] == 0 and m["w"] == 100})
    for lst in sorted({m["list"] for c, m in meta.items() if m["algo"] == "par"}):
        line = []
        for t in thr:
            cell = next((r for r in speed_rows if r[1] == lst and r[2] == t and r[4] == "all"), None)
            line.append("%s (%s)" % (cell[6], cell[8]) if cell else "")
        md.append("## Parallel %s: speedup vs sequential heap, bin = all (efficiency vs T1)\n" % lst)
        md.append(md_table(["threads"] + [str(t) for t in thr], [["speedup (eff.)"] + line]) + "\n")
    grouped = defaultdict(list)
    for r in qual:
        grouped[(r[2], r[5], r[6])].append(r)
    if len(grouped) > 1:
        md.append("## Heuristic sensitivity (mean over noise seeds)\n")
        tab = [[algo, rho, w, len(rs), fmt(statistics.mean(float(r[9]) for r in rs), 4), fmt(min(float(r[10]) for r in rs), 4),
                fmt(statistics.mean(float(r[11]) for r in rs), 1), fmt(statistics.mean(float(r[14]) for r in rs), 0)] for (algo, rho, w), rs in sorted(grouped.items())]
        md.append(md_table(["algo", "noise permille", "inflation %", "configs", "mean HV ratio", "min HV ratio", "exact points found %", "expansions/query"], tab) + "\n")
    open(os.path.join(out_dir, "pareto_summary.md"), "w").write("\n".join(md))
    return len(by_config)


def figures(platform, out_dir, stats, meta, main, heap, slist, bins, sp_rows, q_rows):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib not installed: figures skipped")
        return
    if heap:
        fig, axes = plt.subplots(1, len(bins), figsize=(3.2 * len(bins), 3.4), sharey=True, squeeze=False)
        lists = sorted({r[1] for r in sp_rows})
        colors = {l: "C%d" % i for i, l in enumerate(lists)}
        for ax, b in zip(axes[0], bins):
            for l in lists:
                for pinned, style in ((1, "-"), (0, "--")):
                    pts = sorted((r[2], float(r[6])) for r in sp_rows if r[1] == l and r[3] == pinned and r[4] == b and r[6] != "")
                    if pts:
                        ax.plot([p[0] for p in pts], [p[1] for p in pts], style, marker="o", ms=3, color=colors[l],
                                label="%s %s" % (l, "pinned" if pinned else "free"))
            ax.axhline(1.0, color="k", lw=0.8)
            ax.set_xscale("log", base=2); ax.set_yscale("log"); ax.set_title("bin %s" % b); ax.set_xlabel("threads")
        axes[0][0].set_ylabel("speedup vs sequential heap")
        axes[0][-1].legend(fontsize=6)
        fig.suptitle("%s: parallel A*, time objective" % platform); fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "fig_speedup.png"), dpi=150); plt.close(fig)
        # breakdown: heap -> sorted list -> parallel T1 -> parallel Tmax (first list, unpinned if present)
        lst = lists[0] if lists else None
        if lst and slist:
            cand = [c for c in main if meta[c]["algo"] == "par" and meta[c]["list"] == lst]
            if cand:
                tmax = max(meta[c]["threads"] for c in cand)
                pick = lambda t: next((c for c in cand if meta[c]["threads"] == t and meta[c]["pinned"] == 0), next((c for c in cand if meta[c]["threads"] == t), None))
                steps = [("heap", heap), ("sorted list\n(no lock)", slist), ("par T1\n%s" % lst, pick(1)), ("par T%d\n%s" % (tmax, lst), pick(tmax))]
                fig, ax = plt.subplots(figsize=(6.5, 3.6))
                width = 0.8 / len(steps)
                for i, (label, c) in enumerate(steps):
                    if c is None:
                        continue
                    vals = [stats[c][b][0] / stats[heap][b][0] if b in stats[c] and b in stats[heap] else 0 for b in bins]
                    ax.bar([j + i * width for j in range(len(bins))], vals, width, label=label.replace("\n", " "))
                ax.set_xticks([j + 0.4 - width / 2 for j in range(len(bins))]); ax.set_xticklabels(["bin %s" % b for b in bins])
                ax.set_yscale("log"); ax.axhline(1, color="k", lw=0.8); ax.set_ylabel("time relative to heap"); ax.set_ylim(top=ax.get_ylim()[1] * 6); ax.legend(fontsize=7, ncol=2, loc="upper center")
                ax.set_title("%s: where the time goes" % platform); fig.tight_layout()
                fig.savefig(os.path.join(out_dir, "fig_breakdown.png"), dpi=150); plt.close(fig)
    grouped = defaultdict(list)
    for r in q_rows:
        if r[5] == "time" and r[2] == "seq" and r[10] != "":
            grouped[r[7]].append((r[6], float(r[10])))
    if len(grouped) > 1:
        fig, ax = plt.subplots(figsize=(5, 3.4))
        for w, pts in sorted(grouped.items()):
            by_rho = defaultdict(list)
            for rho, g in pts:
                by_rho[rho].append(g)
            xs = sorted(by_rho)
            ax.plot(xs, [statistics.mean(by_rho[x]) for x in xs], marker="o", label="inflation %d%%" % w)
        ax.set_xlabel("noise (permille)"); ax.set_ylabel("mean gap to optimum (ppm)"); ax.legend(fontsize=7)
        ax.set_title("%s: quality of the relaxed heuristic" % platform); fig.tight_layout()
        fig.savefig(os.path.join(out_dir, "fig_quality.png"), dpi=150); plt.close(fig)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", action="append", required=True)
    ap.add_argument("--results", default=os.path.join(ROOT, "results"))
    ap.add_argument("--no-figures", action="store_true")
    a = ap.parse_args()
    for p in a.platform:
        out_dir = os.path.join(a.results, p, "analysis")
        if glob.glob(os.path.join(a.results, p, "run*")):
            rows = load_platform(a.results, p)
            n = analyze(p, rows, out_dir, not a.no_figures)
            print("%s: %d scalar configurations, %d rows -> %s" % (p, n, len(rows), out_dir))
        prows = load_pareto(a.results, p)
        if prows:
            n = analyze_pareto(p, prows, out_dir)
            print("%s: %d vector configurations, %d rows -> %s" % (p, n, len(prows), out_dir))
        elif not glob.glob(os.path.join(a.results, p, "run*")):
            sys.exit("no run* or pareto_run* directory under %s" % os.path.join(a.results, p))


if __name__ == "__main__":
    main()
