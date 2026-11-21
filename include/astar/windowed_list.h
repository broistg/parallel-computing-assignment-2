#pragma once
// WindowedList<List>: an adapter that keeps the shared sorted list SHORT.
//
// Why: in A* the open list holds the whole frontier (thousands of items; hundreds of thousands in the vector search), and a
// sorted-list insert costs O(list length).  Measured on the real graph: the same search with the sorted list and NO lock and one
// thread is 4.1x slower than with a binary heap, and the cost grows with the query length (1.4x for 1-3 km, 5.3x for 15-25 km).
//
// How: only items whose f_value is within a WINDOW of the best known frontier go into the shared list (any list implementing the
// frozen open-list API).  Items beyond the window go into a cheap per-thread unsorted bag (no synchronisation).  When a thread
// finds the shared list empty it advances the window from the smallest f_value in its own bag and moves the items that now fit into
// the shared list; every thread also promotes its own bag items when it sees that another thread advanced the window.
//
// Effect on A*: nodes are expanded in "almost" f order (within one window), so a few nodes are expanded before their final cost is
// known and are re-expanded later (reopening is already part of the parallel search).  The result stays exact; the cost is a few
// percent more expansions (reported by the counters).  Termination is unchanged: bag items are counted by the A* pending counter
// when they are added, so pending_items == 0 still means that nothing is left anywhere.
//
// Limits: remove()/contains() only see the shared list (A* does not use them); the A* never inserts a duplicate item (a node's cost
// only ever decreases strictly), so dropping duplicates while promoting cannot happen.  At most kMaxSlots threads.
#include <algorithm>
#include <atomic>
#include <climits>
#include <memory>
#include <vector>

#include "openlist.h"

namespace astar {

// Window = delta added to the f_value of the first item (usually the source). The divisor is set from the command line.
inline std::atomic<int>& window_divisor() {
    static std::atomic<int> divisor{4096};  // measured optimum on the HCMC graph, 6 cores (REPORT section 4)
    return divisor;
}

// Number of items a thread buffers before it touches the shared list (adds) and takes from it per lock acquisition (pops).
// 1 = no batching.  Set from the command line (--batch).  Batching uses add_batch / pop_batch when the list has them.
inline std::atomic<int>& window_batch() {
    static std::atomic<int> batch{8};  // measured optimum: 8-16 scalar, 4-8 vector
    return batch;
}

// Window slack (--window-slack K; -1 = off, the default and the original behaviour). A thread that finds the shared list empty opens
// the window around its OWN smallest bag item. That is what gives the other threads speculative work, but a thread whose bag holds only
// large-f items can push the threshold far past the real front: every later successor below the threshold then goes straight into the
// shared list, which grows to thousands of items and saturates its lock (REPORT section 4, vector stalls). With K >= 0 the base of the
// window is min(own smallest, global smallest + K * window), where global = minimum over all threads' published bag minima. K = 0 follows
// the global minimum exactly (short list, but threads starve: measured), large K approaches the original behaviour.
inline std::atomic<int>& window_slack() {
    static std::atomic<int> k{-1};
    return k;
}

// --window-skip-empty: before locking the shared list to take items, read its (atomic) size and skip the lock when it is 0. Idle threads
// otherwise lock the list on every poll and delay the threads that have work. A stale 0 only delays a poll; termination is decided by the
// search's pending counter, not by this check.
inline std::atomic<bool>& window_skip_empty() {
    static std::atomic<bool> on{false};
    return on;
}

template <class List>
class WindowedList {
public:
    WindowedList() : instance_(next_instance_id()), bags_(new PaddedBag[kMaxSlots]) {}

    // Called once by the search before the first add: delta in f_value units (>= 1).
    void set_window(int64_t delta) { window_ = std::max<int64_t>(1, delta); }

    bool add(const OpenItem& item) {
        int64_t threshold = threshold_.load(std::memory_order_acquire);
        if (threshold == kUnset) {  // first item: open the window around it
            int64_t desired = item.f_value + window_;
            threshold_.compare_exchange_strong(threshold, desired, std::memory_order_acq_rel);
            threshold = threshold_.load(std::memory_order_acquire);
        }
        Bag& bag = my_bag();
        if (item.f_value <= threshold) {
            const size_t batch = size_t(window_batch().load(std::memory_order_relaxed));
            if (batch <= 1) return list_.add(item);
            bag.adds.push_back(item);  // buffered; published when the buffer is full or before this thread looks for work
            if (bag.adds.size() >= batch) flush_adds(bag);
            return true;
        }
        bag.items.push_back(item);
        if (item.f_value < bag.min_f) { bag.min_f = item.f_value; bag.pub_min.store(bag.min_f, std::memory_order_relaxed); }
        if (list_diagnostics().load(std::memory_order_relaxed)) publish(bag);
        return true;
    }

    bool pop_min(OpenItem& out) {
        if (pop_min_impl(out)) return true;
        if (list_diagnostics().load(std::memory_order_relaxed)) sample_holders();
        return false;
    }

    bool pop_min_impl(OpenItem& out) {
        Bag& bag = my_bag();
        if (!bag.prefetched.empty()) {  // no shared-list access
            out = bag.prefetched.back(); bag.prefetched.pop_back();
            publish(bag);
            return true;
        }
        flush_adds(bag);  // publish this thread's buffered successors before looking for work
        int64_t threshold = threshold_.load(std::memory_order_acquire);
        if (!bag.items.empty() && threshold != bag.seen_threshold && bag.min_f <= threshold) promote(bag, threshold);  // another thread advanced
        bag.seen_threshold = threshold;
        if (take(bag, out)) return true;
        if (bag.items.empty()) return false;
        // shared list empty: advance the window from the smallest bag item (global: of all threads; default: of this thread)
        int64_t base = bag.min_f;
        const int slack = window_slack().load(std::memory_order_relaxed);
        if (slack >= 0 || list_diagnostics().load(std::memory_order_relaxed)) {
            const int64_t gmin = std::min(bag.min_f, other_bag_min(bag));
            if (list_diagnostics().load(std::memory_order_relaxed)) {
                ListOpStats& stats = tls_list_stats();
                const uint64_t gap = uint64_t(bag.min_f - gmin);
                stats.advances++;
                stats.advance_gap_sum += gap;
                if (gap > stats.advance_gap_max) stats.advance_gap_max = gap;
                if (gap > uint64_t(window_)) stats.advance_far++;
            }
            if (slack >= 0) {
                const int64_t limit = gmin > INT64_MAX - (int64_t(slack) + 1) * window_ ? INT64_MAX : gmin + int64_t(slack) * window_;
                base = std::min(base, limit);
            }
        }
        const int64_t wanted = base + window_;
        threshold = threshold_.load(std::memory_order_acquire);
        while (threshold < wanted && !threshold_.compare_exchange_weak(threshold, wanted, std::memory_order_acq_rel)) {}
        threshold = std::max(threshold, wanted);
        promote(bag, threshold);
        bag.seen_threshold = threshold;
        return take(bag, out);
    }

    bool remove(const OpenItem& item) { return list_.remove(item); }
    bool contains(const OpenItem& item) { return list_.contains(item); }
    size_t size_approx() const { return list_.size_approx(); }

private:
    static constexpr int kMaxSlots = 128;
    static constexpr int64_t kUnset = INT64_MIN;
    struct alignas(128) PaddedBag {
        std::vector<OpenItem> adds;        // successors waiting to be inserted into the shared list as one batch
        std::vector<OpenItem> prefetched;  // items taken from the shared list in one batch, largest first (back = next to expand)
        std::vector<OpenItem> items;       // beyond the window
        int64_t min_f = INT64_MAX;
        int64_t seen_threshold = kUnset;
        std::atomic<uint32_t> pub_prefetched{0};  // --diag only: sizes published so an idle thread can see who holds work
        std::atomic<uint32_t> pub_items{0};
        uint32_t idle_polls = 0;
        std::atomic<int64_t> pub_min{INT64_MAX};  // smallest f in `items`, published for other threads' window advances
    };
    using Bag = PaddedBag;

    static uint64_t next_instance_id() {
        static std::atomic<uint64_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }
    Bag& my_bag() {  // each thread owns one bag slot per list instance
        struct Cache { uint64_t instance = 0; int slot = 0; };
        static thread_local Cache cache;
        if (cache.instance != instance_) {
            cache.instance = instance_;
            cache.slot = next_slot_.fetch_add(1, std::memory_order_relaxed);
            if (cache.slot >= kMaxSlots) std::abort();  // more threads than slots
        }
        return bags_[cache.slot];
    }
    void flush_adds(Bag& bag) {
        if (bag.adds.empty()) return;
        std::sort(bag.adds.begin(), bag.adds.end());
        if constexpr (has_add_batch<List>::value) list_.add_batch(bag.adds.data(), bag.adds.size());
        else for (const OpenItem& item : bag.adds) list_.add(item);
        bag.adds.clear();
    }
    // Takes the smallest item (and, with batching and a list that supports it, up to batch-1 more into the prefetch buffer).
    bool take(Bag& bag, OpenItem& out) {
        if (window_skip_empty().load(std::memory_order_relaxed) && list_.size_approx() == 0) return false;
        const size_t batch = size_t(window_batch().load(std::memory_order_relaxed));
        if constexpr (has_pop_batch<List>::value) {
            if (batch > 1) {
                OpenItem got[64];
                size_t n = list_.pop_batch(got, std::min<size_t>(batch, 64));
                if (n == 0) return false;
                out = got[0];
                for (size_t i = n; i-- > 1;) bag.prefetched.push_back(got[i]);  // largest first, so back() is the smallest remaining
                if (list_diagnostics().load(std::memory_order_relaxed)) {
                    ListOpStats& stats = tls_list_stats();
                    stats.takes++; stats.take_items += n; if (n < batch) stats.take_short++;
                    publish(bag);
                }
                return true;
            }
        }
        return list_.pop_min(out);
    }
    // Smallest bag minimum published by the OTHER threads (INT64_MAX when none).
    int64_t other_bag_min(const Bag& me) const {
        const int slots = std::min(next_slot_.load(std::memory_order_relaxed), kMaxSlots);
        int64_t m = INT64_MAX;
        for (int i = 0; i < slots; ++i) {
            if (&bags_[i] == &me) continue;
            m = std::min(m, bags_[i].pub_min.load(std::memory_order_relaxed));
        }
        return m;
    }
    // Moves every bag item with f <= threshold to the shared list.
    void promote(Bag& bag, int64_t threshold) {
        size_t kept = 0, moved = 0;
        int64_t new_min = INT64_MAX;
        for (size_t i = 0; i < bag.items.size(); ++i) {
            if (bag.items[i].f_value <= threshold) { bag.adds.push_back(bag.items[i]); ++moved; }
            else { new_min = std::min(new_min, bag.items[i].f_value); bag.items[kept++] = bag.items[i]; }
        }
        bag.items.resize(kept);
        bag.min_f = new_min;
        bag.pub_min.store(new_min, std::memory_order_relaxed);
        flush_adds(bag);
        if (list_diagnostics().load(std::memory_order_relaxed)) {
            ListOpStats& stats = tls_list_stats();
            stats.promotes++;
            stats.promoted_items += moved;
            if (moved > stats.promoted_max) stats.promoted_max = moved;
            publish(bag);
        }
    }
    void publish(Bag& bag) {
        bag.pub_prefetched.store(uint32_t(bag.prefetched.size()), std::memory_order_relaxed);
        bag.pub_items.store(uint32_t(bag.items.size()), std::memory_order_relaxed);
    }
    // --diag: called when a thread found no work. Every 8th call, sums what the OTHER threads hold privately (prefetch buffers and
    // beyond-window bags) next to the shared list size: "work exists but this thread cannot see it" versus "nothing is left".
    void sample_holders() {
        Bag& me = my_bag();
        if ((me.idle_polls++ & 7u) != 0) return;
        const int slots = std::min(next_slot_.load(std::memory_order_relaxed), kMaxSlots);
        uint64_t hoard = 0, held = 0;
        for (int i = 0; i < slots; ++i) {
            if (&bags_[i] == &me) continue;
            hoard += bags_[i].pub_prefetched.load(std::memory_order_relaxed);
            held += bags_[i].pub_items.load(std::memory_order_relaxed);
        }
        ListOpStats& stats = tls_list_stats();
        stats.idle_samples++;
        stats.hoard_sum += hoard;
        if (hoard > stats.hoard_max) stats.hoard_max = hoard;
        stats.held_sum += held;
        stats.shared_sum += list_.size_approx();
    }

    List list_;
    const uint64_t instance_;
    std::unique_ptr<PaddedBag[]> bags_;
    std::atomic<int> next_slot_{0};
    std::atomic<int64_t> threshold_{kUnset};
    int64_t window_ = 1;
};

}  // namespace astar
