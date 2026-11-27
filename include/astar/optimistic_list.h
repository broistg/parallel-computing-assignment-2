#pragma once
// Optimistic sorted list implementing the frozen open-list interface (openlist.h): the group's Section-2 "optimistic" variant
// (ex2/src/strategies/Optimistic.h) adapted to OpenItem keys and the lock-template convention of CoarseList / FineList.
//
// add / remove / contains walk from the head WITHOUT taking any lock, then lock the two nodes found (pred, curr) and VALIDATE that
// pred is still reachable from the head and still points to curr; if not, they unlock and retry.  pop_min locks the head and the
// first node only (no walk).  The sentinel head carries no item; the end of the list is nullptr.
//
// Memory: a thread walking without locks can be standing on a node that another thread unlinks, so unlinked nodes are NOT freed
// immediately (the textbook version leaves this to a garbage collector).  They are pushed onto a retired list and freed when the
// list is destroyed.  Every search creates its list per query, so the extra memory is bounded by the number of removals in one
// query (72 bytes per removed item with std::mutex, 384 with the cache-line padded TTAS lock) and is released at the end of the query.  Every `next` pointer is a std::atomic: the
// unlocked walk reads it while writers (holding the node's lock) change it.
#include <atomic>

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
        head_->lock.lock();
        Node* first = head_->next.load(std::memory_order_relaxed);  // head's next changes only under head's lock
        if (!first) {
            head_->lock.unlock();
            return false;
        }
        first->lock.lock();
        head_->next.store(first->next.load(std::memory_order_relaxed), std::memory_order_release);
        size_.fetch_sub(1, std::memory_order_relaxed);
        first->lock.unlock();
        head_->lock.unlock();
        out = first->item;
        retire(first);
        return true;
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
    }
    // pred is validated if it is still in the list and still points to curr.  Called with pred and curr locked.
    bool validate(Node* pred, Node* curr) const {
        for (Node* n = head_; n; n = n->next.load(std::memory_order_acquire)) {
            if (n == pred) return pred->next.load(std::memory_order_acquire) == curr;
            if (pred != head_ && n != head_ && pred->item < n->item) return false;  // walked past where pred would be
        }
        return false;
    }
    static void lock_pair(Node* pred, Node* curr) {  // list order: pred before curr
        pred->lock.lock();
        if (curr) curr->lock.lock();
    }
    static void unlock_pair(Node* pred, Node* curr) {
        if (curr) curr->lock.unlock();
        pred->lock.unlock();
    }
    void retire(Node* n) {  // push-only Treiber stack; nodes are freed in the destructor
        Node* top = retired_.load(std::memory_order_relaxed);
        do {
            n->retired_next = top;
        } while (!retired_.compare_exchange_weak(top, n, std::memory_order_release, std::memory_order_relaxed));
    }

    Node* head_;  // sentinel
    std::atomic<Node*> retired_{nullptr};
    std::atomic<size_t> size_{0};
};

}  // namespace astar
