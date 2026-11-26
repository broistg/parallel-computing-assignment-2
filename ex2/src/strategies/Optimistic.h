#pragma once
#include "../IListStrategy.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>

// Optimistic synchronization: walk the list without locks, lock pred and curr, validate (pred still reachable from head and
// still pointing to curr), retry on failure.
//
// Two changes against the first version (found when the list was merged into the A* framework, see include/astar/optimistic_list.h):
//  * `next` is a std::atomic: the unlocked walk reads it while a writer changes it (a data race otherwise, flagged by ThreadSanitizer).
//  * remove() no longer deletes the unlinked node: a thread walking without locks may be standing on it (the textbook version relies
//    on a garbage collector).  Unlinked nodes go onto a retired list that is freed in the destructor.

class OptimisticdNode {
public:
    int id;
    std::string name;
    std::atomic<OptimisticdNode*> next;
    std::mutex mutexlock;
    OptimisticdNode* retired_next;  // only used after unlinking

    OptimisticdNode(int id, std::string name, OptimisticdNode* next = nullptr)
        : id(id), name(std::move(name)), next(next), retired_next(nullptr) {}
};

class OptimisticStrategy : public IListStrategy {
public:
    OptimisticdNode* head;
    OptimisticdNode* tail;

    OptimisticStrategy() : retired(nullptr) {
        tail = new OptimisticdNode(kKeyMax, "TAIL_SENTINEL", nullptr);
        head = new OptimisticdNode(kKeyMin, "HEAD_SENTINEL", tail);
    }

    ~OptimisticStrategy() override {
        OptimisticdNode* curr = head;
        while (curr != nullptr) {
            OptimisticdNode* temp = curr;
            curr = curr->next.load(std::memory_order_relaxed);
            delete temp;
        }
        curr = retired.load(std::memory_order_relaxed);
        while (curr != nullptr) {
            OptimisticdNode* temp = curr;
            curr = curr->retired_next;
            delete temp;
        }
    }

    bool add(int id, const std::string& name) override {
        if (id <= kKeyMin || id >= kKeyMax) return false;  // sentinel keys are reserved
        while (true) {
            OptimisticdNode* pred;
            OptimisticdNode* curr;
            find(id, pred, curr);
            std::unique_lock<std::mutex> pred_lock(pred->mutexlock);
            std::unique_lock<std::mutex> curr_lock(curr->mutexlock);
            if (validate(pred, curr)) {
                if (curr->id == id) {
                    return false;
                }
                OptimisticdNode* new_node = new OptimisticdNode(id, name, curr);
                pred->next.store(new_node, std::memory_order_release);
                return true;
            }
        }
    }

    bool remove(int id) override {
        if (id <= kKeyMin || id >= kKeyMax) return false;  // never unlink a sentinel
        while (true) {
            OptimisticdNode* pred;
            OptimisticdNode* curr;
            find(id, pred, curr);
            std::unique_lock<std::mutex> pred_lock(pred->mutexlock);
            std::unique_lock<std::mutex> curr_lock(curr->mutexlock);
            if (validate(pred, curr)) {
                if (curr->id == id) {
                    pred->next.store(curr->next.load(std::memory_order_relaxed), std::memory_order_release);
                    retire(curr);
                    return true;
                }
                return false;
            }
        }
    }

    bool contains(int id) override {
        if (id <= kKeyMin || id >= kKeyMax) return false;
        while (true) {
            OptimisticdNode* pred;
            OptimisticdNode* curr;
            find(id, pred, curr);
            std::unique_lock<std::mutex> pred_lock(pred->mutexlock);
            std::unique_lock<std::mutex> curr_lock(curr->mutexlock);
            if (validate(pred, curr)) {
                return curr->id == id;
            }
        }
    }

    bool validate(OptimisticdNode* pred, OptimisticdNode* curr) {
        OptimisticdNode* temp = head;
        while (temp->id <= pred->id) {
            if (temp == pred) return (pred->next.load(std::memory_order_acquire) == curr);
            temp = temp->next.load(std::memory_order_acquire);
        }
        return false;
    }

    void print() override {
        OptimisticdNode* temp = head;
        while (temp != nullptr) {
            std::cout << temp->id << ", ";
            temp = temp->next.load(std::memory_order_acquire);
        }
        std::cout << "\n";
    }

    bool validateStructure(int expected_size = -1) override {
        // 1. head and tail are never null
        if (head == nullptr || tail == nullptr) {
            return false;
        }
        // 2. the sentinels are unchanged
        if (head->id != kKeyMin || tail->id != kKeyMax || tail->next.load() != nullptr) {
            return false;
        }
        // 3. no cycle (slow / fast pointers)
        OptimisticdNode* slow = head;
        OptimisticdNode* fast = head;
        while (fast != nullptr && fast->next.load() != nullptr) {
            slow = slow->next.load();
            fast = fast->next.load()->next.load();
            if (slow == fast) {
                return false;
            }
        }
        // 4. strictly increasing keys from head to tail, connected
        OptimisticdNode* curr = head;
        int actual_size = 0;
        while (curr != tail) {
            OptimisticdNode* nxt = curr->next.load();
            if (nxt == nullptr) {
                return false;
            }
            if (curr->id >= nxt->id) {
                return false;
            }
            if (curr != head) {
                actual_size++;
            }
            curr = nxt;
        }
        // 5. element count (if given)
        if (expected_size != -1 && actual_size != expected_size) {
            return false;
        }
        return true;
    }

private:
    std::atomic<OptimisticdNode*> retired;

    void find(int id, OptimisticdNode*& pred, OptimisticdNode*& curr) {
        pred = head;
        curr = head->next.load(std::memory_order_acquire);
        while (curr->id < id) {
            pred = curr;
            curr = curr->next.load(std::memory_order_acquire);
        }
    }

    void retire(OptimisticdNode* n) {  // push-only stack, freed in the destructor
        OptimisticdNode* top = retired.load(std::memory_order_relaxed);
        do {
            n->retired_next = top;
        } while (!retired.compare_exchange_weak(top, n, std::memory_order_release, std::memory_order_relaxed));
    }
};
