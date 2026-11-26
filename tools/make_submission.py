#!/usr/bin/env python3
"""Build the submission zip: report PDF + source + data needed to reproduce. Standard library only.

    python3 tools/make_submission.py --report report.pdf --out submission.zip [--with-raw] [--verify]

Included: include/ src/ tests/ tools/ (minus internal helpers), config/ (profile, queries, golden files, presets), the frozen
snapshot (data/snapshot_hcmc, compressed by the zip), CMakeLists.txt, Makefile, README.md, results/platform_*.json, the
analysis folders of every platform (tables, figures, summaries), and two generated documents:
    docs/DESIGN.md   = objectives, parallel contract, frozen interfaces   (from docs/SPEC_1, SPEC_2, INTERFACES)
    docs/TESTING.md  = trust ladder, golden files, mutation checks         (from docs/SPEC_3)
Raw per-run CSVs (results/<platform>/run*, window, pareto_*) are left out unless --with-raw is given (size).
Left out on purpose: internal group documents (TASKS, REPORT*, PARAMETERS, SUBMISSION_PLAN, RUNBOOK, EXPORT_HELP), build outputs,
synthetic snapshots, caches, pilot/diagnostic/smoke results.
--verify unpacks the zip into an empty folder and checks that it builds (make / CMake) and that a small test runs.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TOP_FILES = ["README.md", "Makefile", "CMakeLists.txt"]
TREES = ["include", "src", "tests", "tools", "config", "ex2"]
EXCLUDE_TOOLS = {"summarize.py", "check_osm_access.py", "pilot.py"}  # internal helpers; pilot.py is only a planning aid
EXCLUDE_NAMES = {"__pycache__", ".DS_Store"}
RAW_DIRS = ("run", "pareto_run", "window", "pareto_window", "pareto_plain", "smoke", "pareto_smoke", "diag")


def is_excluded_name(path):
    return any(part in EXCLUDE_NAMES for part in path.split(os.sep)) or path.endswith((".pyc", ".partial"))


def collect(with_raw):
    files = []
    for name in TOP_FILES:
        if os.path.exists(os.path.join(ROOT, name)):
            files.append(name)
    for tree in TREES:
        for dirpath, _, names in os.walk(os.path.join(ROOT, tree)):
            for n in names:
                rel = os.path.relpath(os.path.join(dirpath, n), ROOT)
                if is_excluded_name(rel) or (tree == "tools" and n in EXCLUDE_TOOLS):
                    continue
                files.append(rel)
    snap = os.path.join(ROOT, "data", "snapshot_hcmc")
    if os.path.isdir(snap):
        files += [os.path.join("data", "snapshot_hcmc", n) for n in sorted(os.listdir(snap))]
    else:
        print("WARNING: data/snapshot_hcmc is missing from this checkout; the zip will not contain the graph")
    results = os.path.join(ROOT, "results")
    if os.path.isdir(results):
        for n in sorted(os.listdir(results)):
            full = os.path.join(results, n)
            if n.startswith("platform_") and n.endswith(".json") and n != "platform_sandbox.json":
                files.append(os.path.join("results", n))
            elif os.path.isdir(full):
                for dirpath, _, names in os.walk(full):
                    for f in names:
                        rel = os.path.relpath(os.path.join(dirpath, f), ROOT)
                        parts = rel.split(os.sep)  # results/<platform>/<dir>/...
                        if is_excluded_name(rel) or n == "sandbox":
                            continue
                        top = parts[2] if len(parts) > 3 else ""
                        if top == "analysis" or (len(parts) == 3 and (f.endswith(".md") or f.endswith(".json"))):
                            files.append(rel)
                        elif with_raw and top.startswith(RAW_DIRS) and not top.startswith(("smoke", "pareto_smoke", "diag")):
                            files.append(rel)
    return sorted(set(files))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def demote(text):
    """Turn every '# ' heading of an included spec into '## ' so the generated documents have one title."""
    return "\n".join(("#" + line) if line.startswith("#") else line for line in text.splitlines())


def generated_docs():
    design = ("# Design: what is computed, the parallel contract, the frozen interfaces\n\n"
              "Generated from the project specifications. The decisions behind them and the measurements are in the report (PDF).\n\n"
              + demote(read("docs/SPEC_1_objectives.md")) + "\n\n" + demote(read("docs/SPEC_2_parallel_contract.md")) + "\n\n"
              + demote(read("docs/INTERFACES.md")) + "\n")
    testing = ("# Testing: trust ladder, golden files, mutation checks\n\n"
               "Run everything with `./tests/run_tests.sh` (POSIX), plus `make test-tsan` on Linux. Native Windows: see README.md.\n\n"
               + demote(read("docs/SPEC_3_reference.md")) + "\n")
    return {"docs/DESIGN.md": design, "docs/TESTING.md": testing}


def build_zip(out, report, with_raw):
    files = collect(with_raw)
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        if report:
            z.write(report, "report.pdf")
        else:
            print("WARNING: no --report given: the zip has no report.pdf yet")
        for rel in files:
            z.write(os.path.join(ROOT, rel), rel.replace(os.sep, "/"))
        for rel, text in generated_docs().items():
            z.writestr(rel, text)
    size = os.path.getsize(out) / 1e6
    print("%s: %d files + report, %.1f MB%s" % (out, len(files) + 2, size, " (raw CSVs included)" if with_raw else " (raw CSVs not included)"))


def verify(out):
    with tempfile.TemporaryDirectory() as tmp:
        with zipfile.ZipFile(out) as z:
            z.extractall(tmp)
        names = set(os.listdir(tmp))
        for need in ("README.md", "report.pdf", "include", "src", "tests", "tools", "config"):
            if need not in names:
                sys.exit("verify: %s is missing from the zip" % need)
        for forbidden in ("docs/TASKS.md", "docs/REPORT.md", "docs/REPORT_vi.md", "docs/SUBMISSION_PLAN.md", "docs/RUNBOOK.md"):
            if os.path.exists(os.path.join(tmp, forbidden)):
                sys.exit("verify: internal file %s is in the zip" % forbidden)
        if not os.path.exists(os.path.join(tmp, "include", "lock.h")):
            print("verify: include/lock.h is not in the zip (copy the Assignment-1 file into the checkout before building the zip)")
        if os.name != "nt":
            extra = ["LOCK_INC=" + os.environ["LOCK_INC"]] if os.environ.get("LOCK_INC") and not os.path.exists(os.path.join(tmp, "include", "lock.h")) else []
            code = subprocess.call(["make", "all"] + extra, cwd=tmp)
            if code:
                sys.exit("verify: make failed in the unpacked zip")
            if subprocess.call([sys.executable, "tools/make_profile.py", "config/profile.csv"], cwd=tmp, stdout=subprocess.DEVNULL):
                sys.exit("verify: make_profile failed")
            print("verify: unpacked zip builds with make")
        else:
            print("verify: on native Windows build the unpacked zip with CMake by hand")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--report", default="", help="path of the final report PDF (stored as report.pdf)")
    ap.add_argument("--out", default="submission.zip")
    ap.add_argument("--with-raw", action="store_true")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--list", action="store_true", help="only list the files that would go into the zip")
    a = ap.parse_args()
    if a.list:
        print("\n".join(collect(a.with_raw) + sorted(generated_docs())))
        return 0
    if a.report and not os.path.exists(a.report):
        sys.exit("report not found: " + a.report)
    build_zip(a.out, a.report or None, a.with_raw)
    if a.verify:
        verify(a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
