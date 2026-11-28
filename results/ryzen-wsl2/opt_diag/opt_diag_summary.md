# Optimistic list diagnostic: ryzen-wsl2, 6 threads, scalar preset (divisor 4096, batch 8)

Sequential heap: 1272.8 ms per full query set (median of 5 repetitions).

## Speed and memory (timing runs, no instrumentation)

| allocator | list | total ms | speedup vs heap | peak MB |
|---|---|---|---|---|
| default | win_coarse_ttas | 856.1 | 1.49 | 80 |
| default | win_opt_ttas | 1315.9 | 0.97 | 123 |

## Where the optimistic list spends its work (diagnostic runs)

| allocator | list | attempts / op | retries / attempt | find steps / attempt | validate steps / attempt | retire CAS fails / pop | add ns | pop ns |
|---|---|---|---|---|---|---|---|---|
| default | win_coarse_ttas | - | - | - | - | - | 49 | 461 |
| default | win_opt_ttas | 0.27 | 0.323 | 15.2 | 15.5 | 0.000 | 80 | 488 |

## Where the optimistic list waits (diagnostic runs; the clock reads add a little time of their own)

| allocator | list | attempts near the head (%) | add/remove lock wait ns / attempt | pop_min head-lock wait ns | pop_min first-node-lock wait ns | head lock held ns / pop | coarse lock wait ns / acquisition |
|---|---|---|---|---|---|---|---|
| default | win_coarse_ttas | - | - | - | - | - | 824 |
| default | win_opt_ttas | 50.2 | 189 | 429 | 0 | 282 | - |

## Latency per operation (ns, sampled 1 in 8 operations, default allocator)

| list | threads | add ns | pop_min ns |
|---|---|---|---|
| coarse_ttas | 1 | 531 | 98 |
| coarse_ttas | 6 | 4447 | 3355 |
| fine_ttas | 1 | 1349 | 39 |
| fine_ttas | 6 | 3687 | 258 |
| opt_ttas | 1 | 3375 | 141 |
| opt_ttas | 6 | 3849 | 405 |
| win_coarse_ttas | 1 | 23 | 110 |
| win_coarse_ttas | 6 | 47 | 467 |
| win_fine_ttas | 1 | 23 | 121 |
| win_fine_ttas | 6 | 45 | 320 |
| win_opt_ttas | 1 | 23 | 163 |
| win_opt_ttas | 6 | 95 | 519 |

Reading the table: `attempts / op` counts lock-and-validate rounds of add/remove/contains (pop_min does not validate; WindowedList
also batches, so one op here is one item); `validate steps` is the walk from the head that every attempt performs, `find steps` the
lock-free walk before it.  If validate >> find the validation walk dominates; if retries are high the optimistic walk is being invalidated;
if the jemalloc rows move relative to the default rows the allocator is involved.
The wait table tests the contention explanation: a high share of attempts near the head together with long pop_min waits on the head or
first-node lock means adds and pops fight over the same few nodes.
