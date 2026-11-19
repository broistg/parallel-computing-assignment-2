# Spec 3 — Reference implementations and the trust ladder

Principle: **exact ground truth is separate from the guided baseline.**
* Ground truth = `tools/oracle.py` (Python, plain heap Dijkstra / Martins). Slow on purpose, independent of the C++ code; only the data files and the integer cost formula are shared.
* Guided sequential baseline = `seq_astar` (C++, `std::priority_queue`, no atomics). This is the denominator of every speedup.
* Also reported: parallel code at 1 thread (cost of the concurrent machinery itself).

## Trust ladder (each layer tested only after the one below passes)
| L | What | Check | Where |
|---|------|-------|-------|
| 0 | Snapshot | sha256 over nodes+edges identical on every machine; counts; haversine-length rule | `snapshot_io.py verify`, loader |
| 1 | Oracle | equals exhaustive enumeration of all simple paths (time, dist, full Pareto front) on tiny graphs | `tests/test_oracle.py` |
| 2 | Data invariants | FIFO, free-flow lower bound, heuristic consistency | `test_data` |
| 3 | List | matches `std::set` sequentially; no loss/duplication under 2-8 threads | `test_list` |
| 4 | CAS-min | final value = global min under 16-thread hammer | `test_try_lower_cost` |
| 5 | Search | seq and par (T=1..16, repeated) equal an in-test Dijkstra; relaxed mode never beats optimum | `test_search`, `test_stress` |
| 6 | Cross-language | C++ cost == Python golden file on the frozen query set, for two bucket widths; a golden file from different inputs is rejected; vector search: C++ fronts == Python Martins fronts (L6d) | `tests/run_tests.sh` |
| 7 | Platform | all of the above on all three machines; TSan on x86 | `make test-tsan` |

## Golden file
`golden.csv` = `query_id,obj,cost` produced by the oracle for the frozen snapshot, profile and query set, plus a sidecar `golden.csv.meta.json` holding the FNV-1a fingerprints of those three inputs. `bench` refuses a golden file whose fingerprints do not match the data it is run on (override only with `--allow-unverified-golden`). Every later result is compared with this file, never with another implementation. Pareto: `front.csv` (`query_id,idx,time_ms,dist_dm`) from the Martins oracle on a subset (cost grows with front size). It is regenerated only if snapshot, profile or query set change, and then everything is re-run.

## Mutation checks done
Early `pending` decrement -> hang (caught by watchdog). Plain store instead of CAS-min -> caught by the dedicated hammer test (not by whole-search tests: the window is too narrow on 2 cores). Removing the stale check -> still correct (it is an optimisation), as expected.
