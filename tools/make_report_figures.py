#!/usr/bin/env python3
"""Report figures (PDF) and LaTeX table fragments from the experiment results. Needs matplotlib. Nothing is typed in by hand.

    python3 tools/make_report_figures.py --platform ryzen-wsl2 [--results results] [--out report/generated] [--decimal comma|point]

Reads results/<platform>/analysis/{speedup,quality,memory}.csv (run `tools/analyze.py` first), results/<platform>/window and
pareto_window (raw CSVs), and the optional list-profile files. Writes into <out>/<platform>/:
  fig_plain_speedup.pdf     plain lists, speedup vs the sequential heap against threads (pinned, all bins)
  fig_window_threads.pdf    windowed lists at the chosen presets, scalar and vector, against threads
  fig_window_sweep.pdf      windowed list, divisor x batch heat map at the largest thread count of the sweep
  fig_memory.pdf            peak memory of the whole process by list
  fig_insert_profile.pdf    where inserts land in the list (eighths), if the profile files exist
  tab_plain.tex, tab_window_threads.tex, tab_sensitivity.tex, fig_sensitivity.pdf, tab_vector_quality.tex, tab_latency.tex, tab_memory.tex   (\\input into the report)
Run it again after the final runs: the report then picks up the new numbers. A figure or table whose data is missing is skipped
with a message.
"""
import argparse
import csv
import glob
import os
import re
import statistics
import sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import analyze  # noqa: E402  (window_load / window_describe)

import matplotlib  # noqa: E402
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

SCALAR_PRESET = ("4096", "8", "off +skip")     # divisor, batch, slack (config/presets.json)
VECTOR_PRESET = ("16384", "12", "64 +skip")
DEC = "comma"


def num(x, digits=2):
    s = ("%." + str(digits) + "f") % x
    return s.replace(".", ",") if DEC == "comma" else s


def read_csv(path):
    if not os.path.exists(path):
        print("skip: %s not found" % path)
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def tex_name(s):
    return "\\texttt{%s}" % s.replace("_", "\\_")


def save(fig, out, name):
    fig.tight_layout()
    fig.savefig(os.path.join(out, name))
    plt.close(fig)
    print("wrote", os.path.join(out, name))


def plain_speedup(base, out):
    rows = [r for r in read_csv(os.path.join(base, "analysis", "speedup.csv")) if r["bin"] == "all" and r["pinned"] == "1"]
    if not rows:
        return
    curves = defaultdict(dict)
    for r in rows:
        curves[r["list"]][int(r["threads"])] = float(r["speedup_vs_heap"])
    fig, ax = plt.subplots(figsize=(5.2, 3.4))
    for lst in sorted(curves):
        xs = sorted(curves[lst])
        ax.plot(xs, [curves[lst][x] for x in xs], marker="o", ms=3, lw=1, label=lst)
    ax.axhline(1.0, color="k", lw=0.6, ls="--")
    ax.set_xlabel("threads"); ax.set_ylabel("speedup vs sequential heap"); ax.set_yscale("log")
    ax.legend(fontsize=6, ncol=2)
    save(fig, out, "fig_plain_speedup.pdf")
    threads = sorted({t for c in curves.values() for t in c})
    lines = ["\\begin{tabular}{l" + "r" * len(threads) + "}", "\\toprule", "danh sách & " + " & ".join(str(t) for t in threads) + " \\\\", "\\midrule"]
    for lst in sorted(curves):
        lines.append(tex_name(lst) + " & " + " & ".join(num(curves[lst][t], 3) if t in curves[lst] else "--" for t in threads) + " \\\\")
    lines += ["\\bottomrule", "\\end{tabular}"]
    open(os.path.join(out, "tab_plain.tex"), "w").write("\n".join(lines) + "\n")


def window_data(base, sub):
    d = os.path.join(base, sub)
    return analyze.window_load(d) if os.path.isdir(d) else {}


def window_threads(base, out):
    panels = (("window", "scalar", SCALAR_PRESET), ("pareto_window", "vector", VECTOR_PRESET))
    fig, axes = plt.subplots(1, 2, figsize=(7.4, 3.2))
    table = {}
    for ax, (sub, label, preset) in zip(axes, panels):
        data = window_data(base, sub)
        seq = next((v for n, v in data.items() if analyze.window_describe(n)[0] == "seq (heap)"), None)
        if not seq:
            continue
        curves = defaultdict(dict)
        for n, v in data.items():
            lst, t, wd, wb, sl = analyze.window_describe(n)
            if lst == "seq (heap)" or (wd, wb, sl) != preset or "all" not in v["batch"]:
                continue
            curves[lst][t] = seq["batch"]["all"] / v["batch"]["all"]
        for lst in sorted(curves):
            xs = sorted(curves[lst])
            ax.plot(xs, [curves[lst][x] for x in xs], marker="o", ms=3, lw=1, label=lst)
        ax.axhline(1.0, color="k", lw=0.6, ls="--")
        ax.set_title("%s (div %s, batch %s, slack %s)" % (label, *preset), fontsize=8)
        ax.set_xlabel("threads"); ax.set_ylabel("speedup vs sequential heap"); ax.legend(fontsize=6)
        table[label] = curves
    if not table:
        print("skip: no window sweeps")
        return
    save(fig, out, "fig_window_threads.pdf")
    threads = sorted({t for cs in table.values() for c in cs.values() for t in c})
    lines = ["\\begin{tabular}{ll" + "r" * len(threads) + "}", "\\toprule", "tìm kiếm & danh sách & " + " & ".join(str(t) for t in threads) + " \\\\", "\\midrule"]
    for k, (label, curves) in enumerate(table.items()):
        for lst in sorted(curves):
            lines.append("%s & %s & %s \\\\" % (label, tex_name(lst), " & ".join(num(curves[lst][t]) if t in curves[lst] else "--" for t in threads)))
        if k < len(table) - 1:
            lines.append("\\midrule")
    lines += ["\\bottomrule", "\\end{tabular}"]
    open(os.path.join(out, "tab_window_threads.tex"), "w").write("\n".join(lines) + "\n")


def window_sweep(base, out):
    fig, axes = plt.subplots(1, 2, figsize=(7.4, 3.2))
    drew = False
    for ax, (sub, label) in zip(axes, (("window", "scalar"), ("pareto_window", "vector"))):
        data = window_data(base, sub)
        seq = next((v for n, v in data.items() if analyze.window_describe(n)[0] == "seq (heap)"), None)
        if not seq:
            continue
        by_threads = defaultdict(dict)  # threads -> {(divisor, batch): speedup}; slack off, no skip-empty: the plain sweep grid
        for n, v in data.items():
            lst, t, wd, wb, sl = analyze.window_describe(n)
            if lst != "win_coarse_ttas" or sl not in ("off", "") or "all" not in v["batch"] or not wd:
                continue
            by_threads[t][(int(wd), int(wb))] = seq["batch"]["all"] / v["batch"]["all"]
        if not by_threads:
            continue
        top = max(by_threads, key=lambda t: (len(by_threads[t]), t))  # the thread count with the most cells (ties: more threads)
        cells = by_threads[top]
        if not cells:
            continue
        wds = sorted({k[0] for k in cells}); wbs = sorted({k[1] for k in cells})
        grid = [[cells.get((d, b), float("nan")) for b in wbs] for d in wds]
        im = ax.imshow(grid, cmap="viridis", aspect="auto", origin="lower")
        ax.set_xticks(range(len(wbs))); ax.set_xticklabels(wbs, fontsize=6)
        ax.set_yticks(range(len(wds))); ax.set_yticklabels(wds, fontsize=6)
        for i, d in enumerate(wds):
            for j, b in enumerate(wbs):
                if (d, b) in cells:
                    ax.text(j, i, num(cells[(d, b)]), ha="center", va="center", fontsize=5, color="w")
        ax.set_xlabel("batch"); ax.set_ylabel("window divisor")
        ax.set_title("%s, win_coarse_ttas, %d threads, slack off" % (label, top), fontsize=7)
        fig.colorbar(im, ax=ax, fraction=0.046)
        drew = True
    if drew:
        save(fig, out, "fig_window_sweep.pdf")
    else:
        plt.close(fig); print("skip: window sweep grid")


def memory(base, out):
    rows = read_csv(os.path.join(base, "analysis", "memory.csv"))
    plain = {}
    for r in rows:
        name = r.get("config", "")
        m = re.match(r"par_(.+?)_T(\d+)_pin_time_", name)
        if m and m.group(2) == "6":
            plain[m.group(1)] = float(r.get("peak_rss_kb") or r.get("max_rss_kb") or 0) / 1024.0
        elif name.startswith("seq_") and "time" in name:
            plain["seq (heap)"] = float(r.get("peak_rss_kb") or r.get("max_rss_kb") or 0) / 1024.0
    vector = {}
    data = window_data(base, "pareto_window")
    top = max((analyze.window_describe(n)[1] for n in data), default=0)
    for n, v in data.items():
        lst, t, wd, wb, sl = analyze.window_describe(n)
        if lst == "seq (heap)":
            vector[lst] = v["rss_mb"]
        elif t == top and (wd, wb, sl) == VECTOR_PRESET:
            vector[lst] = v["rss_mb"]
    if not plain and not vector:
        print("skip: no memory data"); return
    fig, axes = plt.subplots(1, 2, figsize=(7.4, 3.2))
    for ax, d, title in ((axes[0], plain, "scalar, plain lists, 6 threads"), (axes[1], vector, "vector, windowed lists, %d threads" % top)):
        names = sorted(d)
        ax.barh(names, [d[n] for n in names]); ax.set_xlabel("peak RSS (MB)"); ax.set_title(title, fontsize=8)
        ax.tick_params(axis="y", labelsize=6)
    save(fig, out, "fig_memory.pdf")
    lines = ["\\begin{tabular}{lr}", "\\toprule", "danh sách & RSS đỉnh (MB) \\\\", "\\midrule"]
    for title, d in (("scalar, 6 luồng", plain), ("vector có cửa sổ", vector)):
        lines.append("\\multicolumn{2}{l}{\\emph{%s}} \\\\" % title)
        lines += ["%s & %s \\\\" % (tex_name(n), num(d[n], 0)) for n in sorted(d)]
    lines += ["\\bottomrule", "\\end{tabular}"]
    open(os.path.join(out, "tab_memory.tex"), "w").write("\n".join(lines) + "\n")


def sensitivity(base, out):
    rows = read_csv(os.path.join(base, "analysis", "quality.csv"))
    rows = [r for r in rows if r.get("obj", "time") == "time"]
    if not rows:
        return
    keyset = defaultdict(list)
    for r in rows:
        algo = r["algo"]
        if algo == "par" and not (r["list"] == "coarse_ttas" and r["threads"] == "6"):
            continue
        keyset[(algo, int(r["rho"]), int(r["w"]))].append(r)
    lines = ["\\begin{tabular}{lrrrrr}", "\\toprule", "thuật toán & nhiễu (\\textperthousand) & phóng đại (\\%) & sai khác TB (ppm) & không tối ưu (\\%) & mở rộng/truy vấn \\\\", "\\midrule"]
    for (algo, rho, w), rs in sorted(keyset.items()):
        def avg(col):
            return statistics.mean(float(r[col]) for r in rs)
        lines.append("%s & %d & %d & %s & %s & %s \\\\" % (algo, rho, w, num(avg("gap_mean_ppm"), 1), num(avg("non_optimal_pct"), 2), num(avg("expansions_per_query"), 0)))
    lines += ["\\bottomrule", "\\end{tabular}"]
    open(os.path.join(out, "tab_sensitivity.tex"), "w").write("\n".join(lines) + "\n")
    print("wrote", os.path.join(out, "tab_sensitivity.tex"))


def sensitivity_figure(base, out):
    """Quality/work trade-off: each point is one (noise, inflation) setting, sequential search, time objective."""
    rows = read_csv(os.path.join(base, "analysis", "quality.csv"))
    rows = [r for r in rows if r.get("obj", "time") == "time" and r["algo"] == "seq"]
    if not rows:
        return
    groups = defaultdict(list)
    for r in rows:
        groups[(int(r["rho"]), int(r["w"]))].append(r)
    fig, ax = plt.subplots(figsize=(6.2, 3.8))
    colours = {0: "#1b6ca8", 100: "#4c9f70", 250: "#d98e04", 500: "#b5352b"}
    marks = {100: "o", 120: "s", 150: "^"}
    for (rho, w), rs in sorted(groups.items()):
        x = statistics.mean(float(r["expansions_per_query"]) for r in rs)
        y = statistics.mean(float(r["gap_mean_ppm"]) for r in rs) / 1e4  # ppm -> percent
        ax.scatter(x, max(y, 1e-4), s=48, c=colours.get(rho, "grey"), marker=marks.get(w, "o"), zorder=3)
    ax.set_yscale("symlog", linthresh=1e-2)
    ax.set_ylim(bottom=0)
    ax.set_xlabel("nút mở rộng mỗi truy vấn")
    ax.set_ylabel("sai khác trung bình so với tối ưu (%)")
    ax.grid(alpha=0.3)
    from matplotlib.lines import Line2D
    handles = [Line2D([], [], marker="o", ls="", c=c, label="nhiễu %d\u2030" % r) for r, c in colours.items()]
    handles += [Line2D([], [], marker=m, ls="", c="grey", label="phóng đại %d%%" % w) for w, m in marks.items()]
    ax.legend(handles=handles, fontsize=7, ncol=2, loc="lower left")
    save(fig, out, "fig_sensitivity.pdf")


def vector_quality(base, out):
    """Hypervolume ratio against the exact fronts, by heuristic setting (seq and parallel win_coarse_ttas at 6 threads)."""
    rows = read_csv(os.path.join(base, "analysis", "pareto_quality.csv"))
    if not rows:
        return
    keyset = defaultdict(list)
    for r in rows:
        if r["algo"] == "par" and not (r["list"] == "win_coarse_ttas" and r["threads"] == "6"):
            continue
        keyset[(r["algo"], int(r["rho"]), int(r["w"]))].append(r)
    lines = ["\\begin{tabular}{lrrrrrr}", "\\toprule",
             "thuật toán & nhiễu (\\textperthousand) & phóng đại (\\%) & HV TB & HV nhỏ nhất & điểm đúng tìm thấy (\\%) & mở rộng/truy vấn \\\\", "\\midrule"]
    for (algo, rho, w), rs in sorted(keyset.items()):
        def avg(col):
            return statistics.mean(float(r[col]) for r in rs)
        lines.append("%s & %d & %d & %s & %s & %s & %s \\\\" % (
            algo, rho, w, num(avg("hv_ratio_mean"), 3), num(min(float(r["hv_ratio_min"]) for r in rs), 3),
            num(avg("exact_points_found_pct"), 1), num(avg("expansions_per_query"), 0)))
    lines += ["\\bottomrule", "\\end{tabular}"]
    open(os.path.join(out, "tab_vector_quality.tex"), "w").write("\n".join(lines) + "\n")
    print("wrote", os.path.join(out, "tab_vector_quality.tex"))


def latency(base, out):
    """Per-operation latency table from tools/opt_diag.py (results/<platform>/opt_diag/opt_diag_summary.md)."""
    path = os.path.join(base, "opt_diag", "opt_diag_summary.md")
    if not os.path.exists(path):
        print("skip: %s not found" % path)
        return
    rows, on = {}, False
    for line in open(path):
        if line.startswith("## Latency"):
            on = True
            continue
        if on and line.startswith("## "):
            break
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if on and len(cells) == 4 and cells[1].isdigit():
            rows.setdefault(cells[0], {})[int(cells[1])] = (float(cells[2]), float(cells[3]))
    if not rows:
        print("skip: no latency rows in", path)
        return
    threads = sorted({t for r in rows.values() for t in r})
    head = " & ".join("add (%d) & pop (%d)" % (t, t) for t in threads)
    lines = ["\\begin{tabular}{l" + "rr" * len(threads) + "}", "\\toprule", "danh sách & " + head + " \\\\", "\\midrule"]
    for name, r in rows.items():
        cells = []
        for t in threads:
            a, p_ = r.get(t, (float("nan"), float("nan")))
            cells += [num(a, 0), num(p_, 0)]
        lines.append("%s & %s \\\\" % (tex_name(name), " & ".join(cells)))
    lines += ["\\bottomrule", "\\end{tabular}"]
    open(os.path.join(out, "tab_latency.tex"), "w").write("\n".join(lines) + "\n")
    print("wrote", os.path.join(out, "tab_latency.tex"))


def insert_profile(base, out):
    fig, axes = plt.subplots(1, 2, figsize=(7.4, 3.0))
    drew = False
    for ax, (pattern, label) in zip(axes, (("listprof_scalar*.listprof.csv", "scalar"), ("listprof_vector*.listprof.csv", "vector"))):
        files = glob.glob(os.path.join(base, pattern))
        if not files:
            continue
        pos = [0] * 8
        steps = size = n = 0
        for r in read_csv(files[0]):
            for k in range(8):
                pos[k] += int(r["pos%d" % k])
            steps += int(r["ins_steps"]); size += int(r["ins_size_sum"]); n += int(r["ins_profiled"])
        total = sum(pos)
        if not total:
            continue
        ax.bar(range(8), [100.0 * p / total for p in pos])
        ax.set_xlabel("vị trí chèn (tám phần của danh sách)"); ax.set_ylabel("% lần chèn")
        ax.set_title("%s: duyệt TB %.0f%% độ dài danh sách" % (label, 100.0 * steps / size if size else 0), fontsize=8)
        drew = True
    if drew:
        save(fig, out, "fig_insert_profile.pdf")
    else:
        plt.close(fig); print("skip: no list-profile files")


def main():
    global DEC
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", required=True)
    ap.add_argument("--results", default=os.path.join(os.path.dirname(HERE), "results"))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(HERE), "report", "generated"))
    ap.add_argument("--decimal", choices=("comma", "point"), default="comma")
    a = ap.parse_args()
    DEC = a.decimal
    base = os.path.join(a.results, a.platform)
    out = os.path.join(a.out, a.platform)
    os.makedirs(out, exist_ok=True)
    for step in (plain_speedup, window_threads, window_sweep, memory, sensitivity, sensitivity_figure, vector_quality, latency, insert_profile):
        step(base, out)


if __name__ == "__main__":
    main()
