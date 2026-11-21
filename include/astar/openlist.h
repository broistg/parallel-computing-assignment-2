#pragma once
// FROZEN list interface for the A* open set (sorted linked-list SET of OpenItem).
// Every list variant of Section 2 (coarse / fine / optimistic / lazy) must provide:
//
//   bool add(const OpenItem&)      true if inserted, false if an equal item is already present
//   bool remove(const OpenItem&)   true if removed
//   bool contains(const OpenItem&) linearizable membership test
//   bool pop_min(OpenItem& out)    remove and return the smallest item; false if empty
//   size_t size_approx() const     racy size estimate, for statistics only
//
// Ordering is lexicographic on (f_value, node, g_cost).  Items are small trivially-copyable values.
// A list never blocks on emptiness; callers poll.  Memory: a variant may defer freeing
// removed nodes to its destructor (no reclamation scheme required); the A* driver
// creates a fresh list per query.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace astar {

// One entry of the open set: a node together with the path cost g_cost it was reached with and
// its priority f_value = g_cost + heuristic.  Ordered by (f_value, node, g_cost).
struct OpenItem {
    int64_t f_value;
    uint32_t node;
    uint32_t g_cost;
};
inline bool operator<(const OpenItem& a, const OpenItem& b) {
    if (a.f_value != b.f_value) return a.f_value < b.f_value;
    if (a.node != b.node) return a.node < b.node;
    return a.g_cost < b.g_cost;
}
inline bool operator==(const OpenItem& a, const OpenItem& b) { return a.f_value == b.f_value && a.node == b.node && a.g_cost == b.g_cost; }

// ---------------------------------------------------------------------------
// Coarse-grained list: one lock for the whole list (lock type is a template parameter,
// any lock from lock.h works).  Reference implementation, also used by the tests.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Operation statistics (per thread, summed into the search result at the end of a worker).
// Insert profile (opt-in, --list-profile): how many list nodes each insert walked past, the list length at that moment, and WHERE in
// the list the item landed (steps / length in eighths: bucket 0 = within the first eighth = near the head, bucket 7 = near the tail).
// It answers "is the O(length) insert traversal the cost, and would a tail pointer or a hint shorten it".
// ---------------------------------------------------------------------------
struct ListOpStats {
    static constexpr int kPositionBuckets = 8;
    uint64_t adds = 0, add_dups = 0, pops = 0, pop_empty = 0;
    uint64_t add_ns = 0, add_samples = 0, pop_ns = 0, pop_samples = 0;
    uint64_t ins_profiled = 0, ins_steps = 0, ins_size_sum = 0, ins_size_max = 0;
    uint64_t ins_position[kPositionBuckets] = {};
    // Stall diagnostics (opt-in, --diag, file <out>.diag.csv): where does a thread wait, and who holds the work while it waits.
    uint64_t lock_acq = 0, lock_wait_ns = 0, lock_wait_max_ns = 0;          // time to acquire the shared-list lock (coarse list)
    uint64_t idle_episodes = 0, idle_ns = 0, idle_max_ns = 0, idle_final_ns = 0;  // searches' empty-list waits; final = the wait that ended the search
    uint64_t idle_samples = 0, hoard_sum = 0, hoard_max = 0, held_sum = 0, shared_sum = 0;  // sampled at an empty pop: items other threads hold privately
    uint64_t takes = 0, take_items = 0, take_short = 0;                      // pop batches: how many, items taken, batches smaller than requested
    // Critical-section length (coarse list, --diag): time the lock was HELD, and the list length at acquisition. Long holds on a SHORT
    // list = the holder was descheduled; long holds on a LONG list = the walk itself is slow.
    uint64_t hold_ns = 0, hold_max_ns = 0, hold_ge100us = 0, hold_ge1ms = 0, hold_ge1ms_add = 0, hold_ge1ms_size_sum = 0;
    uint64_t list_size_sum = 0, list_size_max = 0, list_size_ge1ms_max = 0;
    uint64_t promotes = 0, promoted_items = 0, promoted_max = 0;           // WindowedList: bag -> shared list promotions (--diag)
    uint64_t advances = 0, advance_gap_sum = 0, advance_gap_max = 0, advance_far = 0;  // window advances: own bag minimum minus global minimum; far = gap > window
    void operator+=(const ListOpStats& o) {
        adds += o.adds; add_dups += o.add_dups; pops += o.pops; pop_empty += o.pop_empty;
        add_ns += o.add_ns; add_samples += o.add_samples; pop_ns += o.pop_ns; pop_samples += o.pop_samples;
        ins_profiled += o.ins_profiled; ins_steps += o.ins_steps; ins_size_sum += o.ins_size_sum;
        if (o.ins_size_max > ins_size_max) ins_size_max = o.ins_size_max;
        for (int i = 0; i < kPositionBuckets; ++i) ins_position[i] += o.ins_position[i];
        lock_acq += o.lock_acq; lock_wait_ns += o.lock_wait_ns; lock_wait_max_ns = std::max(lock_wait_max_ns, o.lock_wait_max_ns);
        idle_episodes += o.idle_episodes; idle_ns += o.idle_ns; idle_max_ns = std::max(idle_max_ns, o.idle_max_ns); idle_final_ns += o.idle_final_ns;
        idle_samples += o.idle_samples; hoard_sum += o.hoard_sum; hoard_max = std::max(hoard_max, o.hoard_max);
        held_sum += o.held_sum; shared_sum += o.shared_sum;
        takes += o.takes; take_items += o.take_items; take_short += o.take_short;
        hold_ns += o.hold_ns; hold_max_ns = std::max(hold_max_ns, o.hold_max_ns); hold_ge100us += o.hold_ge100us; hold_ge1ms += o.hold_ge1ms;
        hold_ge1ms_add += o.hold_ge1ms_add; hold_ge1ms_size_sum += o.hold_ge1ms_size_sum;
        list_size_sum += o.list_size_sum; list_size_max = std::max(list_size_max, o.list_size_max);
        list_size_ge1ms_max = std::max(list_size_ge1ms_max, o.list_size_ge1ms_max);
        promotes += o.promotes; promoted_items += o.promoted_items; promoted_max = std::max(promoted_max, o.promoted_max);
        advances += o.advances; advance_gap_sum += o.advance_gap_sum; advance_gap_max = std::max(advance_gap_max, o.advance_gap_max); advance_far += o.advance_far;
    }
};

inline ListOpStats& tls_list_stats() {
    static thread_local ListOpStats s;
    return s;
}
inline std::atomic<bool>& list_latency_sampling() {
    static std::atomic<bool> on{false};
    return on;
}
inline std::atomic<bool>& list_diagnostics() {
    static std::atomic<bool> on{false};
    return on;
}
inline std::atomic<bool>& list_insert_profiling() {
    static std::atomic<bool> on{false};
    return on;
}
// Called by a list inside its critical section after an insert that walked `steps` nodes in a list of `size` items.
// `steps` = nodes walked for this insert (for a batch item: since the previous item), `position` = nodes from the head to the insert point.
inline void record_insert_profile(uint64_t steps, uint64_t position, uint64_t size) {
    if (!list_insert_profiling().load(std::memory_order_relaxed)) return;
    ListOpStats& stats = tls_list_stats();
    stats.ins_profiled++;
    stats.ins_steps += steps;
    stats.ins_size_sum += size;
    if (size > stats.ins_size_max) stats.ins_size_max = size;
    int bucket = size == 0 ? 0 : int(position * ListOpStats::kPositionBuckets / (size + 1));
    stats.ins_position[bucket >= ListOpStats::kPositionBuckets ? ListOpStats::kPositionBuckets - 1 : bucket]++;
}

// A lock that does nothing: CoarseList<NullLock> is the sorted list WITHOUT any synchronisation. Only valid with one
// thread; it exists so the sequential baseline with the same data structure (seqlist) separates the cost of the sorted
// list from the cost of locking.
struct NullLock {
    void lock() {}
    void unlock() {}
};

template <class Lock>
class CoarseList {
    struct Node {
        OpenItem item;
        Node* next;
    };

public:
    CoarseList() : head_(new Node{{0, 0, 0}, nullptr}) {}
    ~CoarseList() {
        while (head_) {
            Node* n = head_->next;
            delete head_;
            head_ = n;
        }
    }
    CoarseList(const CoarseList&) = delete;
    CoarseList& operator=(const CoarseList&) = delete;

    bool add(const OpenItem& x) {
        Node* fresh = new Node{x, nullptr};  // allocate outside the critical section
        const uint64_t held = acquire();
        Node* p = head_;
        uint64_t steps = 0;
        while (p->next && p->next->item < x) { p = p->next; ++steps; }
        if (p->next && p->next->item == x) {
            release(held, true);
            delete fresh;
            return false;
        }
        fresh->next = p->next;
        p->next = fresh;
        record_insert_profile(steps, steps, size_.load(std::memory_order_relaxed));
        size_.fetch_add(1, std::memory_order_relaxed);
        release(held, true);
        return true;
    }

    bool remove(const OpenItem& x) {
        lock_.lock();
        Node* p = head_;
        while (p->next && p->next->item < x) p = p->next;
        Node* victim = p->next;
        if (!victim || !(victim->item == x)) {
            lock_.unlock();
            return false;
        }
        p->next = victim->next;
        size_.fetch_sub(1, std::memory_order_relaxed);
        lock_.unlock();
        delete victim;
        return true;
    }

    bool contains(const OpenItem& x) {
        lock_.lock();
        Node* p = head_->next;
        while (p && p->item < x) p = p->next;
        bool r = p && p->item == x;
        lock_.unlock();
        return r;
    }

    bool pop_min(OpenItem& out) {
        const uint64_t held = acquire();
        Node* first = head_->next;
        if (!first) {
            release(held, false);
            return false;
        }
        head_->next = first->next;
        size_.fetch_sub(1, std::memory_order_relaxed);
        release(held, false);
        out = first->item;
        delete first;
        return true;
    }

    // OPTIONAL batch operations (not part of the frozen API; searches use them only through list_add_batch / list_pop_batch,
    // which fall back to add / pop_min for lists without them).  One lock acquisition and one traversal for the whole batch.
    // add_batch: items must be sorted ascending; returns how many were inserted (equal items already present are skipped).
    size_t add_batch(const OpenItem* items, size_t n) {
        std::vector<Node*> fresh(n);
        for (size_t i = 0; i < n; ++i) fresh[i] = new Node{items[i], nullptr};  // allocate outside the critical section
        size_t inserted = 0;
        const uint64_t held = acquire();
        Node* p = head_;
        uint64_t steps = 0, walked = 0;  // steps: since the previous item (the walk is shared); walked: from the head
        const uint64_t size_before = size_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < n; ++i) {
            while (p->next && p->next->item < items[i]) { p = p->next; ++steps; ++walked; }
            if (p->next && p->next->item == items[i]) { delete fresh[i]; continue; }
            fresh[i]->next = p->next;
            p->next = fresh[i];
            p = fresh[i];
            record_insert_profile(steps, walked, size_before + inserted);
            steps = 0;
            ++inserted;
        }
        size_.fetch_add(inserted, std::memory_order_relaxed);
        release(held, true);
        return inserted;
    }
    // pop_batch: removes up to k smallest items (ascending into out); returns how many.
    size_t pop_batch(OpenItem* out, size_t k) {
        const uint64_t held = acquire();
        Node* first = head_->next;
        Node* last = nullptr;
        size_t taken = 0;
        for (Node* q = first; q && taken < k; q = q->next) { last = q; ++taken; }
        if (taken) {
            head_->next = last->next;
            last->next = nullptr;
            size_.fetch_sub(taken, std::memory_order_relaxed);
        }
        release(held, false);
        size_t i = 0;
        for (Node* q = first; q;) {
            Node* next = q->next;
            out[i++] = q->item;
            delete q;
            q = next;
        }
        return taken;
    }

    size_t size_approx() const { return size_.load(std::memory_order_relaxed); }

private:
    // lock_.lock(), timed when --diag is on (time spent waiting for the lock = contention / convoy). Returns the time the lock was
    // obtained (0 without --diag), to be passed to release().
    uint64_t acquire() {
        if (!list_diagnostics().load(std::memory_order_relaxed)) { lock_.lock(); return 0; }
        auto t0 = std::chrono::steady_clock::now();
        lock_.lock();
        auto t1 = std::chrono::steady_clock::now();
        uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        ListOpStats& stats = tls_list_stats();
        stats.lock_acq++;
        stats.lock_wait_ns += ns;
        if (ns > stats.lock_wait_max_ns) stats.lock_wait_max_ns = ns;
        uint64_t size = size_.load(std::memory_order_relaxed);
        stats.list_size_sum += size;
        if (size > stats.list_size_max) stats.list_size_max = size;
        return uint64_t(t1.time_since_epoch().count());
    }
    // lock_.unlock(), recording how long the lock was held (is_add: the operation inserts, else it pops).
    void release(uint64_t acquired, bool is_add) {
        if (acquired != 0) {
            uint64_t now = uint64_t(std::chrono::steady_clock::now().time_since_epoch().count());
            uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::duration(now - acquired)).count());
            ListOpStats& stats = tls_list_stats();
            stats.hold_ns += ns;
            if (ns > stats.hold_max_ns) stats.hold_max_ns = ns;
            if (ns >= 100000) stats.hold_ge100us++;
            if (ns >= 1000000) {
                uint64_t size = size_.load(std::memory_order_relaxed);
                stats.hold_ge1ms++;
                if (is_add) stats.hold_ge1ms_add++;
                stats.hold_ge1ms_size_sum += size;
                if (size > stats.list_size_ge1ms_max) stats.list_size_ge1ms_max = size;
            }
        }
        lock_.unlock();
    }
    Node* head_;  // sentinel
    Lock lock_;
    std::atomic<size_t> size_{0};
};

// ---------------------------------------------------------------------------
// Instrumentation wrapper: operation mix (always), sampled operation latency (opt-in), insert profile (opt-in, see above).
// Per-thread counters live in a thread_local struct; call flush() at the end of a worker.
// ---------------------------------------------------------------------------
// Optional batch operations of a list (see CoarseList); the helpers below fall back to add / pop_min.
template <class T, class = void>
struct has_add_batch : std::false_type {};
template <class T>
struct has_add_batch<T, std::void_t<decltype(std::declval<T&>().add_batch(static_cast<const OpenItem*>(nullptr), size_t(0)))>> : std::true_type {};
template <class T, class = void>
struct has_pop_batch : std::false_type {};
template <class T>
struct has_pop_batch<T, std::void_t<decltype(std::declval<T&>().pop_batch(static_cast<OpenItem*>(nullptr), size_t(0)))>> : std::true_type {};

// Detects lists that take a window size from the search (WindowedList); used by InstrumentedList::configure_window.
template <class T, class = void>
struct has_set_window : std::false_type {};
template <class T>
struct has_set_window<T, std::void_t<decltype(std::declval<T&>().set_window(int64_t(0)))>> : std::true_type {};

template <class L>
class InstrumentedList {
    static constexpr unsigned kSampleMask = 7;  // 1 in 8 operations when sampling is on
    using clk = std::chrono::steady_clock;

public:
    bool add(const OpenItem& x) {
        ListOpStats& stats = tls_list_stats();
        bool r;
        if (sample(stats.adds)) {
            auto t0 = clk::now();
            r = impl_.add(x);
            stats.add_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count());
            stats.add_samples++;
        } else {
            r = impl_.add(x);
        }
        stats.adds++;
        if (!r) stats.add_dups++;
        return r;
    }
    bool pop_min(OpenItem& out) {
        ListOpStats& stats = tls_list_stats();
        bool r;
        if (sample(stats.pops + stats.pop_empty)) {
            auto t0 = clk::now();
            r = impl_.pop_min(out);
            stats.pop_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(clk::now() - t0).count());
            stats.pop_samples++;
        } else {
            r = impl_.pop_min(out);
        }
        if (r) stats.pops++; else stats.pop_empty++;
        return r;
    }
    bool remove(const OpenItem& x) { return impl_.remove(x); }
    bool contains(const OpenItem& x) { return impl_.contains(x); }
    size_t size_approx() const { return impl_.size_approx(); }
    // Lists that keep a bounded shared window (WindowedList) take its size from the search; for every other list this is a no-op.
    template <class Window>
    void configure_window(Window delta) { if constexpr (has_set_window<L>::value) impl_.set_window(delta); }

    // Move this thread's counters into `total` (call once at the end of a worker) and reset them.
    template <class Mutex>
    void flush(ListOpStats& total, Mutex& m) {
        std::lock_guard<Mutex> g(m);
        total += tls_list_stats();
        tls_list_stats() = ListOpStats{};
    }

private:
    static bool sample(uint64_t n) {
        return list_latency_sampling().load(std::memory_order_relaxed) && (n & kSampleMask) == 0;
    }
    L impl_;
};

}  // namespace astar
