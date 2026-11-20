#pragma once
// Fine-grained (hand-over-hand / lock-coupling) sorted list implementing the frozen open-list interface (openlist.h).
// Every node has its own lock; a thread holds the locks of two neighbouring nodes while it walks, so threads working on
// different parts of the list proceed in parallel.  add / remove / contains walk from the head; pop_min locks the head and
// the first node only.  The sentinel head carries no item.
//
// Memory: a removed node is freed right after both locks are released.  This is safe here because a node can only be reached
// through its predecessor, and the remover holds the predecessor's lock while unlinking, so no other thread can be waiting
// for (or about to lock) the removed node.
#include <atomic>
#include <vector>

#include "openlist.h"

namespace astar {

template <class Lock>
class FineList {
    struct Node {
        OpenItem item;
        Node* next;
        Lock lock;
    };

public:
    FineList() : head_(new Node{{0, 0, 0}, nullptr, {}}) {}
    ~FineList() {
        while (head_) {
            Node* n = head_->next;
            delete head_;
            head_ = n;
        }
    }
    FineList(const FineList&) = delete;
    FineList& operator=(const FineList&) = delete;

    bool add(const OpenItem& x) {
        Node* fresh = new Node{x, nullptr, {}};  // allocate outside any critical section
        uint64_t steps = 0;
        Node* pred = head_;
        pred->lock.lock();
        Node* curr = pred->next;
        if (curr) curr->lock.lock();
        while (curr && curr->item < x) {
            pred->lock.unlock();
            pred = curr;
            curr = curr->next;
            if (curr) curr->lock.lock();
            ++steps;
        }
        if (curr && curr->item == x) {
            curr->lock.unlock();
            pred->lock.unlock();
            delete fresh;
            return false;
        }
        fresh->next = curr;
        pred->next = fresh;
        record_insert_profile(steps, steps, size_.load(std::memory_order_relaxed));
        size_.fetch_add(1, std::memory_order_relaxed);
        if (curr) curr->lock.unlock();
        pred->lock.unlock();
        return true;
    }

    bool remove(const OpenItem& x) {
        Node* pred = head_;
        pred->lock.lock();
        Node* curr = pred->next;
        if (curr) curr->lock.lock();
        while (curr && curr->item < x) {
            pred->lock.unlock();
            pred = curr;
            curr = curr->next;
            if (curr) curr->lock.lock();
        }
        if (!curr || !(curr->item == x)) {
            if (curr) curr->lock.unlock();
            pred->lock.unlock();
            return false;
        }
        pred->next = curr->next;
        size_.fetch_sub(1, std::memory_order_relaxed);
        curr->lock.unlock();
        pred->lock.unlock();
        delete curr;
        return true;
    }

    bool contains(const OpenItem& x) {
        Node* pred = head_;
        pred->lock.lock();
        Node* curr = pred->next;
        if (curr) curr->lock.lock();
        while (curr && curr->item < x) {
            pred->lock.unlock();
            pred = curr;
            curr = curr->next;
            if (curr) curr->lock.lock();
        }
        bool found = curr && curr->item == x;
        if (curr) curr->lock.unlock();
        pred->lock.unlock();
        return found;
    }

    bool pop_min(OpenItem& out) {
        head_->lock.lock();
        Node* first = head_->next;
        if (!first) {
            head_->lock.unlock();
            return false;
        }
        first->lock.lock();
        head_->next = first->next;
        size_.fetch_sub(1, std::memory_order_relaxed);
        first->lock.unlock();
        head_->lock.unlock();
        out = first->item;
        delete first;
        return true;
    }

    // OPTIONAL batch operations (see CoarseList): used by WindowedList when its batch size is > 1.
    // add_batch: items sorted ascending; ONE hand-over-hand walk inserts all of them; returns how many were inserted.
    size_t add_batch(const OpenItem* items, size_t n) {
        std::vector<Node*> fresh(n);
        for (size_t i = 0; i < n; ++i) fresh[i] = new Node{items[i], nullptr, {}};
        size_t inserted = 0;
        uint64_t steps = 0, walked = 0;
        const uint64_t size_before = size_.load(std::memory_order_relaxed);
        Node* pred = head_;  // locked (always the last node we walked past)
        pred->lock.lock();
        Node* curr = pred->next;  // locked
        if (curr) curr->lock.lock();
        // `tail` is the node whose next pointer we extend: pred, or a node we inserted after pred. Inserted nodes are not locked:
        // nobody can reach them while we hold pred (readers get there only through pred), and locking them would invert the
        // list-order locking discipline (new node locked after its successor).
        Node* tail = pred;
        for (size_t i = 0; i < n; ++i) {
            while (curr && curr->item < items[i]) {
                pred->lock.unlock();  // publishes the nodes inserted behind curr
                pred = curr;
                tail = pred;
                curr = curr->next;
                if (curr) curr->lock.lock();
                ++steps; ++walked;
            }
            if (curr && curr->item == items[i]) { delete fresh[i]; continue; }
            fresh[i]->next = curr;
            tail->next = fresh[i];
            tail = fresh[i];
            record_insert_profile(steps, walked, size_before + inserted);
            steps = 0;
            ++inserted;
        }
        size_.fetch_add(inserted, std::memory_order_relaxed);
        if (curr) curr->lock.unlock();
        pred->lock.unlock();
        return inserted;
    }
    // pop_batch: removes up to k (<= 64) smallest items, ascending into out; returns how many.
    size_t pop_batch(OpenItem* out, size_t k) {
        Node* taken[64];
        if (k > 64) k = 64;
        head_->lock.lock();
        size_t count = 0;
        Node* last = head_;
        for (Node* q = head_->next; q && count < k; q = q->next) {  // lock the taken nodes in list order (same order as every walker)
            q->lock.lock();
            taken[count++] = q;
            last = q;
        }
        if (count) {
            head_->next = last->next;
            size_.fetch_sub(count, std::memory_order_relaxed);
        }
        for (size_t i = 0; i < count; ++i) taken[i]->lock.unlock();
        head_->lock.unlock();
        for (size_t i = 0; i < count; ++i) {
            out[i] = taken[i]->item;
            delete taken[i];
        }
        return count;
    }

    size_t size_approx() const { return size_.load(std::memory_order_relaxed); }

private:
    Node* head_;  // sentinel
    std::atomic<size_t> size_{0};
};

}  // namespace astar
