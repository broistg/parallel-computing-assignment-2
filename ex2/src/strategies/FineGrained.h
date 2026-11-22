#pragma once
#include "../IListStrategy.h"
#include <mutex>
#include <string>
#include <iostream>

#define INT_MAX 50
#define INT_MIN 0

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
        tail = new FineGrainedNode(INT_MAX, "TAIL_SENTINEL", nullptr);
        head = new FineGrainedNode(INT_MIN, "HEAD_SENTINEL", tail);
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
};