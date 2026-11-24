#include "ConcurrentList.h"
#include "FineGrained.h"
#include "Optimistic.h"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <cassert>
#include <random>

int main(int argc, char *argv[]) {
    std::unique_ptr<ConcurrentList> list = nullptr;
    int strategy_choice = 0;
    
    std::cout << "CHON CHIEN THUAT CONCURRENT LINKED LIST\n";
    std::cout << "1. Finr-Grained Synchronization\n";
    std::cout << "2. Optimistic Synchronization\n";
    std::cout << "Nhap lua chon (1 hoac 2): ";
    std::cin >> strategy_choice;

    if (strategy_choice == 1) {
        list = std::make_unique<ConcurrentList>(std::make_unique<FineGrainedStrategy>());
        std::cout << "-> Da chon Fine-Grained.\n\n";
    } else if (strategy_choice == 2) {
        list = std::make_unique<ConcurrentList>(std::make_unique<OptimisticStrategy>());
        std::cout << "-> Da chon Optimistic.\n\n";
    } else {
        std::cout << "Lua chon khong hop le!\n\n";
        return 0;
    }

    // Warm up tạo 4 luồng thêm dữ liệu đồng thời id từ 1->40 trên khoảng [1, 50]
    std::cout << "===Khởi động nhẹ nhàng===";
    std::vector<std::thread> warmup_threads;
    for (int t = 0; t < 4; ++t) {
        warmup_threads.emplace_back([&list, t]() {
            for (int i = 1; i <= 10; ++i) {
                int id = t * 10 + i;
                list->add(id, "User_" + std::to_string(id));
            }
        });
    }

    for (auto& th : warmup_threads) {
        th.join();
    }

    std::cout << "Contains ID 15: " << std::boolalpha << list->contains(15) << "\n";
    std::cout << "Remove ID 15:   " << list->remove(15) << "\n";
    std::cout << "Contains ID 15: " << list->contains(15) << "\n";
    std::cout << "Add ID 15:      " << list->add(15, "User_15") << "\n";
    std::cout << "Structure validate: " << list->validateStructure() << "\n";

    // Tranh chấp đa luồng cường độ cao (High-Contention Stress Test)
    std::cout << "===Kiểm tra tranh chấp cường độ cao===\n";
    const int NUM_THREADS = 8;
    const int OPS_PER_THREAD = 25000;

    std::vector<std::thread> threads;
    std::atomic<int> net_elements{40};

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&list, &net_elements, t]() {
            std::mt19937 rng(42 + t);
            std::uniform_int_distribution<int> id_dist(1, 50);
            std::uniform_int_distribution<int> action_dist(0, 2);

            for (int i = 1; i <= 10; ++i) {
                int id = id_dist(rng);
                int action = action_dist(rng);

                if (action == 0) {
                    if (list->add(id, "User_" + std::to_string(id))) {
                        net_elements.fetch_add(1, std::memory_order_relaxed);
                    }
                } else if (action == 1) {
                    if (list->remove(id)) {
                        net_elements.fetch_sub(1, std::memory_order_relaxed);
                    }
                } else {
                    list->contains(id);
                }
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    // list->print();
    if(list->validateStructure(net_elements.load())) {
        std::cout << "[PASS] Test: Giu vung thu tu tang dan, khong trung lap, thong suot head->tail sau "
              << (NUM_THREADS * OPS_PER_THREAD) << " thao tac (So node con lai: "
              << net_elements.load() << ").\n";
    }
    else {
        std::cout << "[FAIL] Cau truc danh sach bi hong sau khi chay da luong!\n";
    }

    return 0;
}