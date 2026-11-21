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

## Insert profile (optional diagnostic, not part of the frozen schemas)
`bench` and `bench_pareto` with `--list-profile` also write `<out>.listprof.csv` (one row per measured query and repetition): `config_id,list,threads,query_id,bin,rep,list_adds,list_pops,ins_profiled,ins_steps,ins_size_sum,ins_size_max,pos0..pos7`. `ins_steps` = list nodes walked past by inserts, `ins_size_sum` = sum of the list length at each insert, `pos0..pos7` = number of inserts that landed in each eighth of the list (pos0 near the head). Only lists that call `record_insert_profile` (CoarseList) fill it; others write zeros.

## Stall diagnostics (optional, not part of the frozen schemas)
`bench` and `bench_pareto` with `--diag` also write `<out>.diag.csv` (one row per measured query and repetition): `config_id,list,threads,query_id,bin,rep,search_ns,lock_acq,lock_wait_ns,lock_wait_max_ns,idle_episodes,idle_ns,idle_max_ns,idle_final_ns,idle_samples,hoard_sum,hoard_max,held_sum,shared_sum,takes,take_items,take_short,hold_ns,hold_max_ns,hold_ge100us,hold_ge1ms,hold_ge1ms_add,hold_ge1ms_size_sum,list_size_sum,list_size_max,list_size_ge1ms_max,promotes,promoted_items,promoted_max,advances,advance_gap_sum,advance_gap_max,advance_far`. All times are summed over threads (compare with `threads * search_ns`).
- `lock_*`: time spent waiting to acquire the shared-list lock (CoarseList only; FineList writes zeros).
- `idle_*`: waits on an empty list while work was still pending (vector search only); `idle_final_ns` = the wait that ended the search.
- `idle_samples`, `hoard_sum`, `hoard_max`, `held_sum`, `shared_sum` (WindowedList only): at every 8th empty pop, `hoard` = items in OTHER threads' prefetch buffers, `held` = items in other threads' beyond-window bags, `shared` = shared-list length. Divide the sums by `idle_samples` for averages.
- `hold_*`, `list_size_*` (CoarseList only): time the lock was HELD (sum, max, number of holds >= 100 us and >= 1 ms, how many of those >= 1 ms were inserts, sum of the list length at those holds) and the shared-list length at each acquisition (sum over `lock_acq`, max, max at a >= 1 ms hold). Long holds on a SHORT list mean the holder was descheduled; long holds on a LONG list mean the insert walk itself is slow.
- `promotes`, `promoted_items`, `promoted_max` (WindowedList): bag-to-shared-list promotions, items moved, the largest single promotion.
- `advances`, `advance_gap_sum`, `advance_gap_max`, `advance_far` (WindowedList): window advances (a thread found the shared list empty and opened the window), the gap between that thread's smallest bag item and the global smallest bag item (f units), and how many advances had a gap larger than the window.
- `takes`, `take_items`, `take_short`: pop batches taken, items received, batches smaller than `--batch`.
Without `--diag` nothing is timed or published and the main CSVs are identical.

## Window slack (optional, default off)
`bench` and `bench_pareto` accept `--window-slack K` (WindowedList lists only; -1 = off, the default and the previous behaviour). A thread that finds the shared list empty opens the window around min(its own smallest bag item, global smallest bag item + K * window), where the global minimum is taken over the bag minima that every thread publishes. K = 0 follows the global minimum exactly. Added 21 Nov after the stall diagnosis (TASKS A.18): the original rule (own smallest item) lets one thread push the threshold far past the front, which gives other threads work but can flood the shared list; K = 0 keeps the list short but starves the threads (measured on the synthetic graph: 65-98% idle), so K trades the two.
The experiment driver passes it with `--window-slacks off,8,16` (window grids only; `off` keeps the old configuration names, a value K appends `_sl<K>` to the name). `tools/analyze_window.py` shows slack, the slowest repetition and a stall count per configuration.

## Window skip-empty (optional, default off; added 21 Nov)
`--window-skip-empty`: before locking the shared list to take items, a thread reads the list's atomic size and returns "no work" without locking when it is 0. Idle threads otherwise lock the list on every poll and delay the threads that have work (measured on bins 0-1: lock-wait 38% of thread time with the lock held ~7%). A stale 0 only delays a poll; termination stays with the search's pending counter. Measured effect (A.21): alone it lowers idle time and lock-wait modestly; combined with `--window-slack 16` it cuts the short-bin penalty of slack from 11x to ~2x (bin 0) and from 4x to ~1.3x (bin 1). A `--window-slack-after P` variant (apply slack only after a thread took P items) was tried and removed: it brings the short bins back to the base level but gives no stall protection on bin 4, because the window threshold only moves forward and cannot be pulled back once an early advance has pushed it far past the front.
The driver passes it with `--window-skip-empty` (window grids only; names get the suffix `_se`, e.g. `..._wb8_sl16_se_...`).
