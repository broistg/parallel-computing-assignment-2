# ALT (landmark) heuristic — plan (28 Nov)

Status: **planned, not started.** Group decision of 21 Nov: ALT starts only after a clean submission candidate exists (tagged commit, tests green, PDF builds). Spec section 4 is optional.

## What ALT gives and why it fits this code
Pick K landmarks L. Precompute exact shortest distances to and from every landmark on the *static* graph. For a node v and target t:
`h_alt(v,t) = max over L of max( d(L,t) - d(L,v), d(v,L) - d(t,L) )`, then `h = max(h_alt, h_great_circle)`.
* Distance objective: `d` = path length (dm), static, so h is admissible and consistent.
* Time objective: `d` = shortest **free-flow** time (ms). The profile multiplier is >= 1000 permille, so free-flow time is a lower bound of the time-dependent cost. h stays admissible and consistent (same argument as the current max-speed bound). The time dependence is NOT used by the bound; that is the price of a cheap, exact precomputation. Say this in the report.
* Noise / inflation (`HeuristicConfig`) are applied on top of `h`, unchanged. Exact mode stays optimal, so every golden file still applies.
* Vector search: `h_time` and `h_dist` each get their own table; `pareto.h` already builds one `Heuristic` per objective.

## Design
1. `include/astar/landmarks.h` (new): `struct Landmarks { K; vector<uint32_t> id; vector<int32_t> to_l[K], from_l[K]; for both objectives }`. Built from the CSR graph by K x 2 x 2 Dijkstras (forward and on the reversed graph; the reverse CSR is built once).
2. Selection: farthest-point heuristic (deterministic: start from the node nearest the centroid, then repeatedly take the node farthest from the chosen set by free-flow time); K = 8 and 16 compared; integers only, so the result is identical on all platforms.
3. Memory: K=16, 2 objectives, 2 directions, int32 = 195,823 x 64 x 4 B = about 50 MB. Report it next to the memory table (platform with 7.4 GB is fine; K=8 halves it).
4. Cost: 64 Dijkstras of about 50-100 ms each = a few seconds once per process, not per query. Not part of the timed search. If too slow, cache to `<snapshot>/landmarks_K.bin` with the graph hash in the header.
5. `Heuristic` gets an optional `const Landmarks*`; `operator()` takes the max when present. The call site cost (2K array reads) must be measured: per-node work is tiny, so it shifts the sequential/parallel balance. Selected by `--heuristic base|alt` and `--landmarks K`; default stays `base`, so the frozen presets and all submitted numbers are untouched.
6. Search driver unchanged otherwise.

## Tests
* Admissibility and consistency: random (v,w) edge samples, `h(v) <= c(v,w) + h(w)` on both objectives, and `h(v) <= golden` for sampled pairs.
* Exact mode: all golden queries (time and distance, bins 0-4) give gap 0 with ALT, sequential and parallel; vector front equals the golden front (HV ratio 1.000).
* Cross-platform determinism of landmark ids and tables (hash printed in the run header).
* Mutation: break the reverse graph (use forward distances twice) and confirm the consistency test fails.

## Experiments (one machine, after the final runs; NOT part of the three-machine rerun)
* Expansions and sequential time, base vs ALT, K in {8,16}, per bin (bin 0 shorter queries gain less).
* Parallel: `win_coarse_ttas` and `win_fine_ttas` at 1/2/6 threads with ALT. Expectation to test: fewer expansions mean less parallel work per query, so speedup over the sequential heap may drop even when absolute time improves; report both against the sequential-with-ALT baseline and the old baseline.
* Interaction with noise / inflation (ALT is the sharper bound, so inflation 120% should now prune less).

## Order and cut-off
1. Clean submission candidate exists (prerequisite). 2. Landmarks + heuristic + tests (about one day). 3. Experiments on machine 1 (about half a day of run time, offloaded to the user). 4. Report paragraph in section 2.4 and one table. **Cut-off: if the tests are not green two days before the deadline, drop it** and replace the open note in section 2.4 by a paragraph saying ALT and CH were not done (CH needs time-dependent edge weights, which is the hard part).
Because ALT is additive and off by default, dropping it costs nothing.

## Open questions for the group
* K and whether to cache tables on disk (needs a decision about a file in the snapshot folder; the zip stays unaffected if the cache is git-ignored).
* Whether to report ALT as part of S3 (scalability study) or only S4 (the spec treats it as an optional optimization).
