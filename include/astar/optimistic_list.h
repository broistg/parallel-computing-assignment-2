#pragma once
// Optimistic sorted list implementing the frozen open-list interface (openlist.h): the group's Section-2 "optimistic" variant
// (ex2/src/strategies/Optimistic.h) adapted to OpenItem keys and the lock-template convention of CoarseList / FineList.
//
// add / remove / contains walk from the head WITHOUT taking any lock, then lock the two nodes found (pred, curr) and VALIDATE that
// pred is still reachable from the head and still points to curr; if not, they unlock and retry.  pop_min locks the head and the
// first node only (no walk).  add_batch / pop_batch (optional, used by WindowedList) group several items per lock round.  The sentinel head carries no item; the end of the list is nullptr.
//
// Memory: a thread walking without locks can be standing on a node that another thread unlinks, so unlinked nodes are NOT freed
// immediately (the textbook version leaves this to a garbage collector).  They are pushed onto a retired list and freed when the
// list is destroyed.  Every search creates its list per query, so the extra memory is bounded by the number of removals in one
// query (72 bytes per removed item with std::mutex, 384 with the cache-line padded TTAS lock) and is released at the end of the query.  Every `next` pointer is a std::atomic: the
// unlocked walk reads it while writers (holding the node's lock) change it.
#include <atomic>
#include <chrono>
#include <vector>

#include "openlist.h"

namespace astar {

template <class Lock>
class OptimisticList {
    struct Node {
        OpenItem item;
        std::atomic<Node*> next;
        Lock lock;
        Node* retired_next;  // only used after unlinking
        Node(const OpenItem& x, Node* n) : item(x), next(n), lock(), retired_next(nullptr) {}
    };

public:
    OptimisticList() : head_(new Node(OpenItem{0, 0, 0}, nullptr)) {}
    ~OptimisticList() {
        for (Node* n = head_; n;) {
            Node* next = n->next.load(std::memory_order_relaxed);
            delete n;
            n = next;
        }
        for (Node* n = retired_.load(std::memory_order_relaxed); n;) {
            Node* next = n->retired_next;
            delete n;
            n = next;
        }
    }
    OptimisticList(const OptimisticList&) = delete;
    OptimisticList& operator=(const OptimisticList&) = delete;

    bool add(const OpenItem& x) {
        Node* fresh = new Node(x, nullptr);  // allocated outside any critical section
        for (;;) {
            uint64_t steps = 0;
            Node *pred, *curr;
            find(x, pred, curr, steps);
            lock_pair(pred, curr);
            if (validate(pred, curr)) {
                if (curr && curr->item == x) {
                    unlock_pair(pred, curr);
                    delete fresh;
                    return false;
                }
                fresh->next.store(curr, std::memory_order_relaxed);
                record_insert_profile(steps, steps, size_.load(std::memory_order_relaxed));
                pred->next.store(fresh, std::memory_order_release);
                size_.fetch_add(1, std::memory_order_relaxed);
                unlock_pair(pred, curr);
                return true;
            }
            unlock_pair(pred, curr);
        }
    }

    bool remove(const OpenItem& x) {
        for (;;) {
            uint64_t steps = 0;
            Node *pred, *curr;
            find(x, pred, curr, steps);
            lock_pair(pred, curr);
            if (validate(pred, curr)) {
                if (curr && curr->item == x) {
                    pred->next.store(curr->next.load(std::memory_order_relaxed), std::memory_order_release);
                    size_.fetch_sub(1, std::memory_order_relaxed);
                    unlock_pair(pred, curr);
                    retire(curr);
                    return true;
                }
                unlock_pair(pred, curr);
                return false;
            }
            unlock_pair(pred, curr);
        }
    }

    bool contains(const OpenItem& x) {
        for (;;) {
            uint64_t steps = 0;
            Node *pred, *curr;
            find(x, pred, curr, steps);
            lock_pair(pred, curr);
            if (validate(pred, curr)) {
                bool found = curr && curr->item == x;
                unlock_pair(pred, curr);
                return found;
            }
            unlock_pair(pred, curr);
        }
    }

    bool pop_min(OpenItem& out) {
        const bool d = diag();
        const uint64_t t0 = d ? now_ns() : 0;
        head_->lock.lock();
        const uint64_t t1 = d ? now_ns() : 0;
        Node* first = head_->next.load(std::memory_order_relaxed);  // head's next changes only under head's lock
        if (!first) {
            head_->lock.unlock();
            if (d) {
                ListOpStats& st = tls_list_stats();
                st.opt_pops++; st.opt_pop_head_wait_ns += t1 - t0; st.opt_pop_hold_ns += now_ns() - t1;
            }
            return false;
        }
        first->lock.lock();
        const uint64_t t2 = d ? now_ns() : 0;
        head_->next.store(first->next.load(std::memory_order_relaxed), std::memory_order_release);
        size_.fetch_sub(1, std::memory_order_relaxed);
        first->lock.unlock();
        head_->lock.unlock();
        if (d) {
            ListOpStats& st = tls_list_stats();
            st.opt_pops++; st.opt_pop_head_wait_ns += t1 - t0; st.opt_pop_first_wait_ns += t2 - t1; st.opt_pop_hold_ns += now_ns() - t1;
        }
        out = first->item;
        retire(first);
        return true;
    }

    // OPTIONAL batch operations (see CoarseList): used by WindowedList when its batch size is > 1.
    // add_batch: items sorted ascending; returns how many were inserted (equal items already present are skipped).  Consecutive items
    // that fall between the same two nodes are inserted under ONE lock-and-validate round (one chain published by a single store);
    // the next group starts with a new lock-free walk.
    size_t add_batch(const OpenItem* items, size_t n) {
        std::vector<Node*> fresh(n, nullptr);  // allocated outside any critical section; unused ones are deleted below
        for (size_t i = 0; i < n; ++i) fresh[i] = new Node(items[i], nullptr);
        size_t inserted = 0, i = 0;
        while (i < n) {
            uint64_t steps = 0;
            Node *pred, *curr;
            find(items[i], pred, curr, steps);
            lock_pair(pred, curr);
            if (!validate(pred, curr)) {
                unlock_pair(pred, curr);
                continue;
            }
            // Under both locks: pred.item < items[i] <= curr.item, so at least items[i] is consumed in this round.
            Node *chain_first = nullptr, *chain_last = nullptr;
            size_t group = 0;
            for (; i < n; ++i) {
                if (i > 0 && items[i] == items[i - 1]) { delete fresh[i]; fresh[i] = nullptr; continue; }
                if (curr && !(items[i] < curr->item)) {
                    if (items[i] == curr->item) { delete fresh[i]; fresh[i] = nullptr; continue; }
                    break;  // beyond curr: next round
                }
                if (chain_last) chain_last->next.store(fresh[i], std::memory_order_relaxed);
                else chain_first = fresh[i];
                chain_last = fresh[i];
                fresh[i] = nullptr;
                ++group;
            }
            if (group) {
                chain_last->next.store(curr, std::memory_order_relaxed);
                record_insert_profile(steps, steps, size_.load(std::memory_order_relaxed));
                pred->next.store(chain_first, std::memory_order_release);  // publishes the whole chain
                size_.fetch_add(group, std::memory_order_relaxed);
                inserted += group;
            }
            unlock_pair(pred, curr);
        }
        return inserted;
    }
    // pop_batch: removes up to k (<= 64) smallest items, ascending into out; returns how many.  The head and the taken nodes are
    // locked in list order (the same order as every other operation); the taken nodes are retired with ONE push.
    size_t pop_batch(OpenItem* out, size_t k) {
        Node* taken[64];
        if (k > 64) k = 64;
        const bool d = diag();
        const uint64_t t0 = d ? now_ns() : 0;
        head_->lock.lock();
        const uint64_t t1 = d ? now_ns() : 0;
        size_t count = 0;
        Node* last = head_;
        for (Node* q = head_->next.load(std::memory_order_relaxed); q && count < k; q = q->next.load(std::memory_order_relaxed)) {
            q->lock.lock();  // q's next pointer is read only after q is locked (hand-over-hand, from the head)
            taken[count++] = q;
            last = q;
        }
        if (count) {
            head_->next.store(last->next.load(std::memory_order_relaxed), std::memory_order_release);
            size_.fetch_sub(count, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < count; ++i) taken[i]->lock.unlock();
        head_->lock.unlock();
        if (d) {
            ListOpStats& st = tls_list_stats();
            st.opt_pops += count ? 1 : 0; st.opt_pop_head_wait_ns += t1 - t0; st.opt_pop_hold_ns += now_ns() - t1;
        }
        for (size_t i = 0; i < count; ++i) out[i] = taken[i]->item;
        if (count) retire_chain(taken, count);
        return count;
    }

    size_t size_approx() const { return size_.load(std::memory_order_relaxed); }

private:
    // Lock-free walk to the first node whose item is >= x (curr, may be nullptr) and its predecessor.
    void find(const OpenItem& x, Node*& pred, Node*& curr, uint64_t& steps) const {
        pred = head_;
        curr = pred->next.load(std::memory_order_acquire);
        while (curr && curr->item < x) {
            pred = curr;
            curr = curr->next.load(std::memory_order_acquire);
            ++steps;
        }
        if (diag()) {
            ListOpStats& st = tls_list_stats();
            st.opt_attempts++;
            st.opt_find_steps += steps;
            if (steps <= 1) st.opt_near_head++;  // pred is the head or the first node
        }
    }
    // pred is validated if it is still in the list and still points to curr.  Called with pred and curr locked.
    bool validate(Node* pred, Node* curr) const {
        uint64_t walked = 0;
        bool ok = false;
        for (Node* n = head_; n; n = n->next.load(std::memory_order_acquire)) {
            ++walked;
            if (n == pred) { ok = pred->next.load(std::memory_order_acquire) == curr; break; }
            if (pred != head_ && n != head_ && pred->item < n->item) break;  // walked past where pred would be
        }
        if (diag()) {
            ListOpStats& st = tls_list_stats();
            st.opt_validate_steps += walked;
            if (!ok) st.opt_retries++;
        }
        return ok;
    }
    static bool diag() { return list_diagnostics().load(std::memory_order_relaxed); }
    static uint64_t now_ns() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static void lock_pair(Node* pred, Node* curr) {  // list order: pred before curr
        const bool d = diag();
        const uint64_t t0 = d ? now_ns() : 0;
        pred->lock.lock();
        if (curr) curr->lock.lock();
        if (d) tls_list_stats().opt_pair_wait_ns += now_ns() - t0;
    }
    static void unlock_pair(Node* pred, Node* curr) {
        if (curr) curr->lock.unlock();
        pred->lock.unlock();
    }
    // Pushes nodes[0..count) onto the retired stack with one compare-exchange; their own next pointers stay untouched.
    void retire_chain(Node** nodes, size_t count) {
        for (size_t i = 0; i + 1 < count; ++i) nodes[i]->retired_next = nodes[i + 1];
        Node* top = retired_.load(std::memory_order_relaxed);
        nodes[count - 1]->retired_next = top;
        uint64_t failed = 0;
        while (!retired_.compare_exchange_weak(top, nodes[0], std::memory_order_release, std::memory_order_relaxed)) {
            nodes[count - 1]->retired_next = top;
            ++failed;
        }
        if (failed && diag()) tls_list_stats().opt_retire_cas_fail += failed;
    }
    void retire(Node* n) {  // push-only Treiber stack; nodes are freed in the destructor
        Node* top = retired_.load(std::memory_order_relaxed);
        n->retired_next = top;
        uint64_t failed = 0;
        while (!retired_.compare_exchange_weak(top, n, std::memory_order_release, std::memory_order_relaxed)) {
            n->retired_next = top;  // top was refreshed by the failed compare-exchange
            ++failed;
        }
        if (failed && diag()) tls_list_stats().opt_retire_cas_fail += failed;
    }

    Node* head_;  // sentinel
    std::atomic<Node*> retired_{nullptr};
    std::atomic<size_t> size_{0};
};

}  // namespace astar
