#!/usr/bin/env python3
"""Why is the windowed optimistic list flat in the scalar thread curve?  One short experiment on one machine.

    python3 tools/opt_diag.py --platform ryzen-wsl2                         # 6 threads (physical cores), default allocator
    python3 tools/opt_diag.py --platform ryzen-wsl2 --jemalloc /usr/lib/x86_64-linux-gnu/libjemalloc.so.2
    python3 tools/opt_diag.py --platform ryzen-wsl2 --dry-run               # only print the commands

It runs the scalar window preset (config/presets.json) for win_coarse_ttas (reference) and win_opt_ttas, each in two kinds of run:
  * timing run   (no instrumentation): search time, speedup vs the sequential heap, peak memory;
  * diagnostic run (--diag --latency): validation attempts and retries, nodes walked by the lock-free search and by the validation walk
    from the head, failed compare-and-swaps on the retired-node stack, and the mean latency of add and pop_min.
A third block measures the mean latency of add and pop_min for plain and windowed lists at 1 thread and at the full thread count (default
allocator; the table of the report).  With --jemalloc the same runs are repeated with LD_PRELOAD (Linux only), which replaces malloc/free: if the optimistic list is limited
by the allocator, its speedup moves; if not, the cause is elsewhere (see the counters).  Output: results/<platform>/opt_diag/*.csv and
results/<platform>/opt_diag/opt_diag_summary.md (send this file to the group).  Standard library only.
"""
import argparse
import csv
import json
import os
import statistics
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LISTS = ["win_coarse_ttas", "win_opt_ttas"]
LATENCY_LISTS = ["coarse_ttas", "fine_ttas", "opt_ttas", "win_coarse_ttas", "win_fine_ttas", "win_opt_ttas"]  # per-operation latency table (report)


def default_binary(name):
    for rel in (name, name + ".exe", os.path.join("Release", name + ".exe"), os.path.join("Release", name)):
        path = os.path.join(ROOT, "build", rel)
        if os.path.exists(path):
            return path
    return os.path.join(ROOT, "build", name)


def read_rows(path):
    with open(path) as f:
        return [r for r in csv.DictReader(f) if r["warmup"] != "1"]


def total_ms(rows):
    """Median over repetitions of the summed search time of all queries (ms)."""
    per_rep = {}
    for r in rows:
        per_rep[r["rep"]] = per_rep.get(r["rep"], 0) + int(r["search_ns"])
    return statistics.median(per_rep.values()) / 1e6 if per_rep else float("nan")


def diag_sums(path):
    keys = ["lock_acq", "opt_attempts", "opt_retries", "opt_find_steps", "opt_validate_steps", "opt_retire_cas_fail"]
    tot = dict.fromkeys(keys, 0)
    if not os.path.exists(path):
        return tot
    with open(path) as f:
        for r in csv.DictReader(f):
            for k in keys:
                tot[k] += int(r.get(k) or 0)
    return tot


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", required=True)
    ap.add_argument("--threads", type=int, default=0, help="thread count (default: physical cores of the platform file)")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--jemalloc", default="", help="path of libjemalloc.so (Linux): also run with LD_PRELOAD")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--bench", default=default_binary("bench"))
    ap.add_argument("--snapshot", default=os.path.join(ROOT, "data", "snapshot_hcmc"))
    ap.add_argument("--profile", default=os.path.join(ROOT, "config", "profile.csv"))
    ap.add_argument("--queries", default=os.path.join(ROOT, "config", "queries.csv"))
    ap.add_argument("--golden", default=os.path.join(ROOT, "config", "golden_time.csv"))
    ap.add_argument("--out", default=os.path.join(ROOT, "results"))
    a = ap.parse_args()

    pf = os.path.join(a.out, "platform_%s.json" % a.platform)
    if not os.path.exists(pf):
        sys.exit("missing %s: run tools/inspect_platform.py --name %s first" % (pf, a.platform))
    platform = json.load(open(pf))
    threads = a.threads or platform["physical_cores"]
    presets = json.load(open(os.path.join(ROOT, "config", "presets.json")))
    sc = presets.get("scalar", presets)
    wdiv, wbatch = sc["window_div"], sc["batch"]
    outdir = os.path.join(a.out, a.platform, "opt_diag")
    os.makedirs(outdir, exist_ok=True)

    base = [a.bench, "--snapshot", a.snapshot, "--profile", a.profile, "--queries", a.queries, "--golden", a.golden,
            "--obj", "time", "--reps", str(a.reps), "--platform", a.platform]
    cpus = ",".join(str(x) for x in platform["pin_order_physical_first"][:threads])
    allocators = [("default", None)]
    if a.jemalloc:
        if not os.path.exists(a.jemalloc):
            sys.exit("jemalloc library not found: " + a.jemalloc)
        allocators.append(("jemalloc", a.jemalloc))

    jobs = [("seq", "seq", None, [ "--algo", "seq"], False)]
    for alloc, lib in allocators:
        for lst in LISTS:
            par = ["--algo", "par", "--threads", str(threads), "--list", lst, "--cpus", cpus, "--window-div", str(wdiv), "--batch", str(wbatch), "--window-skip-empty"]
            jobs.append(("%s_%s_time" % (alloc, lst), alloc, lib, par, False))
            jobs.append(("%s_%s_diag" % (alloc, lst), alloc, lib, par + ["--diag", "--latency"], True))

    def par_args(lst, t):
        args = ["--algo", "par", "--threads", str(t), "--list", lst, "--cpus", ",".join(str(x) for x in platform["pin_order_physical_first"][:t])]
        if lst.startswith("win_"):
            args += ["--window-div", str(wdiv), "--batch", str(wbatch), "--window-skip-empty"]
        return args
    for lst in LATENCY_LISTS:
        for t in sorted({1, threads}):
            jobs.append(("lat_%s_t%d" % (lst, t), "default", None, par_args(lst, t) + ["--diag", "--latency"], True))

    for name, alloc, lib, args, diag in jobs:
        path = os.path.join(outdir, name + ".csv")
        for ext in ("", ".diag.csv"):
            if os.path.exists(path + ext) and not a.dry_run:
                os.remove(path + ext)
        cmd = base + args + ["--out", path]
        env = dict(os.environ)
        if lib:
            env["LD_PRELOAD"] = lib
        print("\n>>> %s%s\n    %s" % (name, " (LD_PRELOAD=%s)" % lib if lib else "", " ".join(cmd)), flush=True)
        if a.dry_run:
            continue
        code = subprocess.call(cmd, cwd=ROOT, env=env)
        if code != 0:
            sys.exit("run %s failed with exit code %d" % (name, code))
    if a.dry_run:
        return

    seq = total_ms(read_rows(os.path.join(outdir, "seq.csv")))
    lines = ["# Optimistic list diagnostic: %s, %d threads, scalar preset (divisor %s, batch %s)" % (a.platform, threads, wdiv, wbatch), "",
             "Sequential heap: %.1f ms per full query set (median of %d repetitions)." % (seq, a.reps), "",
             "## Speed and memory (timing runs, no instrumentation)", "",
             "| allocator | list | total ms | speedup vs heap | peak MB |", "|---|---|---|---|---|"]
    for alloc, _ in allocators:
        for lst in LISTS:
            rows = read_rows(os.path.join(outdir, "%s_%s_time.csv" % (alloc, lst)))
            ms = total_ms(rows)
            rss = max(int(r["peak_rss_kb"]) for r in rows) / 1024.0 if rows else float("nan")
            bad = sum(1 for r in rows if r["path_ok"] != "1")
            lines.append("| %s | %s | %.1f | %.2f | %.0f |%s" % (alloc, lst, ms, seq / ms, rss, "  **%d bad rows**" % bad if bad else ""))
    lines += ["", "## Where the optimistic list spends its work (diagnostic runs)", "",
              "| allocator | list | attempts / op | retries / attempt | find steps / attempt | validate steps / attempt | retire CAS fails / pop | add ns | pop ns |",
              "|---|---|---|---|---|---|---|---|---|"]
    for alloc, _ in allocators:
        for lst in LISTS:
            base_path = os.path.join(outdir, "%s_%s_diag.csv" % (alloc, lst))
            rows = read_rows(base_path)
            adds = sum(int(r["list_adds"]) for r in rows)
            pops = sum(int(r["list_pops"]) for r in rows)
            ops = adds + pops
            d = diag_sums(base_path + ".diag.csv")
            add_s = sum(int(r["list_add_samples"]) for r in rows)
            pop_s = sum(int(r["list_pop_samples"]) for r in rows)
            add_ns = sum(int(r["list_add_ns"]) for r in rows) / add_s if add_s else float("nan")
            pop_ns = sum(int(r["list_pop_ns"]) for r in rows) / pop_s if pop_s else float("nan")
            if d["opt_attempts"]:
                lines.append("| %s | %s | %.2f | %.3f | %.1f | %.1f | %.3f | %.0f | %.0f |" % (
                    alloc, lst, d["opt_attempts"] / max(ops, 1), d["opt_retries"] / d["opt_attempts"], d["opt_find_steps"] / d["opt_attempts"],
                    d["opt_validate_steps"] / d["opt_attempts"], d["opt_retire_cas_fail"] / max(pops, 1), add_ns, pop_ns))
            else:
                lines.append("| %s | %s | - | - | - | - | - | %.0f | %.0f |" % (alloc, lst, add_ns, pop_ns))
    lines += ["", "## Latency per operation (ns, sampled 1 in 8 operations, default allocator)", "",
              "| list | threads | add ns | pop_min ns |", "|---|---|---|---|"]
    for lst in LATENCY_LISTS:
        for t in sorted({1, threads}):
            rows = read_rows(os.path.join(outdir, "lat_%s_t%d.csv" % (lst, t)))
            add_s = sum(int(r["list_add_samples"]) for r in rows)
            pop_s = sum(int(r["list_pop_samples"]) for r in rows)
            add_ns = sum(int(r["list_add_ns"]) for r in rows) / add_s if add_s else float("nan")
            pop_ns = sum(int(r["list_pop_ns"]) for r in rows) / pop_s if pop_s else float("nan")
            lines.append("| %s | %d | %.0f | %.0f |" % (lst, t, add_ns, pop_ns))
    lines += ["", "Reading the table: `attempts / op` counts lock-and-validate rounds of add/remove/contains (pop_min does not validate; WindowedList",
              "also batches, so one op here is one item); `validate steps` is the walk from the head that every attempt performs, `find steps` the",
              "lock-free walk before it.  If validate >> find the validation walk dominates; if retries are high the optimistic walk is being invalidated;",
              "if the jemalloc rows move relative to the default rows the allocator is involved.", ""]
    summary = os.path.join(outdir, "opt_diag_summary.md")
    with open(summary, "w") as f:
        f.write("\n".join(lines))
    print("\n".join(lines))
    print("written: " + summary)


if __name__ == "__main__":
    main()
