# Frozen interfaces (schema_ver 1)

## 1. Open-list API (Section 2 <-> Section 3)  — `include/astar/openlist.h`
```cpp
struct OpenItem { int64_t f_value; uint32_t node; uint32_t g_cost; };   // order: (f_value, node, g_cost)
bool   add(const OpenItem&);       // false if equal item present
bool   remove(const OpenItem&);
bool   contains(const OpenItem&);
bool   pop_min(OpenItem& out);     // false if empty; never blocks
size_t size_approx() const;
```
Section-2 variants implement exactly this and register with ONE line in `ASTAR_OPEN_LISTS` in `include/astar/registry.h` (picked up by `bench` and `bench_pareto`).
Lock type is a template parameter (A1 `lock.h`: `lock_ttas_pause`, `lock_ticket`, ...; and `std::mutex` as the library-lock baseline). Removed nodes may be freed immediately or at destruction.

## 2. Cost / heuristic signatures  — `search.h`, `heuristic.h`
```cpp
int64_t Problem::edge_cost(uint32_t edge, int64_t cost_so_far) const;   // scalar objective, time-dependent via departure_ms + cost_so_far
int64_t Heuristic::operator()(uint32_t node, int64_t arrival_ms) const;
// vector extension (pending): std::array<int64_t,2> edge_cost_vec(edge, g_time); h_vec(v, arrival_ms)
```
## 3. Result CSV (one row per query x repetition; header written by `bench`)
`schema_ver, config_id, platform, smoke, algo, list, lock, threads, pinned, obj, rho_permille, w_percent, query_id, bin, src, dst, dep_ms, rep, warmup, search_ns, init_ns, found, cost_reported, cost_checked, golden_cost, gap_ppm, path_ok, expansions, reopens, stale, pruned, inserts, relax_ok, cas_fail, idle_spins, list_adds, list_add_dups, list_pops, list_pop_empty, list_add_ns, list_add_samples, list_pop_ns, list_pop_samples, peak_rss_kb`
The column names are frozen even though the C++ identifiers were renamed (`stale`=stale_skips, `relax_ok`=improvements, `cas_fail`=cas_failures, `rho_permille`=noise_permille, `w_percent`=inflation_percent, `cost_reported`/`cost_checked`=reported_cost/checked_cost).
Rules: `warmup=1` rows (rep 0) and `smoke=1` rows are ignored by analysis; `pinned` is the *measured fact* (pinning requested AND succeeded); one process per configuration so `peak_rss_kb` (KB on all OSes) belongs to it; list latency columns are filled only with `--latency` (1-in-8 sampling) and such runs are never used for timing; op mix = adds/pops/pop_empty.
## 3b. Vector (Pareto) search  — `include/astar/pareto.h`, `src/bench_pareto.cpp` (pareto_schema_ver 1)
The open list is the SAME API: the vector search stores `OpenItem{key, label_id, 0}` with `key = (f_time << 31) | f_dist` (lexicographic order of the f vector; costs and estimates are capped at 2^31 - 1) and `label_id` in the `node` field, so any Section-2 list works unchanged. Add a list in ONE place: `include/astar/registry.h` (`ASTAR_OPEN_LISTS`), used by both `bench` and `bench_pareto`.
Result CSV (header written by `bench_pareto`): `pareto_schema_ver, config_id, platform, smoke, algo, list, lock, threads, pinned, rho_permille, w_percent, query_id, bin, src, dst, dep_ms, rep, warmup, search_ns, init_ns, front_size, golden_front_size, exact_points_found, hv_ratio_ppm, dominates_golden, path_ok, overflow, expansions, dead_pops, pruned, labels_created, labels_evicted, dominated_discards, pushes, idle_spins, list_adds, list_add_dups, list_pops, list_pop_empty, list_add_ns, list_add_samples, list_pop_ns, list_pop_samples, peak_rss_kb`.
`hv_ratio_ppm` = HV(found) / HV(exact) x 10^6 (reference point 1.1 x largest exact component, +1); `golden_front_size`, `exact_points_found`, `hv_ratio_ppm`, `dominates_golden` are -1 when the query has no golden front. Exact mode must give `front_size == golden_front_size == exact_points_found`, `hv_ratio_ppm == 1000000`, `dominates_golden == 0`, `path_ok == 1`, `overflow == 0` (`tests/check_pareto.py`). Golden file: `tools/oracle.py pareto` (`query_id,idx,time_ms,dist_dm` + `.meta.json` fingerprints, verified like the scalar one).

## 4. Configuration and `config_id`
`config_id` = FNV-1a over: schema version, snapshot bytes hash, profile hash, full query file, objective, rho, w, seed, reps. It excludes platform, threads, list, pinning (those are the swept axes), so the same experiment has the **same id on every machine**; analysis refuses to join files with different ids. Golden files carry a sidecar `*.meta.json` with the same kind of fingerprints and are verified before use. Canonical command lines live in `config/experiment.sh` (to be written after the freeze), committed before any measurement.
