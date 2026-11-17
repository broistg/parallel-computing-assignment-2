# Spec 1 — Objective semantics and Pareto dominance

Status: FROZEN once all three members have signed the decision log. Code: `include/astar/graph.h`, `heuristic.h`.

## Units and graph
* Snapshot = largest strongly connected component of the OSM drive network (`tools/export_graph.py`), integers only.
* Length `length_dm` (decimetres), free-flow time `free_ms = ceil(length_dm * 360 / speed_kmh)` (ms). Speed by road class: 0=60, 1=50, 2=40, 3=30 km/h.
* `length_dm >= ceil(10 * haversine(u,v)) + 1` is enforced at export and re-checked at load, so great-circle heuristics stay admissible after rounding.

## Objectives (K = 2)
| # | Objective | Edge cost | Depends on time? |
|---|-----------|-----------|------------------|
| 1 | travel time, ms | `time_cost(cls, free_ms, t)` | yes (time-dependent) |
| 2 | distance, dm | `length_dm` | no |

Time-dependent cost: `t` = departure time `t0` + time already travelled (ms since midnight, wraps at 24 h). Multiplier table `config/profile.csv` (4 classes x 96 buckets of 15 min, permille, >= 1000). Between bucket starts the multiplier is interpolated linearly in integers; `cost = ceil(free_ms * mult / 1e6)`.
**FIFO** (departing later never arrives earlier) is required for Dijkstra/A* correctness. It is guaranteed by capping the per-bucket drop of the profile (250 permille) and verified at load (slope < 1/2) and in tests (200 000 random edge/time samples).

## Scalar search (implemented)
f = g + h(v, t). Objective 1 or 2 chosen per run. Goal: minimum-cost path for the departure time `t0`.

## Vector search (specified; sequential and parallel implementations pending)
* Cost vector `c = (time_ms, dist_dm)`. Label = (node, g_time, g_dist, pred).
* **Dominance:** `a` dominates `b` iff `a_i <= b_i` for both i and `a != b`. Equal vectors are duplicates (keep one).
* A label at node v is discarded if some existing label at v dominates it, and it evicts every label it dominates. Valid because dist is static and time obeys FIFO: an earlier-arriving, shorter label can never be worse after any extension.
* Answer to a query = the set of non-dominated labels at the target (the Pareto front), each with its path.
* Open list key: lexicographic `(f_time, f_dist, node)`, where `f_i = g_i + h_i`. Pruning: a label is dropped if the vector `(f_time, f_dist)` is dominated by a label already at the target (NAMOA* rule).
* Scalarised variant (weighted sum `w*time + (1-w)*dist`, integer weights): finds only the convex part of the front; reported separately as a cheap approximation.

## Heuristics (dynamic / uncertain)
Signature `h(v, t)` -> same unit as the objective; `t` = absolute arrival time at v (ignored for distance).
* Base: floor(haversine to target) in dm; for time divide by the maximum speed in the graph (free flow, so multiplier >= 1000 keeps it a lower bound). Admissible and consistent.
* **Uncertain:** `h' = h * w/100 * (1000 + n)/1000`, `n` uniform integer in `[-rho, +rho]` permille, a deterministic hash of `(node, time bucket, seed)`. Dynamic because the noise changes with the arrival bucket.
* `rho = 0, w = 100` is "exact mode" (optimal result guaranteed); otherwise "relaxed mode" (valid path, quality measured against the golden file).

## Result quality metrics
* Scalar: `gap_ppm = (cost_checked - golden) * 1e6 / golden`. Exact mode must give 0.
* Vector: primary = hypervolume ratio HV(found)/HV(exact) with reference point (1.1 x max time, 1.1 x max dist of the exact front); secondary = fraction of exact Pareto points found, front-size ratio.
