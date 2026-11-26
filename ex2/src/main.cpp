// Standalone test program of the Section-2 lists (fine-grained and optimistic), Strategy design pattern.
//   ./ex2_list fine | optimistic | both     (no argument: ask on stdin, as in the first version)
// Checks: sequential behaviour against std::set, concurrent warm-up, rejection of the sentinel keys, and a high-contention stress
// test (mixed add / remove / contains) after which the structure (sorted, no cycle, sentinels intact) and the element count are verified.
// Exit code 0 = all passed.
#include "ConcurrentList.h"
#include "strategies/FineGrained.h"
#include "strategies/Optimistic.h"
#include <atomic>
#include <iostream>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

static std::unique_ptr<ConcurrentList> make_list(int choice) {
    if (choice == 1) return std::make_unique<ConcurrentList>(std::make_unique<FineGrainedStrategy>());
    return std::make_unique<ConcurrentList>(std::make_unique<OptimisticStrategy>());
}

static int failures = 0;
#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        if (!(cond)) {                                                    \
            ++failures;                                                   \
            std::cout << "[FAIL] " << msg << "\n";                        \
        }                                                                 \
    } while (0)

static void run_checks(int choice, const char* label) {
    std::cout << "=== " << label << " ===\n";
    const int before = failures;

    // 1. sequential behaviour against std::set
    {
        auto list = make_list(choice);
        std::set<int> ref;
        std::mt19937 rng(1);
        for (int i = 0; i < 20000; ++i) {
            int id = int(rng() % 100) + 1;
            switch (rng() % 3) {
                case 0: CHECK(list->add(id, "u") == ref.insert(id).second, "sequential add " << id); break;
                case 1: CHECK(list->remove(id) == (ref.erase(id) > 0), "sequential remove " << id); break;
                default: CHECK(list->contains(id) == (ref.count(id) > 0), "sequential contains " << id); break;
            }
        }
        CHECK(list->validateStructure(int(ref.size())), "sequential structure");
    }

    // 2. sentinel keys are rejected and the sentinels survive
    {
        auto list = make_list(choice);
        CHECK(!list->add(kKeyMin, "x") && !list->add(kKeyMax, "x"), "sentinel keys must not be added");
        CHECK(!list->remove(kKeyMin) && !list->remove(kKeyMax), "sentinel keys must not be removed");
        CHECK(!list->contains(kKeyMin) && !list->contains(kKeyMax), "sentinel keys are not elements");
        CHECK(list->add(0, "zero") && list->add(50, "fifty") && list->contains(50) && list->remove(50), "keys 0 and 50 are ordinary keys");
        CHECK(list->validateStructure(1), "structure after sentinel checks");
    }

    // 3. concurrent warm-up: 4 threads add ids 1..40
    {
        auto list = make_list(choice);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&list, t]() {
                for (int i = 1; i <= 10; ++i) {
                    int id = t * 10 + i;
                    list->add(id, "User_" + std::to_string(id));
                }
            });
        for (auto& th : threads) th.join();
        CHECK(list->contains(15), "contains 15");
        CHECK(list->remove(15), "remove 15");
        CHECK(!list->contains(15), "15 is gone");
        CHECK(list->add(15, "User_15"), "add 15 again");
        CHECK(list->validateStructure(40), "warm-up structure (40 elements)");
    }

    // 4. high-contention stress: 8 threads, mixed operations on keys 1..50
    {
        auto list = make_list(choice);
        const int kThreads = 8, kOps = 25000;
        std::atomic<int> net_elements{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&list, &net_elements, t]() {
                std::mt19937 rng(42 + t);
                std::uniform_int_distribution<int> id_dist(1, 50);
                std::uniform_int_distribution<int> action_dist(0, 2);
                for (int i = 0; i < kOps; ++i) {
                    int id = id_dist(rng);
                    int action = action_dist(rng);
                    if (action == 0) {
                        if (list->add(id, "User_" + std::to_string(id))) net_elements.fetch_add(1, std::memory_order_relaxed);
                    } else if (action == 1) {
                        if (list->remove(id)) net_elements.fetch_sub(1, std::memory_order_relaxed);
                    } else {
                        list->contains(id);
                    }
                }
            });
        for (auto& th : threads) th.join();
        CHECK(list->validateStructure(net_elements.load()),
              "stress structure after " << kThreads * kOps << " operations (expected " << net_elements.load() << " elements)");
    }
    std::cout << (failures == before ? "[PASS] " : "[FAIL] ") << label << "\n";
}

int main(int argc, char* argv[]) {
    std::string mode;
    if (argc > 1) {
        mode = argv[1];
    } else {
        int choice = 0;
        std::cout << "Choose a strategy:\n1. Fine-grained synchronization\n2. Optimistic synchronization\nEnter 1 or 2: ";
        std::cin >> choice;
        mode = choice == 1 ? "fine" : choice == 2 ? "optimistic" : "";
    }
    if (mode == "fine" || mode == "both") run_checks(1, "Fine-grained");
    if (mode == "optimistic" || mode == "both") run_checks(2, "Optimistic");
    if (mode != "fine" && mode != "optimistic" && mode != "both") {
        std::cout << "usage: ex2_list fine | optimistic | both\n";
        return 2;
    }
    std::cout << (failures == 0 ? "ALL EX2 CHECKS PASSED\n" : "EX2 CHECKS FAILED\n");
    return failures == 0 ? 0 : 1;
}
