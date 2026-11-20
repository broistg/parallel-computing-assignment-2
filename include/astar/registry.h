#pragma once
// ONE place that lists the open-list implementations available to the benchmarks.
// To add a Section-2 list, add ONE line to ASTAR_OPEN_LISTS:  X("<name used on the command line>", "<lock name for the CSV>", <List type>)
// Both bench (scalar A*) and bench_pareto (vector A*) pick it up; the name is also what tools/run_experiments.py --lists expects.
#include <mutex>

#include "fine_list.h"
#include "lock.h"  // Assignment 1 lock family
#include "openlist.h"
#include "windowed_list.h"

#define ASTAR_OPEN_LISTS(X)                                              \
    X("coarse_ttas", "ttas_pause", CoarseList<lock_ttas_pause>)          \
    X("coarse_ticket", "ticket", CoarseList<lock_ticket>)                \
    X("coarse_std_mutex", "std_mutex", CoarseList<std::mutex>) /* library-lock baseline */ \
    X("fine_ttas", "ttas_pause", FineList<lock_ttas_pause>)              /* fine-grained: one lock per node, hand-over-hand */ \
    X("fine_ticket", "ticket", FineList<lock_ticket>)                    \
    X("fine_std_mutex", "std_mutex", FineList<std::mutex>)               \
    X("win_coarse_ttas", "ttas_pause", WindowedList<CoarseList<lock_ttas_pause>>) /* windowed: short shared list + per-thread bags */ \
    X("win_fine_ttas", "ttas_pause", WindowedList<FineList<lock_ttas_pause>>) /* windowed on top of the fine-grained list */ \
    X("win_fine_ticket", "ticket", WindowedList<FineList<lock_ticket>>)       \
    X("win_fine_std_mutex", "std_mutex", WindowedList<FineList<std::mutex>>)  \
    X("win_coarse_ticket", "ticket", WindowedList<CoarseList<lock_ticket>>)       \
    X("win_coarse_std_mutex", "std_mutex", WindowedList<CoarseList<std::mutex>>)
