# parallel-computing-assignment-2 — Concurrent A* repository guide

```
include/astar/  graph.h  heuristic.h  openlist.h  search.h      (C++17, header-only core)
src/bench_main.cpp      one scalar configuration -> CSV rows
src/bench_pareto.cpp    one vector (Pareto) configuration -> CSV rows (include/astar/pareto.h)
tests/                  test_astar.cpp  test_oracle.py  test_export.py  check_gaps.py  run_tests.sh
tools/                  export_graph.py  check_osm_access.py  snapshot_io.py  synth_graph.py  make_profile.py
                        make_queries.py  oracle.py  inspect_platform.py  pilot.py  summarize.py
config/profile.csv      speed multipliers        data/  snapshots (synthetic = DEV ONLY)
docs/                   SPEC_1..3, INTERFACES, PARAMETERS, TASKS, REPORT, REPORT_vi, EXPORT_HELP
```
Prerequisite: copy the Assignment-1 `lock.h` to `include/lock.h`.
Build: `make` (Linux/WSL2/macOS) or CMake (Windows native). Test: `./tests/run_tests.sh` (add `make test-tsan` on Linux).
Freeze order: (1) one person runs `tools/export_graph.py` and commits `data/snapshot_hcmc`; others verify the hash; (2) `make_queries.py` + `oracle.py golden` -> commit `config/queries.csv`, `config/golden.csv`; (3) `inspect_platform.py` on each machine; (4) `pilot.py`; (5) fix parameters; (6) real runs.
Synthetic snapshots exist only for tests and the pilot plumbing; never report numbers from them.
