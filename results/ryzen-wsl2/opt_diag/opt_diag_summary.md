# Optimistic list diagnostic: ryzen-wsl2, 6 threads, scalar preset (divisor 4096, batch 8)

Sequential heap: 1306.9 ms per full query set (median of 5 repetitions).

## Speed and memory (timing runs, no instrumentation)

| allocator | list | total ms | speedup vs heap | peak MB |
|---|---|---|---|---|
| default | win_coarse_ttas | 978.9 | 1.34 | 80 |
| default | win_opt_ttas | 4048.0 | 0.32 | 125 |
| jemalloc | win_coarse_ttas | 1034.2 | 1.26 | 74 |
| jemalloc | win_opt_ttas | 3700.1 | 0.35 | 140 |

## Where the optimistic list spends its work (diagnostic runs)

| allocator | list | attempts / op | retries / attempt | find steps / attempt | validate steps / attempt | retire CAS fails / pop | add ns | pop ns |
|---|---|---|---|---|---|---|---|---|
| default | win_coarse_ttas | - | - | - | - | - | 53 | 538 |
| default | win_opt_ttas | 0.66 | 0.247 | 9.8 | 10.9 | 0.000 | 51 | 1829 |
| jemalloc | win_coarse_ttas | - | - | - | - | - | 53 | 523 |
| jemalloc | win_opt_ttas | 0.63 | 0.211 | 9.9 | 10.9 | 0.000 | 56 | 1788 |

## Latency per operation (ns, sampled 1 in 8 operations, default allocator)

| list | threads | add ns | pop_min ns |
|---|---|---|---|
| coarse_ttas | 1 | 543 | 99 |
| coarse_ttas | 6 | 4911 | 3642 |
| fine_ttas | 1 | 1368 | 38 |
| fine_ttas | 6 | 4428 | 300 |
| opt_ttas | 1 | 3509 | 53 |
| opt_ttas | 6 | 4396 | 347 |
| win_coarse_ttas | 1 | 23 | 111 |
| win_coarse_ttas | 6 | 56 | 525 |
| win_fine_ttas | 1 | 23 | 123 |
| win_fine_ttas | 6 | 66 | 382 |
| win_opt_ttas | 1 | 24 | 209 |
| win_opt_ttas | 6 | 53 | 1867 |

Reading the table: `attempts / op` counts lock-and-validate rounds of add/remove/contains (pop_min does not validate; WindowedList
also batches, so one op here is one item); `validate steps` is the walk from the head that every attempt performs, `find steps` the
lock-free walk before it.  If validate >> find the validation walk dominates; if retries are high the optimistic walk is being invalidated;
if the jemalloc rows move relative to the default rows the allocator is involved.
