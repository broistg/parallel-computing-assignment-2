#pragma once
// Vector (Pareto) A*: the cost of a path is the vector (travel time in ms, distance in dm); the answer to a query is the
// set of non-dominated source->target paths (the Pareto front).  Specification: docs/SPEC_1_objectives.md ("Vector search").
//
//   * A LABEL is (node, g_time, g_dist, parent label).  Labels live in an append-only arena and are never moved.
//   * Every node keeps the set of its non-dominated labels.  A new label that is weakly dominated by a label of the set
//     (<= in both components) is discarded; otherwise it evicts every label it dominates.  This is valid because the
//     distance is static and the time obeys FIFO (an earlier-arriving, shorter label is never worse after any extension).
//   * The open list orders labels by the packed key (f_time << 31 | f_dist) = lexicographic (f_time, f_dist), where
//     f = g + h.  The key and the label id fit the SAME OpenItem as the scalar search, so every open list of Section 2
//     (coarse, fine-grained, lazy, ...) works unchanged: OpenItem{key, label id, 0}.
//   * NAMOA* pruning: a label whose f vector is weakly dominated by a label already at the target cannot lead to a new
//     front point and is dropped (valid while h is admissible; in relaxed mode the result is an approximation).
//   * Parallel version: one small spin lock per node set, labels allocated from a lock-free chunked arena, and the same
//     termination counter as the scalar search (incremented BEFORE an insert, decremented after the parent's inserts).
//     An evicted label that was already queued is skipped when popped (alive flag).
#include <memory>

#include "search.h"

namespace astar {

constexpr uint32_t kNoLabel = 0xFFFFFFFFu;
constexpr int64_t kVectorLimit = (int64_t(1) << 31) - 1;  // costs / estimates at or above this are treated as infinite

inline int64_t pack_vector_key(int64_t f_time, int64_t f_dist) {
    if (f_time > kVectorLimit) f_time = kVectorLimit;
    if (f_dist > kVectorLimit) f_dist = kVectorLimit;
    return (f_time << 31) | f_dist;
}
inline int64_t key_time(int64_t key) { return key >> 31; }
inline int64_t key_dist(int64_t key) { return key & kVectorLimit; }

struct ParetoLabel {
    uint32_t g_time = 0, g_dist = 0, node = 0, parent = kNoLabel;
    std::atomic<uint8_t> alive{1};
};

// Append-only label storage.  allocate() is thread-safe; a label is read only after it was published through a node-set
// lock or the open list, which also publishes the chunk pointer.
class LabelArena {
public:
    LabelArena() : chunks_(new std::atomic<ParetoLabel*>[kMaxChunks]()) {}
    ~LabelArena() { for (uint32_t c = 0; c < kMaxChunks; ++c) delete[] chunks_[c].load(std::memory_order_relaxed); }
    LabelArena(const LabelArena&) = delete;
    LabelArena& operator=(const LabelArena&) = delete;

    void reset() { next_.store(0, std::memory_order_relaxed); overflow_.store(false, std::memory_order_relaxed); }
    bool overflowed() const { return overflow_.load(std::memory_order_relaxed); }
    uint32_t size() const { return std::min<uint64_t>(next_.load(std::memory_order_relaxed), uint64_t(kMaxChunks) * kChunkSize); }

    uint32_t allocate(uint32_t g_time, uint32_t g_dist, uint32_t node, uint32_t parent) {
        uint64_t id = next_.fetch_add(1, std::memory_order_relaxed);
        if (id >= uint64_t(kMaxChunks) * kChunkSize) { overflow_.store(true, std::memory_order_relaxed); return kNoLabel; }
        uint32_t chunk = uint32_t(id >> kChunkBits);
        ParetoLabel* base = chunks_[chunk].load(std::memory_order_acquire);
        if (!base) {
            ParetoLabel* fresh = new ParetoLabel[kChunkSize];
            if (chunks_[chunk].compare_exchange_strong(base, fresh, std::memory_order_acq_rel)) base = fresh;
            else delete[] fresh;  // another thread installed the chunk first; `base` now holds its pointer
        }
        ParetoLabel& label = base[id & (kChunkSize - 1)];
        label.g_time = g_time; label.g_dist = g_dist; label.node = node; label.parent = parent;
        label.alive.store(1, std::memory_order_relaxed);
        return uint32_t(id);
    }
    ParetoLabel& get(uint32_t id) const { return chunks_[id >> kChunkBits].load(std::memory_order_acquire)[id & (kChunkSize - 1)]; }

private:
    static constexpr uint32_t kChunkBits = 16, kChunkSize = 1u << kChunkBits, kMaxChunks = 1u << 14;  // up to 2^30 labels
    std::unique_ptr<std::atomic<ParetoLabel*>[]> chunks_;
    std::atomic<uint32_t> next_{0};
    std::atomic<bool> overflow_{false};
};

// Test-and-test-and-set spin lock for the (very short) node-set critical sections.
class SpinLock {
public:
    void lock() {
        unsigned spins = 0;
        for (;;) {
            if (!flag_.exchange(1, std::memory_order_acquire)) return;
            while (flag_.load(std::memory_order_relaxed)) { if (++spins > 2000) std::this_thread::yield(); else cpu_pause_hint(); }
        }
    }
    void unlock() { flag_.store(0, std::memory_order_release); }
private:
    std::atomic<uint8_t> flag_{0};
};

template <class Lock>
struct alignas(64) ParetoNodeSet {
    Lock lock;
    std::vector<uint32_t> ids;  // alive labels of this node, mutually non-dominated
};

// Immutable snapshot of the labels at the target ((time, dist) pairs). The search publishes a new snapshot whenever the target set
// changes and readers scan the current one WITHOUT a lock (the check "is this f vector dominated by a target label?" runs for
// every pop and every successor, so a lock on the target's set would serialise all threads).  Old snapshots stay alive until the
// next query (a few thousand at most) so a reader can never see freed memory.
struct TargetFront {
    std::vector<std::pair<uint32_t, uint32_t>> points;
};

template <class Lock>
struct ParetoWorkspace {
    LabelArena arena;
    std::unique_ptr<ParetoNodeSet<Lock>[]> sets;
    std::vector<std::unique_ptr<TargetFront>> front_versions;  // appended only under the target node's lock
    uint32_t num_nodes = 0;
    void prepare(uint32_t nodes) {
        front_versions.clear();
        if (num_nodes != nodes) { sets.reset(new ParetoNodeSet<Lock>[nodes]); num_nodes = nodes; }
        else for (uint32_t node = 0; node < nodes; ++node) sets[node].ids.clear();
        arena.reset();
    }
};

struct ParetoCounters {
    uint64_t expansions = 0;           // labels expanded
    uint64_t dead_pops = 0;            // popped labels that had been evicted meanwhile
    uint64_t pruned = 0;               // labels dropped because their f vector is dominated by a target label
    uint64_t labels_created = 0;       // labels that entered a node set
    uint64_t labels_evicted = 0;       // labels removed from a node set by a newer dominating label
    uint64_t dominated_discards = 0;   // new labels rejected because the node set already dominated them
    uint64_t pushes = 0;               // labels put into the open list
    uint64_t idle_spins = 0;           // polls of an empty list while work was still pending
    void operator+=(const ParetoCounters& o) {
        expansions += o.expansions; dead_pops += o.dead_pops; pruned += o.pruned; labels_created += o.labels_created;
        labels_evicted += o.labels_evicted; dominated_discards += o.dominated_discards; pushes += o.pushes; idle_spins += o.idle_spins;
    }
};

struct FrontPoint { int64_t time_ms, dist_dm; };

struct ParetoResult {
    std::vector<FrontPoint> front;   // sorted by time ascending (distance descending)
    bool path_ok = true;             // every front point's path re-evaluated with the true costs gives exactly its vector
    bool overflow = false;           // label arena exhausted: the front may be incomplete
    ParetoCounters counters;
    ListOpStats list_stats;
    int64_t search_ns = 0, init_ns = 0;
};

namespace detail {

template <class Lock>
struct ParetoContext {
    const Problem& problem;
    Problem time_problem, dist_problem;
    const Graph& graph;
    Heuristic h_time, h_dist;
    ParetoWorkspace<Lock>& ws;
    std::atomic<const TargetFront*> target_front{nullptr};  // current snapshot of the target's labels

    ParetoContext(const Problem& p, ParetoWorkspace<Lock>& workspace)
        : problem(p), time_problem(p), dist_problem(p), graph(*p.graph),
          h_time(*p.graph, p.target, Objective::Time, p.heuristic_config),
          h_dist(*p.graph, p.target, Objective::Distance, p.heuristic_config), ws(workspace) {
        time_problem.objective = Objective::Time;
        dist_problem.objective = Objective::Distance;
    }
};

// Is (f_time, f_dist) weakly dominated by a label already at the target?
template <class Lock>
bool target_blocks(ParetoContext<Lock>& ctx, int64_t f_time, int64_t f_dist) {
    const TargetFront* front = ctx.target_front.load(std::memory_order_acquire);
    if (!front) return false;
    for (const auto& point : front->points)
        if (int64_t(point.first) <= f_time && int64_t(point.second) <= f_dist) return true;
    return false;
}

// Adds a label to `node`'s set unless the set weakly dominates it; evicts what it dominates. Returns its id or kNoLabel.
template <class Lock>
uint32_t try_insert_label(ParetoContext<Lock>& ctx, uint32_t node, uint32_t g_time, uint32_t g_dist, uint32_t parent, ParetoCounters& counters) {
    ParetoNodeSet<Lock>& set = ctx.ws.sets[node];
    LabelArena& arena = ctx.ws.arena;
    std::lock_guard<Lock> guard(set.lock);
    for (uint32_t id : set.ids) {
        const ParetoLabel& old_label = arena.get(id);
        if (old_label.g_time <= g_time && old_label.g_dist <= g_dist) { counters.dominated_discards++; return kNoLabel; }
    }
    uint32_t fresh = arena.allocate(g_time, g_dist, node, parent);
    if (fresh == kNoLabel) return kNoLabel;
    size_t kept = 0;
    for (size_t i = 0; i < set.ids.size(); ++i) {
        ParetoLabel& old_label = arena.get(set.ids[i]);
        if (g_time <= old_label.g_time && g_dist <= old_label.g_dist) { old_label.alive.store(0, std::memory_order_release); counters.labels_evicted++; }
        else set.ids[kept++] = set.ids[i];
    }
    set.ids.resize(kept);
    set.ids.push_back(fresh);
    counters.labels_created++;
    if (node == ctx.problem.target) {  // publish a new immutable snapshot (still under the target's lock)
        auto snapshot = std::make_unique<TargetFront>();
        snapshot->points.reserve(set.ids.size());
        for (uint32_t id : set.ids) snapshot->points.push_back({arena.get(id).g_time, arena.get(id).g_dist});
        ctx.target_front.store(snapshot.get(), std::memory_order_release);
        ctx.ws.front_versions.push_back(std::move(snapshot));
    }
    return fresh;
}

// Generates the successor labels of one label; `push(key, label_id)` queues every new non-target label.
template <class Lock, class Push>
void expand_label(ParetoContext<Lock>& ctx, uint32_t label_id, ParetoCounters& counters, Push&& push) {
    const Graph& graph = ctx.graph;
    const ParetoLabel& label = ctx.ws.arena.get(label_id);
    const uint32_t node = label.node, g_time = label.g_time, g_dist = label.g_dist, target = ctx.problem.target;
    for (uint32_t edge = graph.first_edge[node]; edge < graph.first_edge[node + 1]; ++edge) {
        uint32_t next = graph.edge_target[edge];
        int64_t new_time = int64_t(g_time) + ctx.time_problem.edge_cost(edge, g_time);
        int64_t new_dist = int64_t(g_dist) + int64_t(graph.edge_length_dm[edge]);
        if (new_time >= kVectorLimit || new_dist >= kVectorLimit) continue;
        int64_t f_time = new_time + ctx.h_time(next, ctx.problem.departure_ms + new_time);
        int64_t f_dist = new_dist + ctx.h_dist(next, 0);
        if (next != target && target_blocks(ctx, f_time, f_dist)) { counters.pruned++; continue; }
        uint32_t fresh = try_insert_label(ctx, next, uint32_t(new_time), uint32_t(new_dist), label_id, counters);
        if (fresh == kNoLabel || next == target) continue;
        push(pack_vector_key(f_time, f_dist), fresh);
        counters.pushes++;
    }
}

// Collects the labels left at the target, sorted by time, and re-evaluates each path with the true cost functions.
template <class Lock>
void extract_front(ParetoContext<Lock>& ctx, ParetoResult& result) {
    LabelArena& arena = ctx.ws.arena;
    std::vector<uint32_t> ids = ctx.ws.sets[ctx.problem.target].ids;
    std::sort(ids.begin(), ids.end(), [&](uint32_t a, uint32_t b) { return arena.get(a).g_time < arena.get(b).g_time; });
    for (uint32_t id : ids) {
        const ParetoLabel& end = arena.get(id);
        result.front.push_back({int64_t(end.g_time), int64_t(end.g_dist)});
        std::vector<uint32_t> path;
        uint32_t walk = id;
        for (uint32_t steps = 0; walk != kNoLabel && steps <= ctx.graph.num_nodes + 1; ++steps) {
            path.push_back(arena.get(walk).node);
            walk = arena.get(walk).parent;
        }
        std::reverse(path.begin(), path.end());
        bool ok = recompute_path_cost(ctx.time_problem, path) == int64_t(end.g_time) && recompute_path_cost(ctx.dist_problem, path) == int64_t(end.g_dist);
        if (!ok) result.path_ok = false;
    }
    result.overflow = arena.overflowed();
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Sequential vector A*: std::priority_queue over the packed key, node sets without synchronisation.
// ---------------------------------------------------------------------------
inline ParetoResult seq_pareto_astar(const Problem& problem, ParetoWorkspace<NullLock>& workspace) {
    using Clock = std::chrono::steady_clock;
    ParetoResult result;
    auto init_start = Clock::now();
    workspace.prepare(problem.graph->num_nodes);
    detail::ParetoContext<NullLock> ctx(problem, workspace);
    auto search_start = Clock::now();
    using Entry = std::pair<int64_t, uint32_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> open_set;
    ParetoCounters& counters = result.counters;
    uint32_t first = detail::try_insert_label(ctx, problem.source, 0, 0, kNoLabel, counters);
    if (problem.source != problem.target && first != kNoLabel)
        open_set.push({pack_vector_key(ctx.h_time(problem.source, problem.departure_ms), ctx.h_dist(problem.source, 0)), first});
    while (!open_set.empty()) {
        Entry entry = open_set.top();
        open_set.pop();
        if (!workspace.arena.get(entry.second).alive.load(std::memory_order_relaxed)) { counters.dead_pops++; continue; }
        if (detail::target_blocks(ctx, key_time(entry.first), key_dist(entry.first))) { counters.pruned++; continue; }
        counters.expansions++;
        detail::expand_label(ctx, entry.second, counters, [&](int64_t key, uint32_t id) { open_set.push({key, id}); });
    }
    auto search_end = Clock::now();
    result.init_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(search_start - init_start).count();
    result.search_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(search_end - search_start).count();
    detail::extract_front(ctx, result);
    return result;
}

// ---------------------------------------------------------------------------
// Parallel vector A*: any open list with the frozen API; node sets protected by one spin lock each.
// ---------------------------------------------------------------------------
struct alignas(128) PaddedParetoCounters {
    ParetoCounters counters;
};

template <class List>
ParetoResult par_pareto_astar(const Problem& problem, ThreadPool& pool, ParetoWorkspace<SpinLock>& workspace) {
    using Clock = std::chrono::steady_clock;
    ParetoResult result;
    auto init_start = Clock::now();
    workspace.prepare(problem.graph->num_nodes);
    detail::ParetoContext<SpinLock> ctx(problem, workspace);
    const int num_threads = pool.threads();
    std::vector<PaddedParetoCounters> per_thread(num_threads);
    std::mutex flush_mutex;
    InstrumentedList<List> open_list;
    std::atomic<int64_t> pending_items{0};
    uint32_t first = detail::try_insert_label(ctx, problem.source, 0, 0, kNoLabel, result.counters);
    if (problem.source != problem.target && first != kNoLabel) {
        pending_items.store(1);
        int64_t source_time_f = ctx.h_time(problem.source, problem.departure_ms);
        // the window is measured on the f_time part of the packed key (f_time << 31 | f_dist); no-op unless the list is a WindowedList
        open_list.configure_window((source_time_f / window_divisor().load(std::memory_order_relaxed)) << 31);
        open_list.add({pack_vector_key(source_time_f, ctx.h_dist(problem.source, 0)), first, 0});
    }
    auto search_start = Clock::now();

    pool.run([&](int thread_id) {
        ParetoCounters& counters = per_thread[thread_id].counters;
        OpenItem item;
        unsigned idle_polls = 0;
        for (;;) {
            if (!open_list.pop_min(item)) {
                if (pending_items.load(std::memory_order_acquire) == 0) break;
                counters.idle_spins++;
                if (++idle_polls > 256) std::this_thread::yield(); else cpu_pause_hint();
                continue;
            }
            idle_polls = 0;
            if (!workspace.arena.get(item.node).alive.load(std::memory_order_acquire)) counters.dead_pops++;
            else if (detail::target_blocks(ctx, key_time(item.f_value), key_dist(item.f_value))) counters.pruned++;
            else {
                counters.expansions++;
                detail::expand_label(ctx, item.node, counters, [&](int64_t key, uint32_t id) {
                    pending_items.fetch_add(1, std::memory_order_acq_rel);  // count BEFORE the label becomes visible
                    if (!open_list.add({key, id, 0})) pending_items.fetch_sub(1, std::memory_order_acq_rel);
                });
            }
            pending_items.fetch_sub(1, std::memory_order_acq_rel);  // only after ALL successors were inserted
        }
        open_list.flush(result.list_stats, flush_mutex);
    });

    auto search_end = Clock::now();
    for (auto& entry : per_thread) result.counters += entry.counters;
    result.init_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(search_start - init_start).count();
    result.search_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(search_end - search_start).count();
    detail::extract_front(ctx, result);
    return result;
}

}  // namespace astar
