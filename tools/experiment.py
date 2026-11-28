#!/usr/bin/env python3
"""One command per machine: build, test, record the platform, run the measurement stages, analyse.
Standard library only; works on Linux, WSL2, macOS and native Windows (cmd / PowerShell).

    python3 tools/experiment.py --platform <short-id> --list              # show the stages
    python3 tools/experiment.py --platform <short-id> --stages setup,tests,inspect,smoke
    python3 tools/experiment.py --platform <short-id> --stages main,fine,vector --dry-run
    python3 tools/experiment.py --platform <short-id>                     # all stages, in order (several hours)

Stages (each one is resumable; run the same command again after an interruption):
  setup         lock.h present? build (make, or CMake on native Windows)
  tests         tests/run_tests.sh (POSIX) / the two C++ test programs (native Windows)
  inspect       results/platform_<id>.json (hardware, thread counts, pinning)
  smoke         one-minute toolchain check, rows tagged smoke
  main          scalar study: sequential heap and sorted list, plain coarse lists x threads x pinned/unpinned, distance objective,
                heuristic sensitivity                                                       -> results/<id>/run1
  fine          fine-grained lists (threads up to the physical core count)                  -> results/<id>/run1
  optimistic    the group's optimistic list (opt_*, plain and windowed at the preset; scalar and vector thread curves)  -> results/<id>/run1, window, pareto_window
  vector        vector (Pareto) study: sequential and windowed lists at the preset, 5 reps   -> results/<id>/pareto_run1
  vector-plain  plain lists on the short vector bins (timeouts are recorded)                -> results/<id>/pareto_plain
  window-sweep  windowed-list parameter sweep (divisor x batch) at 1 and physical-core threads, scalar and vector
  window-threads thread-scaling curve of the preset settings, scalar and vector
  analyze       tables and figures                                                         -> results/<id>/analysis
The windowed-list settings of every stage come from config/presets.json (see docs/REPORT.md, decision 31).
Exit code 2 from a stage = a result differed from the golden file: the script stops and prints the stage.
"""
import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PY = sys.executable
IS_WINDOWS = os.name == "nt"
STAGES = ["setup", "tests", "inspect", "smoke", "main", "fine", "optimistic", "vector", "vector-plain", "window-sweep", "window-threads", "analyze"]


def run(cmd, what, allow_codes=(0,)):
    print("\n>>> %s\n    %s" % (what, " ".join(cmd)), flush=True)
    code = subprocess.call(cmd, cwd=ROOT)
    if code not in allow_codes:
        sys.exit("stage failed (%s): exit code %d" % (what, code))
    return code


def driver(args, what, a):
    cmd = [PY, os.path.join("tools", "run_experiments.py"), "--platform", a.platform] + args
    # Optional global knobs (speed vs report-quality tradeoff). Stages that already pass
    # --reps / --threads keep their own values unless these are set.
    if getattr(a, "reps", 0) and "--reps" not in args:
        cmd += ["--reps", str(a.reps)]
    if getattr(a, "threads", "") and "--threads" not in args:
        cmd += ["--threads", a.threads]
    if a.dry_run:
        cmd.append("--dry-run")
    print("\n>>> %s\n    %s" % (what, " ".join(cmd)), flush=True)
    code = subprocess.call(cmd, cwd=ROOT)
    if code == 2:
        sys.exit("INCORRECT RESULT in stage '%s' (a configuration differed from the golden file): stop and send the output to the group." % what)
    if code not in (0, 1):  # 1 = some configuration timed out or failed: reported, the other stages may still run
        sys.exit("stage '%s' failed with exit code %d" % (what, code))
    if code == 1:
        print("NOTE: stage '%s' reported problems (timeouts or failures); see the PROBLEMS list above and keep going." % what)


def platform_info(a):
    path = os.path.join(ROOT, "results", "platform_%s.json" % a.platform)
    if not os.path.exists(path):
        sys.exit("results/platform_%s.json is missing: run the 'inspect' stage first" % a.platform)
    return json.load(open(path))


def stage_setup(a):
    if not os.path.exists(os.path.join(ROOT, "include", "lock.h")):
        sys.exit("include/lock.h is missing: copy the Assignment-1 lock.h there (see docs/RUNBOOK.md)")
    for need in ("data/snapshot_hcmc", "config/queries.csv", "config/golden_time.csv", "config/golden_dist.csv"):
        if not os.path.exists(os.path.join(ROOT, need)):
            print("WARNING: %s is missing (pull the repository; nobody regenerates it)" % need)
    if IS_WINDOWS:
        run(["cmake", "-B", "build", "-S", "."], "configure (CMake)")
        run(["cmake", "--build", "build", "--config", "Release"], "build (CMake, Release)")
    else:
        run(["make", "all"], "build (make)")


def stage_tests(a):
    if not IS_WINDOWS:
        run(["bash", os.path.join("tests", "run_tests.sh")], "tests/run_tests.sh (must end with ALL TESTS PASSED)")
        return
    # Native Windows: the bash script cannot run; the C++ test programs and the Python data tools can.
    run([PY, os.path.join("tools", "make_profile.py"), os.path.join("config", "profile.csv")], "speed profile")
    for name, rows, seed in (("snapshot_synth_small", 40, 1), ("snapshot_synth_large", 220, 2)):
        if not os.path.isdir(os.path.join(ROOT, "data", name)):
            run([PY, os.path.join("tools", "synth_graph.py"), "--rows", str(rows), "--cols", str(rows), "--seed", str(seed), "--out", os.path.join("data", name)],
                "synthetic graph " + name)
    for exe in ("test_astar", "test_pareto"):
        path = next((p for p in (os.path.join("build", "Release", exe + ".exe"), os.path.join("build", exe + ".exe")) if os.path.exists(os.path.join(ROOT, p))), None)
        if not path:
            sys.exit("%s.exe not found: run the 'setup' stage" % exe)
        run([path, os.path.join("data", "snapshot_synth_small"), os.path.join("config", "profile.csv"), os.path.join("data", "snapshot_synth_large")],
            exe + " (native Windows; the golden-file layers of run_tests.sh are not run here)")
    print("\nNative Windows: C++ test programs passed. Layers L6 (C++ vs Python golden files) are covered by the smoke and main stages.")


def stage_inspect(a):
    run([PY, os.path.join("tools", "inspect_platform.py"), "--name", a.platform], "platform inspection")


def stage_smoke(a):
    driver(["--grid", "smoke"], "scalar smoke", a)
    driver(["--grid", "pareto-smoke"], "vector smoke", a)


def stage_main(a):
    driver(["--grid", "headline"] + (["--only", a.only] if a.only else []), "scalar main study", a)


def stage_fine(a):
    driver(["--grid", "headline", "--lists", "coarse_ttas,fine_ttas,fine_ticket,fine_std_mutex", "--threads", "phys", "--only", "fine_"], "fine-grained lists", a)


def stage_optimistic(a):
    threads = a.threads or ",".join(str(t) for t in platform_info(a)["suggested_thread_counts"])
    driver(["--grid", "headline", "--lists", "coarse_ttas,opt_ttas,opt_std_mutex", "--threads", "phys", "--only", "opt_"], "optimistic lists, plain (scalar)", a)
    driver(["--grid", "window", "--lists", "win_opt_ttas", "--window-divs", "4096", "--window-batches", "8", "--window-threads", threads,
            "--window-slacks", "off", "--window-skip-empty", "--skip-timeouts"], "optimistic list, windowed thread curve, scalar preset", a)
    driver(["--grid", "pareto-window", "--lists", "win_opt_ttas", "--window-divs", "16384", "--window-batches", "12", "--window-threads", threads,
            "--window-slacks", "64", "--window-skip-empty", "--skip-timeouts"], "optimistic list, windowed thread curve, vector preset", a)


def stage_vector(a):
    driver(["--grid", "pareto", "--lists", "win_coarse_ttas,win_fine_ttas"], "vector study (windowed lists at the preset)", a)


def stage_vector_plain(a):
    driver(["--grid", "pareto-plain", "--lists", "coarse_ttas,fine_ttas", "--reps", "3", "--skip-timeouts"], "plain lists on the short vector bins", a)


def stage_window_sweep(a):
    common = ["--lists", "win_coarse_ttas,win_fine_ttas", "--window-divs", "1024,4096,16384", "--window-batches", "4,8,16",
              "--window-skip-empty", "--skip-timeouts"]
    driver(["--grid", "window"] + common + ["--window-slacks", "off"], "window sweep, scalar", a)
    driver(["--grid", "pareto-window"] + common + ["--window-slacks", "off,64"], "window sweep, vector (slack off and 64)", a)


def stage_window_threads(a):
    info = platform_info(a)
    threads = a.threads or ",".join(str(t) for t in info["suggested_thread_counts"])
    driver(["--grid", "window", "--lists", "win_coarse_ttas,win_fine_ttas", "--window-divs", "4096", "--window-batches", "8", "--window-threads", threads,
            "--window-slacks", "off", "--window-skip-empty", "--skip-timeouts"], "thread curve, scalar preset", a)
    driver(["--grid", "pareto-window", "--lists", "win_coarse_ttas,win_fine_ttas", "--window-divs", "16384", "--window-batches", "12", "--window-threads", threads,
            "--window-slacks", "64", "--window-skip-empty", "--skip-timeouts"], "thread curve, vector preset", a)


def stage_analyze(a):
    if a.dry_run:
        print("analyze: skipped in a dry run")
        return
    run([PY, os.path.join("tools", "analyze.py"), "--platform", a.platform], "analysis (writes results/%s/analysis/)" % a.platform)
    cmd = [PY, os.path.join("tools", "make_report_figures.py"), "--platform", a.platform]
    print("\n>>> report figures and tables (writes report/generated/%s/)\n    %s" % (a.platform, " ".join(cmd)), flush=True)
    if subprocess.call(cmd, cwd=ROOT) != 0:  # needs matplotlib; the analysis above is already complete
        print("NOTE: the report figures were not written (is matplotlib installed? `pip install matplotlib`); rerun with --stages analyze.")


FUNCS = {"setup": stage_setup, "tests": stage_tests, "inspect": stage_inspect, "smoke": stage_smoke, "main": stage_main, "fine": stage_fine,
         "optimistic": stage_optimistic, "vector": stage_vector, "vector-plain": stage_vector_plain, "window-sweep": stage_window_sweep, "window-threads": stage_window_threads,
         "analyze": stage_analyze}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--platform", required=True, help="short machine id, e.g. ryzen-wsl2, m3-mac, win-native")
    ap.add_argument("--stages", default="", help="comma list (default: all, in order); --list shows them")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--only", default="", help="main stage only: run just the configurations whose name contains this text")
    ap.add_argument("--reps", type=int, default=0, help="override repetitions (default: 10 scalar / 5 vector). Use 3 for a faster Docker run.")
    ap.add_argument("--threads", default="", help="override thread list, e.g. 1,2,4,14 (skip oversubscribed logical-core counts)")
    ap.add_argument("--dry-run", action="store_true", help="measurement stages only list their configurations")
    a = ap.parse_args()
    if a.list:
        print("\n".join(STAGES))
        return 0
    chosen = [s for s in (a.stages.split(",") if a.stages else STAGES) if s]
    bad = [s for s in chosen if s not in FUNCS]
    if bad:
        sys.exit("unknown stage(s): %s (stages: %s)" % (", ".join(bad), ", ".join(STAGES)))
    for stage in sorted(chosen, key=STAGES.index):
        print("\n==================== stage: %s ====================" % stage)
        FUNCS[stage](a)
    print("\nDone: %s. Send back results/%s/ (or results/%s/analysis/ if the CSVs are too large)." % (", ".join(chosen), a.platform, a.platform))
    return 0


if __name__ == "__main__":
    sys.exit(main())
