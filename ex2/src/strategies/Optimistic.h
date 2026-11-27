#pragma once
#include "../IListStrategy.h"
#include <atomic>
#include <iostream>
#include <mutex>
#include <string>

// Đồng bộ hóa lạc quan (Optimistic): duyệt list không khóa, khóa pred và curr, kiểm tra lại (validate: pred vẫn tới được từ head và
// vẫn trỏ tới curr), nếu sai thì thử lại.
//
// Hai thay đổi so với bản đầu (phát hiện khi ghép list vào khung A*, xem include/astar/optimistic_list.h):
//  * `next` là std::atomic: lượt duyệt không khóa đọc nó trong khi luồng khác đang ghi (nếu không là data race, bị ThreadSanitizer báo).
//  * remove() không còn delete node vừa gỡ: một luồng đang duyệt không khóa có thể đang đứng trên node đó (bản giáo trình dựa vào
//    bộ gom rác). Các node đã gỡ được đưa vào danh sách retired và chỉ giải phóng ở destructor.

class OptimisticdNode {
public:
    int id;
    std::string name;
    std::atomic<OptimisticdNode*> next;
    std::mutex mutexlock;
    OptimisticdNode* retired_next;  // chỉ dùng sau khi node đã bị gỡ

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
        if (id <= kKeyMin || id >= kKeyMax) return false;  // khóa lính canh được dành riêng, không cho thêm
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
        if (id <= kKeyMin || id >= kKeyMax) return false;  // không bao giờ gỡ lính canh
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
        // 1. Kiểm tra lính canh head và tail không bao giờ null
        if (head == nullptr || tail == nullptr) {
            return false;
        }
        // 2. Kiểm tra lính canh head và tail không bị thay đổi
        if (head->id != kKeyMin || tail->id != kKeyMax || tail->next.load() != nullptr) {
            return false;
        }
        // 3. Kiểm tra vòng lặp (Cycle) bằng hai con trỏ slow/fast
        OptimisticdNode* slow = head;
        OptimisticdNode* fast = head;
        while (fast != nullptr && fast->next.load() != nullptr) {
            slow = slow->next.load();
            fast = fast->next.load()->next.load();
            if (slow == fast) {
                return false;
            }
        }
        // 4. Duyệt từ head tới tail kiểm tra thứ tự khóa tăng chặt và tính liên thông
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
        // 5. Kiểm tra số lượng phần tử thực tế (nếu có truyền vào expected_size)
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

    void retire(OptimisticdNode* n) {  // ngăn xếp chỉ thêm, giải phóng ở destructor
        OptimisticdNode* top = retired.load(std::memory_order_relaxed);
        do {
            n->retired_next = top;
        } while (!retired.compare_exchange_weak(top, n, std::memory_order_release, std::memory_order_relaxed));
    }
};
