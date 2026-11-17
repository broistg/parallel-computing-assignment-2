# Spec 2 — Parallel A* termination and correctness contract

Code: `include/astar/search.h` (`par_astar<List>`), tests in `tests/test_astar.cpp`.

## Shared state
* `cost_and_pred[v]` : one 64-bit atomic word per node = `(best known cost << 32) | predecessor`. Updated only through `try_lower_cost` (CAS loop, installs iff the new cost is strictly smaller). Cost and predecessor therefore never tear.
* The stored cost of the target node is the **incumbent**: no separate variable.
* Open set: sorted list (any variant satisfying `openlist.h`) of items `(f_value, node, g_cost)`. No decrease-key: a better path inserts a new item; the old one becomes stale.
* `pending_items` (atomic counter): items inserted but not yet fully processed.

## Worker loop
```
loop:
  if pop_min(item) fails:  if pending_items == 0: exit   else: back off, retry
  if cost(item.node) != item.g_cost:        stale   -> skip
  elif item.f_value >= cost(target):        pruned  -> skip
  else expand: for each edge (node, neighbor): new_cost = item.g_cost + edge_cost(edge, item.g_cost)
           if try_lower_cost(neighbor, new_cost, node):
               f_value = new_cost + h(neighbor, departure + new_cost)
               if f_value < cost(target):  pending_items++ ; if !list.add(...) pending_items--
  pending_items--     (after ALL successor inserts)
```
## Termination rule
`pending_items` is incremented BEFORE an insert and decremented only AFTER the parent's inserts, so `pending_items == 0` implies the list is empty and no thread holds unfinished work. Decrementing early lets a thread see `pending_items == 0` while a successor is about to appear (mutation-tested: hangs/terminates wrongly). Idle threads poll; they never block.

## Correctness claims
1. **Exact mode** (h admissible; consistency not needed): the returned cost equals the optimum for the departure time, for every interleaving. Argument: every node on an optimal path is expanded with its optimal g unless its f >= incumbent, and f is a lower bound, so such an item cannot improve the answer; a node whose g improves is re-inserted (reopening), so out-of-order expansion only costs time.
2. **Relaxed mode** (noisy/inflated h): the result is a valid path with `cost_checked >= optimum`; quality is measured, not claimed.
3. **Path check** (every run): walk the predecessors back from the target, recompute the true cost along the path with the real cost function; `path_ok = valid source->target path and checked_cost <= reported_cost`. In exact mode `checked_cost == reported_cost == golden`.
4. No result is read before the pool's `run()` has joined all workers (acquire/release on `epoch`/`done`).

## Memory
List nodes are allocated per item and freed by the list; a variant may defer freeing to its destructor. No hazard pointers / epochs required. Workspace (`cost_and_pred`, `expanded_before`) is reset outside the timed region and its time reported as `init_ns`.

## What the tests prove (and what they do not)
Randomised comparison with an independent Dijkstra for T = 1..16 threads, repeated runs, plus a hammer test of `try_lower_cost` and a ThreadSanitizer build. Testing cannot prove absence of races on weak memory models; the group must also run the suite on the Apple M3 (ARM), where reordering bugs surface that x86 hides.
