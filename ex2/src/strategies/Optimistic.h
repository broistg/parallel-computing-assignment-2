#pragma once
#include "../IListStrategy.h"
#include <mutex>
#include <string>
#include <iostream>

#define INT_MAX 50
#define INT_MIN 0

class OptimisticdNode {
public:
    int id;
    std::string name;
    OptimisticdNode* next;
    std::mutex mutexlock;

    OptimisticdNode (int id, std::string name, OptimisticdNode* next = nullptr) {
        this->id = id;
        this->name = name;
        this->next = next;
    }
};

class OptimisticStrategy : public IListStrategy {
public:
    OptimisticdNode* head;
    OptimisticdNode* tail;

    OptimisticStrategy() {
        tail = new OptimisticdNode(INT_MAX, "TAIL_SENTINEL", nullptr);
        head = new OptimisticdNode(INT_MIN, "HEAD_SENTINEL", tail);
    }

    ~OptimisticStrategy() override {
        OptimisticdNode* curr = head;
        while (curr != nullptr) {
            OptimisticdNode* temp = curr;
            curr = curr->next;
            delete temp;
        }
    }

    bool add(int id, const std::string& name) override {
        while(true) {
            OptimisticdNode* pred = head;
            OptimisticdNode* curr = head->next;
            // duyệt qua list để tìm node
            while (curr->id < id) {
                pred = curr;
                curr = curr->next;
            }
            std::unique_lock<std::mutex> pred_lock(pred->mutexlock);   // tương đương pred.lock()
            std::unique_lock<std::mutex> curr_lock(curr->mutexlock);   // tương đương curr.lock()
            // kiểm tra node còn tồn tại
            if (validate(pred, curr)) {
                // nếu đã có thì trả về false
                if (curr->id == id) {
                    return false;
                }
                // tạo node mới và nối vào list
                OptimisticdNode* new_node = new OptimisticdNode(id, name, curr);
                pred->next = new_node;
                return true;
            }
            else {
                pred_lock.unlock();
                curr_lock.unlock();
            }
        }
    }

    bool remove(int id) override {
        while(true) {
            OptimisticdNode* pred = head;
            OptimisticdNode* curr = pred->next;
            // duyệt qua list để tìm node
            while (curr->id < id) {
                pred = curr;
                curr = curr->next;
            }
            std::unique_lock<std::mutex> pred_lock(pred->mutexlock);   // tương đương pred.lock()
            std::unique_lock<std::mutex> curr_lock(curr->mutexlock);   // tương đương curr.lock()
            // kiểm tra node còn tồn tại
            if (validate(pred, curr)) {
                if (curr->id == id) {
                    pred->next = curr->next;
                    curr_lock.unlock();
                    delete curr;
                    return true;
                }
                // nếu không tìm thấy thì trả về false
                return false;
            }
            else {
                pred_lock.unlock();
                curr_lock.unlock();
            }
        }
    }

    bool contains(int id) override {
        while (true) {
            OptimisticdNode* pred = head;
            OptimisticdNode* curr = pred->next;
            // duyệt qua list để tìm node
            while (curr->id < id) {
                pred = curr;
                curr = curr->next;
            }
            std::unique_lock<std::mutex> pred_lock(pred->mutexlock);   // tương đương pred.lock()
            std::unique_lock<std::mutex> curr_lock(curr->mutexlock);   // tương đương curr.lock()
            if (validate(pred, curr)) {
                return curr->id == id;
            }
            else {
                pred_lock.unlock();
                curr_lock.unlock();
            }
        }
    }
    
    bool validate(OptimisticdNode* pred, OptimisticdNode* curr) {
        OptimisticdNode* temp = head;
        while (temp->id <= pred->id) {
            if (temp == pred) return (pred->next == curr);
            temp = temp->next;
        }
        return false;
    }

    void print() override {
        OptimisticdNode* temp = head;
        while (temp != nullptr) {
            std::cout << temp->id << ", ";
            temp = temp->next;
        }
        std::cout <<"\n";
    }

    bool validateStructure(int expected_size = -1) override {
        // 1. Kiểm tra lính canh head và tail không bao giờ null
        if (head == nullptr || tail == nullptr) {
            return false;
        }
        // 2. Kiểm tra lính canh head và tail không bị thay đổi
        if (head->id != INT_MIN || tail->id != INT_MAX || tail->next != nullptr) {
            return false;
        }
        // 3. Kiểm tra vòng lặp (Cycle) bằng hai con trỏ slow/fast
        OptimisticdNode* slow = head;
        OptimisticdNode* fast = head;
        while (fast != nullptr && fast->next != nullptr) {
            slow = slow->next;
            fast = fast->next->next;
            if (slow == fast) {
                return false;
            }
        }
        // 4. Duyệt từ head tới tail kiểm tra thứ tự khóa và tính liên thông
        OptimisticdNode* curr = head;
        int actual_size = 0;

        while (curr != tail) {
            if (curr->next == nullptr) {
                return false;
            }
            if (curr->id >= curr->next->id) {
                return false;
            }
            if (curr != head) {
                actual_size++;
            }
            curr = curr->next;
        }
        // 5. Kiểm tra số lượng phần tử thực tế (nếu có truyền vào expected_size)
        if (expected_size != -1 && actual_size != expected_size) {
            return false;
        }
        return true;
    }
};