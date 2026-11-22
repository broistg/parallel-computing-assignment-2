#include "ConcurrentList.h"
#include "FineGrained.h"
#include "Optimistic.h"
#include <iostream>
#include <thread>
#include <vector>

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

    // Tạo 4 luồng thêm dữ liệu đồng thời id từ 1->40
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&list, t]() {
            for (int i = 1; i <= 10; ++i) {
                int id = t * 10 + i;
                list->add(id, "User_" + std::to_string(id));
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    std::cout << "Contains ID 15: " << std::boolalpha << list->contains(15) << "\n";
    std::cout << "Remove ID 15:   " << list->remove(15) << "\n";
    std::cout << "Contains ID 15: " << list->contains(15) << "\n";

    return 0;
}