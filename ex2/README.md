# Section 2: Concurrent sorted linked list (Strategy design pattern)

A concurrent set implemented as a sorted linked list in C++ with a runtime-selectable synchronisation strategy
(Strategy design pattern). Two variants of the assignment's list are implemented: **fine-grained locking**
(hand-over-hand, one mutex per node) and **optimistic synchronisation** (lock-free walk, lock two nodes, validate, retry).
Each variant has `add`, `remove`, `contains`, `print` and `validateStructure` (an offline structural check: sentinels intact, no cycle,
strictly increasing keys, expected element count).

## Source layout
```
ex2/src/
  IListStrategy.h            interface of a strategy; sentinel keys kKeyMin / kKeyMax (user keys: any int strictly between)
  ConcurrentList.h           context class holding the strategy (std::unique_ptr)
  strategies/FineGrained.h   fine-grained synchronisation
  strategies/Optimistic.h    optimistic synchronisation
  main.cpp                   test program: sequential check against std::set, sentinel keys, concurrent warm-up, 8-thread stress
```

## Build and run
Needs a C++14 compiler and pthreads.
```
g++ -std=c++14 -O2 -pthread ex2/src/main.cpp -o ex2_list      # Linux / macOS (WSL2: same)
ex2_list both            # or: fine | optimistic   (no argument: asks on stdin)
```
Native Windows: `g++ -std=c++14 -O2 ex2/src/main.cpp -o ex2_list.exe` or the `ex2_list` target of the CMake build.
`make` and `./tests/run_tests.sh` of the repository build and run it too (`make test-tsan` runs it under ThreadSanitizer).
The program exits with 0 and prints `ALL EX2 CHECKS PASSED` when every check passes.

## Notes on the implementation
* Fine-grained: locks are taken in list order (pred, then curr); a removed node is deleted right after its locks are released, which is
  safe because a node can only be reached through its locked predecessor.
* Optimistic: the walk takes no lock, so a thread may stand on a node that another thread unlinks. `next` pointers are `std::atomic`,
  and unlinked nodes are not deleted but kept on a retired list that is freed in the destructor (the textbook version relies on garbage
  collection). Cost: memory for removed nodes is held until the list is destroyed.
* The same two algorithms also exist in the A* framework, adapted to the open-list interface (`include/astar/fine_list.h`,
  `include/astar/optimistic_list.h`, lock type as a template parameter, optional batch operations), where the report measures them.
