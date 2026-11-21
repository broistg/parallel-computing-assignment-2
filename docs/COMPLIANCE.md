# Compliance with Assignment 2 (v1, 23 Oct 2026) — internal checklist

Status: done = implemented and tested; measured = results exist on the Ryzen/WSL2 machine only; open = not done.
Where a row says "report", the text still has to be written into the PDF. Updated 21 Nov 2026.

| Spec bullet | Where it is covered | Status |
|---|---|---|
| **S1** Sequential A* on a real road network (e.g. HCMC), two arbitrary locations | `include/astar/search.h` (`seq_astar`), snapshot `data/snapshot_hcmc` (195,823 nodes), queries `config/queries.csv` | done, measured |
| **S1** h dynamic and uncertain | `heuristic.h` (noise per node and 15-min bucket, inflation), time-dependent edge cost from `config/profile.csv` (synthetic curve: say so) | done; sensitivity grid in `headline` (noise x inflation) |
| **S1** f a vector function; re-implement the sequential A* | `include/astar/pareto.h` (NAMOA*-style label sets), `bench_pareto` | done; golden fronts for bins 0-3 (bin 4 exceeds the oracle's label budget: stated limit) |
| **S1** Approach to testing correctness | `docs/TESTING.md` (trust ladder: oracle vs brute force, C++ vs Dijkstra, vector front vs reference, golden files with fingerprints, mutation checks, TSan) | done; report section |
| **S2** Implement any two list variants (own or built-in locks) | `CoarseList` (coarse), `FineList` (hand-over-hand) in `openlist.h` / `fine_list.h`; locks from A1 `lock.h` + `std::mutex`. Group's own Section-2 lists plug in through `registry.h` | two variants done (FineList written by Đạt; group lists merged later) |
| **S2** Approach to testing the lists | list tests against a set model, concurrent stress, TSan (`tests/test_astar.cpp`) | done for ours; group lists need their stages instantiated |
| **S2** Evaluate and compare list performance in a usage scenario of your choice | usage scenario = A* itself (list operation mix and latency per list, `--latency`, `analysis/listops.csv`); a stand-alone list micro-benchmark (random add/remove/contains mix) is NOT written | **open: decide whether the A* scenario alone is enough or add `tests/bench_list.cpp`** |
| **S2** Exponential backoff (mentioned as usable) | A1 lock family (TTAS with pause); a backoff list/lock variant is not measured | open (optional) |
| **S3** Adapt sequential A* and sorted list into parallel A* | duplicate inserts + stale check, CAS-min per-node cost word, `pending_items` termination (`search.h`, `docs/SPEC_2`) | done |
| **S3** Approach to testing parallel A* | same ladder, 1-16 threads, 3 locks, mutation checks, TSan | done |
| **S3** Performance and scalability study of ALL sequential and parallel versions | `tools/experiment.py` stages: heap, sorted list without lock, coarse / fine lists x locks x threads x pinned, distance objective, heuristic sensitivity, vector study, windowed lists | measured on one machine; other two machines open |
| **S3** Quality of outcomes AND execution time as metrics | gap to the golden cost (ppm), share not optimal, expansions; vector: hypervolume ratio, front size, exact points found; time and speedup tables | done (`analysis/quality.csv`, `pareto_quality.csv`) |
| **S3** All shared-memory platforms you possess, at least as many as members (3) | Ryzen 5 6600H / WSL2 done; Windows native and Apple M3 open | **open, required** |
| **S4** Other optimizations (optional) | incumbent pruning, CSR, largest SCC (done); windowed list + batching (done, labelled a variant); ALT landmarks: after a clean submission (group decision 21 Nov); contraction hierarchies: skipped, explain in report | partly done |
| Deliverable: ONE zip = final PDF report + all Python/C++ source, to the website by 5 Dec 2026 | `tools/make_submission.py --report report.pdf --verify` | script ready, report open |
| Report language: English | PDF written in English (`docs/REPORT.md` is the working draft) | open |
| Group of 2-3 | 3 members | - |

## Things the report must say (honesty list)
* Speed profile and its rush-hour curve are invented; 15-min buckets are a judgement call.
* Windowed lists change the specified sorted list (short shared list + per-thread bags); parameters were tuned on the scored queries; report the common preset and the per-platform best.
* Plain lists on the long vector queries are too slow to measure fully; the vector oracle covers bins 0-3 only.
* Windowed ticket / std::mutex variants were implemented but dropped unmeasured; Windows-native and macOS paths are tested only where the table says so.
* Thread counts above the physical cores are oversubscribed; FIFO locks collapse there (convoy).
