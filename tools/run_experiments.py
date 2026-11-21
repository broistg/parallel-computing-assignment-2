#!/usr/bin/env python3
"""Experiment driver: runs the whole measurement grid of one platform (standard library only).

    python3 tools/run_experiments.py --platform ryzen-wsl2 --dry-run          # list the grid + time estimate
    python3 tools/run_experiments.py --platform ryzen-wsl2                    # run it (resumable)
    python3 tools/run_experiments.py --platform ryzen-wsl2 --grid smoke       # 1 minute self-check

Protocol (docs/PARAMETERS.md, REPORT.md decision 20):
  * ONE bench process per configuration (so peak memory belongs to that configuration), 10 repetitions + 1 warm-up
    inside the process, medians and IQR are computed later by the analysis scripts;
  * configuration order is shuffled with a fixed seed so slow drift of the machine (thermal state, other load)
    is not confounded with one algorithm; --passes N repeats the whole grid N times in separate directories;
  * pinned and unpinned runs are separate configurations (a pinned run is only listed when the platform file
    says pinning is possible; the CSV column `pinned` still records what really happened);
  * output: results/<platform>/run<k>/<config-name>.csv plus results/<platform>/run<k>/manifest.json
    (the smoke grid writes to results/<platform>/smoke/ so it can never mix with real runs)
    (command, return code, wall time, git commit, correctness verdict per configuration);
  * resumable: a configuration whose CSV exists and is marked done in the manifest is skipped; a timed-out or failed
    configuration leaves a .partial file that is never used by the analysis;
  * exact configurations (noise 0, inflation 100) must reproduce the golden cost on every row; a violation is
    reported at the end and the process exit code is 2.

Grids:
  headline  objective time, exact heuristic: seq, seqlist, and par x lists x thread counts x {pinned, unpinned};
            objective dist (secondary) for a few thread counts; heuristic sensitivity (noise rho x inflation w, 3 seeds for
            rho > 0) for seq and one parallel configuration. This is the grid for the report.
  smoke     tiny grid for checking the toolchain on any machine (rows are tagged smoke=1, never used in the report).
  pareto    vector (Pareto) search, reduced grid (build/bench_pareto, config/queries_pareto.csv, config/golden_pareto.csv):
            sequential, parallel x lists x {1, 2, physical cores} pinned, and a short noise/inflation set; 5 repetitions.
            Output in results/<platform>/pareto_run<k>/ (its CSV schema differs from the scalar one).
  window        sweep of the windowed open list (divisor 64..4096 x batch 1..32, 1 and physical-core threads) vs seq (and, scalar only, plain coarse_ttas / fine_ttas).
                Output in results/<platform>/window/.   pareto-window: the same for the vector search (pareto_window/).
  pareto-smoke  two configurations, 2 repetitions, writes to results/<platform>/pareto_smoke/.
"""
import argparse
import csv
import json
import os
import random
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
RHOS = [0, 100, 250, 500]
WEIGHTS = [100, 120, 150]
NOISE_SEEDS = [1, 2, 3]


def load_platform(name):
    path = os.path.join(ROOT, "results", "platform_%s.json" % name)
    if not os.path.exists(path):
        sys.exit("missing %s: run  python3 tools/inspect_platform.py --name %s  first" % (path, name))
    return json.load(open(path))


def query_bin_counts(path):
    counts = {}
    with open(path) as f:
        for r in csv.DictReader(f):
            counts[int(r["bin"])] = counts.get(int(r["bin"]), 0) + 1
    return counts


class Config:
    def __init__(self, algo, lst, threads, pinned, obj, rho, w, seed, vector=False, wdiv=4096, wbatch=8, wslack=-1, wskip=False):
        self.wdiv, self.wbatch, self.wslack, self.wskip = wdiv, wbatch, wslack, wskip  # only used by win_* lists (wslack -1 = off)
        self.algo, self.list, self.threads, self.pinned = algo, lst, threads, pinned
        self.obj, self.rho, self.w, self.seed, self.vector = obj, rho, w, seed, vector

    @property
    def name(self):
        base = "%s" % self.algo if self.algo != "par" else "par_%s_T%d_%s" % (self.list, self.threads, "pin" if self.pinned else "free")
        if self.algo == "par" and self.list.startswith("win_"):
            base += "_wd%d_wb%d" % (self.wdiv, self.wbatch)
            if self.wslack >= 0:  # off = no suffix, so the names of finished configurations stay valid
                base += "_sl%d" % self.wslack
            if self.wskip:
                base += "_se"
        return "%s%s_%s_rho%d_w%d_s%d" % ("pareto_" if self.vector else "", base, self.obj, self.rho, self.w, self.seed)

    @property
    def exact(self):
        return self.rho == 0 and self.w == 100


def build_grid(kind, platform, lists, threads, window=None):
    window = window or {}
    phys = platform.get("physical_cores") or max(threads)
    logical = platform.get("logical_cores") or max(threads)
    can_pin = bool(platform.get("pinning_possible"))
    cfgs = []

    def add(*args):
        cfgs.append(Config(*args))

    if kind in ("window", "pareto-window"):
        # Sweep of the WindowedList parameters (window divisor x pop/add batch) at physical cores and 1 thread, plus baselines.
        vec = kind == "pareto-window"
        obj = "vector" if vec else "time"
        cfgs.append(Config("seq", "pq", 1, False, obj, 0, 100, 1, vector=vec))
        wl = [l for l in lists if l.startswith("win_")] or ["win_coarse_ttas"]
        for lst in wl:  # every win_ list given in --lists
            for t in window.get("threads") or sorted({1, phys}):
                for wdiv in window.get("divs") or (64, 256, 1024, 4096):
                    for wb in window.get("batches") or (1, 8, 16, 32):
                        for sl in window.get("slacks") or (-1,):
                            cfgs.append(Config("par", lst, t, can_pin, obj, 0, 100, 1, vector=vec, wdiv=wdiv, wbatch=wb, wslack=sl, wskip=window.get("skip", False)))
        if not vec:  # plain lists on the vector search need about 11 minutes per pass on the long bins: measured separately (TASKS A.13)
            cfgs.append(Config("par", "coarse_ttas", phys, can_pin, obj, 0, 100, 1, vector=vec))
            cfgs.append(Config("par", "fine_ttas", phys, can_pin, obj, 0, 100, 1, vector=vec))
        return cfgs

    if kind in ("pareto", "pareto-smoke"):
        # Vector (Pareto) search is much heavier per query and the sorted-list open list grows very large, so the grid is reduced:
        # pinned runs only, thread counts {1, 2, physical cores}, the sequential baseline, and a short heuristic-sensitivity set.
        def addv(*args):
            cfgs.append(Config(*args, vector=True))
        addv("seq", "pq", 1, False, "vector", 0, 100, 1)
        pthreads = sorted({1, 2, phys} & set(threads)) or threads[:1]
        plists = lists[:1] if kind == "pareto-smoke" else lists
        for lst in plists:
            for t in ([2] if kind == "pareto-smoke" else pthreads):
                addv("par", lst, t, can_pin, "vector", 0, 100, 1)
        if kind == "pareto":
            sens_t = phys if phys in threads else threads[-1]
            for rho, w in ((250, 100), (500, 100), (0, 120), (0, 150), (250, 120)):
                for seed in ([1] if rho == 0 else NOISE_SEEDS[:2]):
                    addv("seq", "pq", 1, False, "vector", rho, w, seed)
                    addv("par", lists[0], sens_t, can_pin, "vector", rho, w, seed)
        return cfgs
    if kind == "smoke":
        add("seq", "pq", 1, False, "time", 0, 100, 1)
        for lst in lists[:1]:
            add("par", lst, 2, False, "time", 0, 100, 1)
        return cfgs
    # --- main study: objective time, exact heuristic
    add("seq", "pq", 1, False, "time", 0, 100, 1)
    add("seqlist", "coarse_ttas", 1, False, "time", 0, 100, 1)
    for lst in lists:
        for t in threads:
            for pinned in ([False, True] if can_pin else [False]):
                add("par", lst, t, pinned, "time", 0, 100, 1)
    # --- secondary objective: distance (fewer thread counts)
    dist_threads = sorted({1, 2, phys, logical} & set(threads)) or threads[:1]
    add("seq", "pq", 1, False, "dist", 0, 100, 1)
    for lst in lists:
        for t in dist_threads:
            add("par", lst, t, can_pin, "dist", 0, 100, 1)
    # --- heuristic sensitivity: noise x inflation; sequential and one parallel configuration
    sens_t = phys if phys in threads else threads[-1]
    for rho in RHOS:
        for w in WEIGHTS:
            if rho == 0 and w == 100:
                continue  # the exact case is in the main study
            for seed in ([1] if rho == 0 else NOISE_SEEDS):
                add("seq", "pq", 1, False, "time", rho, w, seed)
                add("par", lists[0], sens_t, can_pin, "time", rho, w, seed)
    return cfgs


def estimate_hours(cfgs, platform_name, queries_path, reps, pilot_path):
    """Rough planning estimate from the pilot (per-query sequential medians x measured par/seq slowdown)."""
    if not os.path.exists(pilot_path):
        return None
    pilot = json.load(open(pilot_path))
    seq_ms = {int(b): v for b, v in pilot["median_seq_ms_per_bin"].items()}
    counts = query_bin_counts(queries_path)
    seq_batch_ms = sum(seq_ms.get(b, 0.0) * n for b, n in counts.items())
    slow = pilot.get("par_time_over_seq_time_median_over_bins", {})

    def slowdown(c):
        if c.algo == "seq":
            return 1.0
        if c.algo == "seqlist":
            return 2.5
        known = sorted(int(k.rsplit("_T", 1)[1]) for k in slow if k.startswith(c.list + "_T"))
        if not known:
            return 10.0
        pick = max([t for t in known if t <= c.threads] or known[:1])
        return slow["%s_T%d" % (c.list, pick)]
    total_s = sum((reps + 1) * seq_batch_ms * slowdown(c) / 1000.0 for c in cfgs)
    return total_s / 3600.0


def git_state():
    def out(*cmd):
        try:
            return subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT).stdout.strip()
        except OSError:
            return ""
    return {"commit": out("git", "rev-parse", "HEAD"), "dirty": bool(out("git", "status", "--porcelain"))}


def command_for(c, a, platform, csv_path):
    if c.vector:
        cmd = [a.bench_pareto, "--snapshot", a.snapshot, "--profile", a.profile, "--queries", a.queries_pareto, "--golden", a.golden_pareto,
               "--algo", c.algo, "--threads", str(c.threads), "--list", c.list, "--rho", str(c.rho), "--w", str(c.w), "--seed", str(c.seed),
               "--reps", str(a.reps), "--platform", a.platform, "--out", csv_path]
        if c.algo == "par" and c.pinned:
            cmd += ["--cpus", ",".join(str(x) for x in platform["pin_order_physical_first"][:c.threads])]
        if c.algo == "par" and c.list.startswith("win_"):
            cmd += ["--window-div", str(c.wdiv), "--batch", str(c.wbatch)]
            if c.wslack >= 0:
                cmd += ["--window-slack", str(c.wslack)]
            if c.wskip:
                cmd += ["--window-skip-empty"]
        if a.grid == "pareto-smoke":
            cmd += ["--smoke"]
        return cmd
    cmd = [a.bench, "--snapshot", a.snapshot, "--profile", a.profile, "--queries", a.queries,
           "--golden", a.golden_dist if c.obj == "dist" else a.golden_time,
           "--algo", "par" if c.algo == "par" else c.algo, "--threads", str(c.threads), "--list", c.list,
           "--obj", c.obj, "--rho", str(c.rho), "--w", str(c.w), "--seed", str(c.seed), "--reps", str(a.reps),
           "--platform", a.platform, "--out", csv_path]
    if c.algo == "par" and c.pinned:
        cmd += ["--cpus", ",".join(str(x) for x in platform["pin_order_physical_first"][:c.threads])]
    if c.algo == "par" and c.list.startswith("win_"):
        cmd += ["--window-div", str(c.wdiv), "--batch", str(c.wbatch)]
        if c.wslack >= 0:
            cmd += ["--window-slack", str(c.wslack)]
        if c.wskip:
            cmd += ["--window-skip-empty"]
    if a.grid == "smoke":
        cmd += ["--smoke"]
    return cmd


def verdict(csv_path, exact, vector=False):
    """(rows, bad rows). Bad = path check failed, or (exact configuration only) cost / front differs from the golden one."""
    rows = bad = 0
    with open(csv_path) as f:
        for r in csv.DictReader(f):
            if r["warmup"] == "1":
                continue
            rows += 1
            if vector:
                wrong_front = exact and not (r["front_size"] == r["golden_front_size"] == r["exact_points_found"] and r["hv_ratio_ppm"] == "1000000")
                if r["path_ok"] != "1" or r["overflow"] != "0" or r["dominates_golden"] not in ("0", "-1") or wrong_front:
                    bad += 1
            elif r["path_ok"] != "1" or (exact and r["gap_ppm"] not in ("", "0")):
                bad += 1
    return rows, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", required=True)
    ap.add_argument("--grid", choices=["headline", "smoke", "pareto", "pareto-smoke", "window", "pareto-window"], default="headline")
    ap.add_argument("--snapshot", default=os.path.join(ROOT, "data", "snapshot_hcmc"))
    ap.add_argument("--profile", default=os.path.join(ROOT, "config", "profile.csv"))
    ap.add_argument("--queries", default=os.path.join(ROOT, "config", "queries.csv"))
    ap.add_argument("--golden-time", dest="golden_time", default=os.path.join(ROOT, "config", "golden_time.csv"))
    ap.add_argument("--golden-dist", dest="golden_dist", default=os.path.join(ROOT, "config", "golden_dist.csv"))
    ap.add_argument("--bench", default=os.path.join(ROOT, "build", "bench"))
    ap.add_argument("--bench-pareto", dest="bench_pareto", default=os.path.join(ROOT, "build", "bench_pareto"))
    ap.add_argument("--queries-pareto", dest="queries_pareto", default=os.path.join(ROOT, "config", "queries_pareto.csv"))
    ap.add_argument("--golden-pareto", dest="golden_pareto", default=os.path.join(ROOT, "config", "golden_pareto.csv"))
    ap.add_argument("--lists", default="coarse_ttas,coarse_ticket,coarse_std_mutex",
                    help="open lists to measure (Section-2 lists: add their registry names here)")
    ap.add_argument("--window-divs", default="", help="window grids only: comma list of window divisors (default 64,256,1024,4096)")
    ap.add_argument("--window-batches", default="", help="window grids only: comma list of batch sizes (default 1,8,16,32)")
    ap.add_argument("--window-slacks", default="", help="window grids only: comma list of window slacks K, off = no limit (default off), e.g. off,8,16; see docs/INTERFACES.md")
    ap.add_argument("--window-skip-empty", dest="window_skip_empty", action="store_true", help="window grids only: run every win_ configuration with --window-skip-empty (names get the suffix _se)")
    ap.add_argument("--window-threads", default="", help="window grids only: comma list of thread counts (default 1 and the physical core count)")
    ap.add_argument("--threads", default="", help="comma list; default = suggested_thread_counts of the platform file")
    ap.add_argument("--reps", type=int, default=0, help="repetitions per configuration (default 10; 5 for the Pareto grid)")
    ap.add_argument("--passes", type=int, default=1)
    ap.add_argument("--shuffle-seed", type=int, default=2026)
    ap.add_argument("--skip-timeouts", action="store_true", help="do not retry configurations the manifest records as timed out (default: retry them)")
    ap.add_argument("--timeout-min", type=float, default=45.0, help="kill one configuration after this many minutes")
    ap.add_argument("--only", default="", help="run only configurations whose name contains this text")
    ap.add_argument("--out", default=os.path.join(ROOT, "results"))
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    vector_grid = a.grid.startswith("pareto")  # pareto, pareto-smoke, pareto-window
    if a.reps <= 0:
        a.reps = 5 if vector_grid else 10
    if a.grid in ("smoke", "pareto-smoke"):
        a.reps = min(a.reps, 2)

    platform = load_platform(a.platform)
    threads = [int(x) for x in a.threads.split(",")] if a.threads else list(platform["suggested_thread_counts"])
    lists = a.lists.split(",")
    as_ints = lambda text: [int(x) for x in text.split(",") if x.strip()]
    cfgs = build_grid(a.grid, platform, lists, threads,
                      {"divs": as_ints(a.window_divs), "batches": as_ints(a.window_batches), "threads": as_ints(a.window_threads), "skip": a.window_skip_empty, "slacks": [-1 if x.strip() == "off" else int(x) for x in a.window_slacks.split(",") if x.strip()]})
    if a.only:
        cfgs = [c for c in cfgs if a.only in c.name]
    names = [c.name for c in cfgs]
    assert len(names) == len(set(names)), "duplicate configuration names"

    est = None if vector_grid else estimate_hours(cfgs, a.platform, a.queries, a.reps, os.path.join(ROOT, "results", "pilot_%s.json" % a.platform))
    print("platform %s: %d configurations x %d pass(es), threads %s, lists %s" % (a.platform, len(cfgs), a.passes, threads, lists))
    if est is not None:
        print("rough estimate from the pilot: %.2f h per pass (planning only; budget is 5 h per platform)" % est)
    if a.dry_run:
        for c in cfgs:
            print("  " + c.name)
        return 0
    binary = a.bench_pareto if vector_grid else a.bench
    if not os.path.exists(binary):
        sys.exit("benchmark binary not found at %s (run make)" % binary)

    failures = []
    for k in range(1, a.passes + 1):
        run_dir = os.path.join(a.out, a.platform, {"smoke": "smoke", "pareto-smoke": "pareto_smoke", "window": "window", "pareto-window": "pareto_window"}.get(a.grid, ("pareto_run%d" if vector_grid else "run%d") % k))
        os.makedirs(run_dir, exist_ok=True)
        man_path = os.path.join(run_dir, "manifest.json")
        manifest = json.load(open(man_path)) if os.path.exists(man_path) else {"git": git_state(), "platform": a.platform, "configs": {}}
        order = list(cfgs)
        random.Random(a.shuffle_seed + k).shuffle(order)
        t_start = time.time()
        for n, c in enumerate(order, 1):
            csv_path = os.path.join(run_dir, c.name + ".csv")
            previous = manifest["configs"].get(c.name, {}).get("status")
            if previous == "done" and os.path.exists(csv_path):
                continue
            if previous == "timeout" and a.skip_timeouts:
                continue  # known to exceed --timeout-min: do not spend the time again
            for stale in (csv_path, csv_path + ".partial"):
                if os.path.exists(stale):
                    os.remove(stale)
            cmd = command_for(c, a, platform, csv_path)
            t0 = time.time()
            status, note = "done", ""
            try:
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=a.timeout_min * 60)
                if r.returncode:
                    status, note = "failed", r.stderr.strip()[-300:]
            except subprocess.TimeoutExpired:
                status, note = "timeout", ">%.0f min" % a.timeout_min
            wall = time.time() - t0
            rows = bad = 0
            if status == "done":
                rows, bad = verdict(csv_path, c.exact, c.vector)
                if bad:
                    status, note = "incorrect", "%d of %d rows differ from golden / failed path check" % (bad, rows)
            if status != "done" and os.path.exists(csv_path):
                os.replace(csv_path, csv_path + ".partial")
            manifest["configs"][c.name] = {"status": status, "note": note, "wall_s": round(wall, 1), "rows": rows,
                                          "command": " ".join(cmd), "threads": c.threads, "pinned": c.pinned}
            json.dump(manifest, open(man_path, "w"), indent=1)
            if status != "done":
                failures.append((k, c.name, status, note))
            print("[run%d %d/%d] %-62s %-9s %7.1fs  elapsed %.1f min %s" % (
                k, n, len(order), c.name, status, wall, (time.time() - t_start) / 60, note))
    if failures:
        print("\nPROBLEMS:")
        for k, name, status, note in failures:
            print("  run%d %s: %s %s" % (k, name, status, note))
        return 2 if any(s == "incorrect" for _, _, s, _ in failures) else 1
    print("\nall configurations done, results in %s" % os.path.join(a.out, a.platform))
    return 0


if __name__ == "__main__":
    sys.exit(main())
