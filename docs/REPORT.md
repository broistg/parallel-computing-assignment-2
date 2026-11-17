# Assignment 2 (Concurrent A*) — Progress report: what is implemented, what was decided and why

Date: 17 Nov 2026 · Deadline: 5 Dec 2026 · Repository: `parallel-computing-assignment-2`
Audience: the three group members (Đạt, Bình, Đăng). A Vietnamese copy is in `REPORT_vi.md`.

## 1. Summary

The foundation for Sections 1 and 3 is done and tested; Section 2 (other concurrent lists) is being done by others and plugs into a frozen interface. **Nothing has been measured on the real HCMC graph or on the group's machines yet.** The sandbox used for development could not reach OpenStreetMap, so everything was built and tested on a synthetic road-like graph in the exact snapshot format.

| Area | State |
|------|-------|
| Graph freeze tooling (OSM → largest SCC → integer CSR files + hash) | written; conversion logic tested on a hand-built OSMnx-style graph; **real export not yet run** |
| Time-dependent cost + speed profile (synthetic rush-hour curve) | done, bucket width switchable, tested at 15 min and 5 min |
| Heuristics: exact, dynamic/uncertain (noise, inflation) | done |
| Sequential A* (priority queue) | done |
| Parallel A* with the open list as a template parameter | done (scalar objective: time or distance) |
| Open list: coarse-locked sorted list with A1 locks and `std::mutex` | done; fine-grained/lazy from Section 2 plug in with one line |
| Exact oracle (Python): time-dependent Dijkstra, Martins Pareto | done, checked against brute force |
| Golden files tied to their inputs by fingerprints | done |
| Vector (Pareto) A*, sequential and parallel in C++ | **specified only** |
| Notebook's 9 bugs as regression tests | **not started** (notebook not yet re-read) |
| Experiment driver, analysis scripts, ALT decision, report text | not started |

## 2. What is implemented

* `tools/export_graph.py`, `snapshot_io.py`: build a snapshot (`nodes.csv`, `edges.csv`, `meta.json` with sha256). Only code that writes snapshots. `synth_graph.py` makes a synthetic snapshot for tests only.
* `include/astar/graph.h`: loader into CSR, speed profile, time-dependent edge cost, FIFO guard, content hashes.
* `heuristic.h`: great-circle heuristic divided by top speed; optional noise and inflation.
* `openlist.h`: frozen list interface, `CoarseList<Lock>`, instrumentation wrapper (operation mix always, sampled latency on request).
* `search.h`: sequential A*, thread pool, parallel A*, path checker.
* `src/bench_main.cpp`: runs one configuration, writes the 44-column result CSV.
* `tools/oracle.py`, `make_queries.py`, `make_profile.py`, `inspect_platform.py`, `pilot.py`.
* `tests/`: `test_astar.cpp`, `test_oracle.py`, `check_gaps.py`, `run_tests.sh`.

## 3. Decisions and the reasons for them

| # | Decision | Reason | Alternatives / trade-offs | Reversible? |
|---|----------|--------|---------------------------|-------------|
| 1 | Freeze ONE snapshot of the HCMC drive network, committed with a hash; one person exports, everybody verifies | OSM changes daily; a re-export would silently change every result and the golden files | each member exports (different graphs); rely on OSMnx cache (not portable) | No, once golden files exist |
| 2 | osmnx/NetworkX only as exporter; C++ reads plain CSV into CSR | keeps the timed code free of Python and library behaviour; CSR is cache-friendly | read GraphML in C++ (heavy) | Yes |
| 3 | Keep only the largest strongly connected component | every query has an answer; no "unreachable" cases polluting timings | keep all and handle failures | Yes, before the freeze |
| 4 | Integer units: length in dm, time in ms, multipliers in permille; rounding up | identical results on every platform, exact golden comparison, no float tolerance | doubles (platform-dependent last bits) | Yes |
| 5 | Edge length ≥ great-circle distance + 1 dm (enforced at export, checked at load) | keeps the straight-line heuristic admissible and consistent despite rounding | trust OSM lengths (some are shorter than the straight line) | No |
| 6 | Time-dependent cost from a speed profile: 4 road classes × 96 buckets of 15 min, linear interpolation | the spec asks for dynamic costs; interpolation avoids jumps. **The curve is invented** (no real traffic data); 15 min is a judgement call, switchable to 5 min by regenerating the file | finer buckets (adds resolution to invented data), step function (breaks FIFO more easily) | Yes (data-driven, tested at 96 and 288) |
| 7 | FIFO guard (slope < ½) verified at load and in tests | Dijkstra/A* are only exact if later departure never arrives earlier | allow non-FIFO (needs a different algorithm) | No |
| 8 | Objectives: travel time (primary), distance (secondary), vector (time, distance) with Pareto dominance | matches the spec's vector f; time is the time-dependent one | weighted sum only (finds only the convex part of the front; kept as a cheap approximation) | Yes |
| 9 | Heuristic = great-circle distance / top speed (free flow). "Uncertain" mode: deterministic noise ±ρ per (node, 15-min bucket, seed) and inflation w | exact mode is admissible and consistent (guarantees optimal results); noise/inflation model dynamic and uncertain estimates reproducibly (hash, not random numbers) | random noise (not reproducible across platforms) | Yes |
| 10 | Reopen nodes when their cost improves (no closed set) | closed sets are unsound with out-of-order parallel expansion and with inconsistent heuristics | closed set + consistency assumption (breaks in relaxed mode) | No |
| 11 | Parallel A*: duplicate inserts + stale check instead of decrease-key | a sorted-list set has no decrease-key; stale items are cheap to detect | delete-then-insert (two list operations, races) | Yes |
| 12 | One 64-bit atomic word per node = (cost, predecessor), updated by CAS-min | cost and predecessor can never disagree; lock-free | per-node locks (more contention) | Yes |
| 13 | Incumbent = stored cost of the target; prune items with f ≥ incumbent | no extra shared variable; safe when h is admissible | separate atomic bound | Yes |
| 14 | Termination: `pending_items` counter, incremented before insert, decremented after the parent's inserts | emptiness of the list alone is not enough (a thread may be about to insert); mutation-tested | barrier-based rounds (slower), "all threads idle" flags (racy) | No |
| 15 | Open list is a template parameter with a frozen API (add, remove, contains, pop_min, size_approx) | Section 2 variants plug in without touching A*; fair comparison | virtual interface (adds call overhead to every operation) | API frozen |
| 16 | Reuse A1 locks; add `std::mutex` as library baseline | continuity with A1, trusted cross-platform header, answers "which lock suits a list"; `std::mutex` shows whether spinning pays off when threads outnumber cores. Not yet compiled against the real `lock.h` | library locks only (loses the A1 comparison) | Yes |
| 17 | Exact ground truth is separate from the guided baseline: Python oracle (independent) vs C++ priority-queue A* | a bug in shared code cannot hide itself; speedups are measured against the best sequential code, not the oracle | compare implementations with each other | No |
| 18 | One golden file for all platforms, with a sidecar of input fingerprints, verified by `bench` | costs are integers, so results are platform-independent; the fingerprints stop a stale file being used | per-platform golden files (could hide a platform bug) | No |
| 19 | Query set committed, not regenerated; 5 distance bins, 50 pairs/bin, 4 fixed departures (03:00, 08:00, 12:30, 17:30) rotated within each bin | Python's seeded generator is not guaranteed identical across versions; departure time changes difficulty, so each bin gets the same mix and results can be split per departure | seeded random departures (also reproducible; planned as a sensitivity run) | Yes |
| 20 | Protocol: 10 repetitions + 1 warm-up, median and IQR; one process per configuration; thread pool created once; workspace reset excluded from timing and reported as `init_ns`; pinned and unpinned runs recorded separately (`pinned` = measured fact); smoke/pilot rows tagged and ignored; ≥ 20 ms of work per measured (configuration, bin) | removes thread start-up and allocation noise; peak memory belongs to its configuration; macOS cannot pin | single process for all configurations (shared peak memory) | Yes |
| 21 | Result schema column names frozen (C++ identifiers were later renamed for clarity) | later analysis scripts depend on them | none | Frozen |
| 22 | Platforms: two Windows machines (WSL2 assumed, as in A1) and one Apple M3; every machine runs `inspect_platform.py` first | exact hardware must be reported; WSL2 is a VM (vCPU preemption) and M3 has performance/efficiency cores | native Windows build (CMake file exists, untested) | Yes |
| 23 | Optimizations: incumbent pruning, CSR layout, largest SCC (done); ALT landmarks (decide 22 Nov, optional per spec); contraction hierarchies skipped (explain in report) | best benefit per effort in the time available | CH (large effort, time-dependent CH is hard) | Open |
| 24 | Testing strategy: layered trust ladder, mutation checks of the tests themselves, TSan; heavy tests are offloaded to the group machines | a test that cannot fail is worthless; the sandbox has 2 cores | run everything in the sandbox | Yes |

## 4. Evidence so far (synthetic graph, 2-core sandbox)

* Oracle vs brute-force path enumeration (time, distance, full Pareto front): 36 queries, 0 mismatches.
* C++ (sequential, parallel with 1 to 16 threads, 3 list types) vs an independent Dijkstra and vs the Python golden file: about 850,000 checks, 0 failures, at 96 and 288 time buckets; a golden file from different inputs is rejected.
* Mutation checks of the test suite: early decrement of the pending counter → hang, caught by the watchdog; plain store instead of CAS-min → caught by the dedicated hammer test (5/5 runs on an idle machine, 0/3 while another heavy process shared the two cores — detection is timing-sensitive, so TSan and the Apple M3 run matter); removing the stale check → still correct (it is only an optimisation).
* ThreadSanitizer (g++, x86-64, 2 cores): 850,900 checks, 0 failures, no race reports, including the 48k-node stress graph (about 7 min). An earlier TSan run that took more than 10 minutes was a bug in my test (threads busy-spinning on a start flag without yielding starve each other when threads outnumber cores); fixed, and the per-stage timing printed by the test makes a slow stage visible immediately.
* Pilot (48k-node synthetic graph, 2 cores, coarse lock): sequential queries take 15 µs to 0.9 ms; parallel A* took about 2.3× the sequential time at 1 thread and about 12× at 2 threads. This is a sandbox observation, not a result: the coarse lock is the suspected bottleneck and the machine has only two cores. It does show that parallel speedup is not automatic and that short queries mostly measure the timer, so very short bins should be dropped or merged.

## 5. Risks and open items

* No real data yet: HCMC export, queries, golden files, pilot on real hardware (see `TASKS.md`, part A).
* The speed profile is synthetic; the report must say so.
* Windows-native and macOS-specific code (affinity, memory measurement, inspection script) is untested.
* The code has been compiled only against a stand-in for `lock.h`.
* The Pareto search and the notebook-bug regression tests are missing.
* A real HCMC graph may contain very long edges that trip the FIFO guard (message: "FIFO guard"); the fix is small.

## 6. How to reproduce

`./tests/run_tests.sh` (all layers), `make test-tsan` (Linux, slow), `python3 tools/inspect_platform.py --name <id>`, then the freeze sequence in `docs/TASKS.md` part A.
