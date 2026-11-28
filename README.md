# Concurrent A* on a road network (Assignment 2)

Sequential and parallel A* on the Ho Chi Minh City road network (OpenStreetMap, frozen snapshot) with time-dependent edge
costs, dynamic / uncertain heuristics and a vector (Pareto) objective; the open list is a sorted list with pluggable
synchronisation (coarse lock, fine-grained hand-over-hand locks, windowed variant). The report (`report.pdf`) explains the
design, the experiments and the results; this file explains how to build, test and reproduce them.

## Layout
```
include/astar/   graph.h heuristic.h openlist.h search.h pareto.h fine_list.h optimistic_list.h windowed_list.h registry.h   (C++17, header-only core)
include/lock.h   the Assignment-1 lock family (ttas / ticket); registry.h maps list names to list types
ex2/             Section 2 standalone lists (fine-grained, optimistic; Strategy pattern) with its own test program and README
src/             bench_main.cpp (scalar A*), bench_pareto.cpp (vector A*): one configuration per process -> CSV
tests/           run_tests.sh and the C++ / Python tests   (see docs/TESTING.md)
tools/           experiment.py (one command per machine), run_experiments.py (grids), analyze.py, opt_diag.py (optimistic-list diagnostic),
                 oracle.py (independent exact reference), export_graph.py / snapshot_io.py (graph freeze), make_*.py, inspect_platform.py
config/          profile.csv (speed profile), queries*.csv, golden_*.csv (+ .meta.json fingerprints), presets.json (window settings)
data/snapshot_hcmc/   the frozen graph (nodes.csv, edges.csv, meta.json with sha256)
results/         platform_<id>.json (hardware), <id>/analysis/ (tables and figures); raw CSVs only if included
docs/            DESIGN.md (objectives, parallel contract, interfaces), TESTING.md
```

## Build
Needs a C++17 compiler (g++ / clang++ / MSVC), Python 3, and `include/lock.h` (Assignment-1 file).
* Linux, WSL2, macOS: `make` (binaries in `build/`).
* Native Windows: `cmake -B build -S . && cmake --build build --config Release`.
* Docker (Windows/macOS/Linux): see `docs/DOCKER.md` — `docker build -t concurrent-astar .` then run `tools/experiment.py` inside the container.

## Test
`./tests/run_tests.sh` must end with `ALL TESTS PASSED` (data tools, oracle vs brute force, list and search tests, C++ against the
Python golden files, vector search). Linux only, slow: `make test-tsan` (ThreadSanitizer). Native Windows: the C++ test programs
are run by `python3 tools/experiment.py --platform <id> --stages tests`.

## Reproduce the measurements on one machine
```
python3 tools/experiment.py --platform <short-id> --stages setup,tests,inspect,smoke      # build, tests, hardware record, 1-minute check
python3 tools/experiment.py --platform <short-id> --dry-run                               # list every configuration
python3 tools/experiment.py --platform <short-id>                                         # all measurement stages (hours; resumable)
```
Stages and output folders are listed by `--list` and in the header of `tools/experiment.py`. Every exact configuration is compared
with the golden file row by row; a mismatch stops the run (exit code 2). Windowed-list settings come from `config/presets.json`.
The snapshot, queries and golden files are frozen: do not regenerate them (`tools/export_graph.py`, `make_queries.py` and
`oracle.py` are included only to show how they were made).

## What to know before trusting a number
* The speed profile (rush-hour curve) is synthetic; no real traffic data was available.
* Windowed lists are a variant of the specified sorted list (a short shared sorted list plus per-thread bags); the report labels them.
* Window parameters were tuned on the queries they are scored on; the report gives the common preset and, where measured, the per-platform best.
* Thread counts above the physical core count are marked oversubscribed; FIFO (ticket) locks are not expected to scale there.
