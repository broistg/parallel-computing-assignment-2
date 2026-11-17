#pragma once
// Sequential and parallel scalar A* (objective = travel time or distance).
// Contract: docs/SPEC_2_parallel_contract.md.  The open set is a template parameter
// (any list satisfying include/astar/openlist.h).  Duplicate insertion + stale check
// replace decrease-key; nodes are reopened whenever their cost improves (sound for inconsistent,
// dynamic or noisy heuristics).  The incumbent (best known cost to the target) is simply the
// stored cost of the target node.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "graph.h"
#include "heuristic.h"
#include "openlist.h"

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace astar {

inline void cpu_pause_hint() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

// One shortest-path query.
struct Problem {
    const Graph* graph;
    const SpeedProfile* profile;
    Objective objective;
    uint32_t source, target;
    int64_t departure_ms;  // departure time, ms since midnight
    HeuristicConfig heuristic_config;

    // Cost of traversing `edge` after `cost_so_far` has already been spent (time objective: entered at departure + cost_so_far).
    int64_t edge_cost(uint32_t edge, int64_t cost_so_far) const {
        if (objective == Objective::Distance) return int64_t(graph->edge_length_dm[edge]);
        return time_dependent_cost(*profile, graph->edge_road_class[edge], graph->edge_free_flow_ms[edge], departure_ms + cost_so_far);
    }
    // Absolute clock time at which a node is reached after `cost_so_far` (what the heuristic sees).
    int64_t arrival_ms(int64_t cost_so_far) const { return objective == Objective::Time ? departure_ms + cost_so_far : 0; }
};

struct Counters {
    uint64_t expansions = 0;   // nodes expanded
    uint64_t reopens = 0;      // expansions of a node that had been expanded before
    uint64_t stale_skips = 0;  // popped items whose cost had been improved meanwhile
    uint64_t pruned = 0;       // items/relaxations discarded because f >= best known cost to the target
    uint64_t inserts = 0;      // items inserted into the open list
    uint64_t improvements = 0; // successful relaxations (a node's cost was lowered)
    uint64_t cas_failures = 0; // lost compare-and-swap races
    uint64_t idle_spins = 0;   // polls of an empty list while work was still pending
    void operator+=(const Counters& other) {
        expansions += other.expansions; reopens += other.reopens; stale_skips += other.stale_skips; pruned += other.pruned;
        inserts += other.inserts; improvements += other.improvements; cas_failures += other.cas_failures; idle_spins += other.idle_spins;
    }
};

struct SearchResult {
    bool found = false;
    int64_t reported_cost = -1;  // stored cost of the target when the search ended
    int64_t checked_cost = -1;   // recomputed along the returned path, -1 if the path is invalid
    std::vector<uint32_t> path;
    Counters counters;
    ListOpStats list_stats;
    int64_t search_ns = 0, init_ns = 0;
};

// Each node keeps ONE 64-bit word: (best known cost so far << 32) | predecessor node.
// Keeping both in one word means a cost and its predecessor can never be torn apart.
inline uint64_t pack_cost_and_pred(uint32_t cost, uint32_t pred) { return (uint64_t(cost) << 32) | pred; }
inline uint32_t cost_of(uint64_t word) { return uint32_t(word >> 32); }
inline uint32_t pred_of(uint64_t word) { return uint32_t(word); }

// Atomic min-update: installs (new_cost, pred) iff new_cost is strictly smaller than the stored cost.
// Returns true if THIS call lowered the cost.  This is the linearization point of a relaxation.
inline bool try_lower_cost(std::atomic<uint64_t>& word, uint32_t new_cost, uint32_t pred, uint64_t& cas_failures) {
    uint64_t seen = word.load(std::memory_order_relaxed);
    while (cost_of(seen) > new_cost) {
        if (word.compare_exchange_weak(seen, pack_cost_and_pred(new_cost, pred), std::memory_order_acq_rel, std::memory_order_relaxed))
            return true;
        cas_failures++;
    }
    return false;
}

// Recompute the cost along `path` with the true cost function; -1 if it is not a source->target path.
inline int64_t recompute_path_cost(const Problem& problem, const std::vector<uint32_t>& path) {
    if (path.empty() || path.front() != problem.source || path.back() != problem.target) return -1;
    const Graph& graph = *problem.graph;
    int64_t cost = 0;
    for (size_t i = 0; i + 1 < path.size(); ++i) {
        uint32_t from = path[i], to = path[i + 1];
        int64_t cheapest = -1;
        for (uint32_t edge = graph.first_edge[from]; edge < graph.first_edge[from + 1]; ++edge)
            if (graph.edge_target[edge] == to) {
                int64_t edge_cost = problem.edge_cost(edge, cost);
                if (cheapest < 0 || edge_cost < cheapest) cheapest = edge_cost;
            }
        if (cheapest < 0) return -1;
        cost += cheapest;
    }
    return cost;
}

// Fills found / reported_cost / path / checked_cost from the final per-node words.
template <class Words>
inline void extract_result(const Problem& problem, const Words& cost_and_pred, SearchResult& result) {
    uint32_t target_cost = cost_of(cost_and_pred[problem.target]);
    if (target_cost == kInfiniteCost) return;
    result.found = true;
    result.reported_cost = target_cost;
    uint32_t node = problem.target;
    for (uint32_t steps = 0; steps <= problem.graph->num_nodes; ++steps) {  // step limit guards against a corrupted chain
        result.path.push_back(node);
        if (node == problem.source) break;
        node = pred_of(cost_and_pred[node]);
    }
    std::reverse(result.path.begin(), result.path.end());
    result.checked_cost = recompute_path_cost(problem, result.path);
}

// ---------------------------------------------------------------------------
// Sequential baseline: std::priority_queue, plain arrays (no atomics).
// ---------------------------------------------------------------------------
struct SeqWorkspace {
    std::vector<uint64_t> cost_and_pred;
    std::vector<uint8_t> expanded_before;
};

inline SearchResult seq_astar(const Problem& problem, SeqWorkspace& workspace) {
    using Clock = std::chrono::steady_clock;
    const Graph& graph = *problem.graph;
    SearchResult result;
    auto init_start = Clock::now();
    workspace.cost_and_pred.assign(graph.num_nodes, pack_cost_and_pred(kInfiniteCost, 0));
    workspace.expanded_before.assign(graph.num_nodes, 0);
    auto search_start = Clock::now();
    Heuristic heuristic(graph, problem.target, problem.objective, problem.heuristic_config);
    auto lowest_priority_first = [](const OpenItem& a, const OpenItem& b) { return b < a; };
    std::priority_queue<OpenItem, std::vector<OpenItem>, decltype(lowest_priority_first)> open_set(lowest_priority_first);
    std::vector<uint64_t>& state = workspace.cost_and_pred;
    state[problem.source] = pack_cost_and_pred(0, problem.source);
    open_set.push({heuristic(problem.source, problem.arrival_ms(0)), problem.source, 0});
    Counters& counters = result.counters;
    while (!open_set.empty()) {
        OpenItem item = open_set.top();
        open_set.pop();
        uint32_t node = item.node;
        if (cost_of(state[node]) != item.g_cost) { counters.stale_skips++; continue; }
        if (item.f_value >= int64_t(cost_of(state[problem.target]))) { counters.pruned++; continue; }
        counters.expansions++;
        if (workspace.expanded_before[node]) counters.reopens++;
        workspace.expanded_before[node] = 1;
        for (uint32_t edge = graph.first_edge[node]; edge < graph.first_edge[node + 1]; ++edge) {
            uint32_t neighbor = graph.edge_target[edge];
            int64_t new_cost = int64_t(item.g_cost) + problem.edge_cost(edge, item.g_cost);
            if (new_cost >= int64_t(kInfiniteCost) || new_cost >= int64_t(cost_of(state[neighbor]))) continue;
            state[neighbor] = pack_cost_and_pred(uint32_t(new_cost), node);
            counters.improvements++;
            int64_t f_value = new_cost + heuristic(neighbor, problem.arrival_ms(new_cost));
            if (f_value >= int64_t(cost_of(state[problem.target]))) { counters.pruned++; continue; }
            open_set.push({f_value, neighbor, uint32_t(new_cost)});
            counters.inserts++;
        }
    }
    auto search_end = Clock::now();
    result.init_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(search_start - init_start).count();
    result.search_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(search_end - search_start).count();
    extract_result(problem, state, result);
    return result;
}

// ---------------------------------------------------------------------------
// Persistent thread pool: run(job) executes job(thread_id) for thread_id = 0..num_threads-1
// (the caller is thread 0).  Threads are created once so start-up never pollutes per-query timings.
// ---------------------------------------------------------------------------
inline bool pin_current_thread(int cpu) {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#elif defined(_WIN32)
    if (cpu < 0 || cpu >= 64) return false;
    return SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpu) != 0;
#else
    (void)cpu;
    return false;  // macOS offers no hard pinning
#endif
}

class ThreadPool {
public:
    ThreadPool(int num_threads, const std::vector<int>& cpu_ids) : num_threads_(num_threads), cpu_ids_(cpu_ids) {
        if (!cpu_ids_.empty()) pinning_succeeded_ = pin_current_thread(cpu_ids_[0]);
        for (int id = 1; id < num_threads_; ++id) helpers_.emplace_back([this, id] { helper_main(id); });
    }
    ~ThreadPool() {
        stop_.store(true);
        epoch_.fetch_add(1, std::memory_order_release);
        for (auto& helper : helpers_) helper.join();
    }
    int threads() const { return num_threads_; }
    bool pinned() const { return !cpu_ids_.empty() && pinning_succeeded_; }

    void run(const std::function<void(int)>& job) {
        job_ = &job;
        finished_helpers_.store(0, std::memory_order_relaxed);
        epoch_.fetch_add(1, std::memory_order_release);  // wakes the helpers
        job(0);
        while (finished_helpers_.load(std::memory_order_acquire) != num_threads_ - 1) std::this_thread::yield();
    }

private:
    void helper_main(int thread_id) {
        if (!cpu_ids_.empty() && !pin_current_thread(cpu_ids_[thread_id % cpu_ids_.size()])) pinning_succeeded_ = false;
        uint32_t last_epoch = 0;
        for (;;) {
            unsigned spins = 0;
            while (epoch_.load(std::memory_order_acquire) == last_epoch) {
                if (++spins > 4000) std::this_thread::yield();
            }
            last_epoch = epoch_.load(std::memory_order_acquire);
            if (stop_.load()) return;
            (*job_)(thread_id);
            finished_helpers_.fetch_add(1, std::memory_order_acq_rel);
        }
    }
    int num_threads_;
    std::vector<int> cpu_ids_;
    bool pinning_succeeded_ = true;
    std::vector<std::thread> helpers_;
    std::atomic<uint32_t> epoch_{0};  // incremented to start a job
    std::atomic<int> finished_helpers_{0};
    std::atomic<bool> stop_{false};
    const std::function<void(int)>* job_ = nullptr;
};

// ---------------------------------------------------------------------------
// Parallel A*
// `pending_items` counts items inserted (or being processed) but not finished.  It is incremented
// BEFORE an insert and decremented only after the popped item's successors were inserted, so
// pending_items == 0 means the open set is empty AND no thread can still produce work.
// ---------------------------------------------------------------------------
struct alignas(128) PaddedCounters {  // one per thread, padded to avoid false sharing
    Counters counters;
};

struct ParWorkspace {
    std::unique_ptr<std::atomic<uint64_t>[]> cost_and_pred;
    std::unique_ptr<std::atomic<uint8_t>[]> expanded_before;
    uint32_t num_nodes = 0;
    void ensure_size(uint32_t nodes) {
        if (num_nodes == nodes) return;
        num_nodes = nodes;
        cost_and_pred.reset(new std::atomic<uint64_t>[nodes]);
        expanded_before.reset(new std::atomic<uint8_t>[nodes]);
    }
};

template <class List>
SearchResult par_astar(const Problem& problem, ThreadPool& pool, ParWorkspace& workspace) {
    using Clock = std::chrono::steady_clock;
    const Graph& graph = *problem.graph;
    const int num_threads = pool.threads();
    SearchResult result;
    auto init_start = Clock::now();
    workspace.ensure_size(graph.num_nodes);
    for (uint32_t node = 0; node < graph.num_nodes; ++node) {
        workspace.cost_and_pred[node].store(pack_cost_and_pred(kInfiniteCost, 0), std::memory_order_relaxed);
        workspace.expanded_before[node].store(0, std::memory_order_relaxed);
    }
    std::vector<PaddedCounters> per_thread(num_threads);
    std::mutex flush_mutex;
    InstrumentedList<List> open_list;
    std::atomic<int64_t> pending_items{0};
    Heuristic heuristic(graph, problem.target, problem.objective, problem.heuristic_config);
    auto& state = workspace.cost_and_pred;
    std::atomic<uint64_t>& target_word = state[problem.target];  // its cost is the incumbent
    state[problem.source].store(pack_cost_and_pred(0, problem.source), std::memory_order_relaxed);
    pending_items.store(1);
    open_list.add({heuristic(problem.source, problem.arrival_ms(0)), problem.source, 0});
    auto search_start = Clock::now();

    pool.run([&](int thread_id) {
        Counters& counters = per_thread[thread_id].counters;
        OpenItem item;
        unsigned idle_polls = 0;
        for (;;) {
            if (!open_list.pop_min(item)) {
                if (pending_items.load(std::memory_order_acquire) == 0) break;  // nothing left anywhere: done
                counters.idle_spins++;
                if (++idle_polls > 256) std::this_thread::yield(); else cpu_pause_hint();
                continue;
            }
            idle_polls = 0;
            uint32_t node = item.node;
            if (cost_of(state[node].load(std::memory_order_acquire)) != item.g_cost) {
                counters.stale_skips++;  // a cheaper path to this node was found after the item was inserted
            } else if (item.f_value >= int64_t(cost_of(target_word.load(std::memory_order_relaxed)))) {
                counters.pruned++;  // cannot beat the best known cost to the target
            } else {
                counters.expansions++;
                if (workspace.expanded_before[node].exchange(1, std::memory_order_relaxed)) counters.reopens++;
                for (uint32_t edge = graph.first_edge[node]; edge < graph.first_edge[node + 1]; ++edge) {
                    uint32_t neighbor = graph.edge_target[edge];
                    int64_t new_cost = int64_t(item.g_cost) + problem.edge_cost(edge, item.g_cost);
                    if (new_cost >= int64_t(kInfiniteCost)) continue;
                    if (!try_lower_cost(state[neighbor], uint32_t(new_cost), node, counters.cas_failures)) continue;
                    counters.improvements++;
                    int64_t f_value = new_cost + heuristic(neighbor, problem.arrival_ms(new_cost));
                    if (f_value >= int64_t(cost_of(target_word.load(std::memory_order_relaxed)))) { counters.pruned++; continue; }
                    pending_items.fetch_add(1, std::memory_order_acq_rel);  // count BEFORE the item becomes visible
                    if (open_list.add({f_value, neighbor, uint32_t(new_cost)})) counters.inserts++;
                    else pending_items.fetch_sub(1, std::memory_order_acq_rel);  // duplicate: nothing was inserted
                }
            }
            pending_items.fetch_sub(1, std::memory_order_acq_rel);  // only after ALL successors were inserted
        }
        open_list.flush(result.list_stats, flush_mutex);
    });

    auto search_end = Clock::now();
    for (auto& entry : per_thread) result.counters += entry.counters;
    result.init_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(search_start - init_start).count();
    result.search_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(search_end - search_start).count();
    std::vector<uint64_t> final_state(graph.num_nodes);
    for (uint32_t node = 0; node < graph.num_nodes; ++node) final_state[node] = state[node].load(std::memory_order_acquire);
    extract_result(problem, final_state, result);
    return result;
}

}  // namespace astar
