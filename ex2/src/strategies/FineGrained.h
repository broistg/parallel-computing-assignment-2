#pragma once
#include "../IListStrategy.h"
#include <mutex>
#include <string>
#include <iostream>

class FineGrainedNode {
public:
    int id;
    std::string name;
    FineGrainedNode* next;
    std::mutex mutexlock;

    FineGrainedNode (int id, std::string name, FineGrainedNode* next = nullptr) {
        this->id = id;
        this->name = name;
        this->next = next;
    }
};

class FineGrainedStrategy : public IListStrategy {
public:
    FineGrainedNode* head;
    FineGrainedNode* tail;

    FineGrainedStrategy() {
        tail = new FineGrainedNode(kKeyMax, "TAIL_SENTINEL", nullptr);
        head = new FineGrainedNode(kKeyMin, "HEAD_SENTINEL", tail);
    }

    ~FineGrainedStrategy() override {
        FineGrainedNode* curr = head;
        while (curr != nullptr) {
            FineGrainedNode* temp = curr;
            curr = curr->next;
            delete temp;
        }
    }

    bool add(int id, const std::string& name) override {
        if (id <= kKeyMin || id >= kKeyMax) return false;  // sentinel keys are reserved
        std::unique_lock<std::mutex> pred_lock(head->mutexlock);    // tương đương head.lock()
        FineGrainedNode* pred = head;
        FineGrainedNode* curr = head->next;
        std::unique_lock<std::mutex> curr_lock(curr->mutexlock);
        // duyệt qua list để tìm node
        while (curr->id < id) {
            pred = curr;
            pred_lock = std::move(curr_lock);
            curr = curr->next;
            curr_lock = std::unique_lock<std::mutex>(curr->mutexlock);
        }
        // nếu đã có thì trả về false
        if (curr->id == id) {
            return false;
        }
        // tạo node mới và nối vào list
        FineGrainedNode* new_node = new FineGrainedNode(id, name, curr);
        pred->next = new_node;
        return true;
    }

    bool remove(int id) override {
        if (id <= kKeyMin || id >= kKeyMax) return false;  // never unlink a sentinel
        std::unique_lock<std::mutex> pred_lock(head->mutexlock);
        FineGrainedNode* pred = head;
        FineGrainedNode* curr = pred->next;
        std::unique_lock<std::mutex> curr_lock(curr->mutexlock);
        // duyệt qua list để tìm node
        while (curr->id < id) {
            pred = curr;
            pred_lock = std::move(curr_lock);
            curr = curr->next;
            curr_lock = std::unique_lock<std::mutex>(curr->mutexlock);
        }
        if (curr->id == id) {
            pred->next = curr->next;
            curr_lock.unlock();
            delete curr;
            return true;
        }
        // nếu không tìm thấy thì trả về false
        return false;
    }

    bool contains(int id) override {
        if (id <= kKeyMin || id >= kKeyMax) return false;
        std::unique_lock<std::mutex> pred_lock(head->mutexlock);
        FineGrainedNode* pred = head;
        FineGrainedNode* curr = pred->next;
        std::unique_lock<std::mutex> curr_lock(curr->mutexlock);
        // duyệt qua list để tìm node
        while (curr->id < id) {
            pred = curr;
            pred_lock = std::move(curr_lock);
            curr = curr->next;
            curr_lock = std::unique_lock<std::mutex>(curr->mutexlock);
        }
        return curr->id == id;
    }

    void print() override {
        FineGrainedNode* temp = head;
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
        if (head->id != kKeyMin || tail->id != kKeyMax || tail->next != nullptr) {
            return false;
        }
        // 3. Kiểm tra vòng lặp (Cycle) bằng hai con trỏ slow/fast
        FineGrainedNode* slow = head;
        FineGrainedNode* fast = head;
        while (fast != nullptr && fast->next != nullptr) {
            slow = slow->next;
            fast = fast->next->next;
            if (slow == fast) {
                return false;
            }
        }
        // 4. Duyệt từ head tới tail kiểm tra thứ tự khóa và tính liên thông
        FineGrainedNode* curr = head;
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