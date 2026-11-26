# Portable convenience Makefile (Linux / WSL2 / macOS).  Native Windows: use CMakeLists.txt.
CXX      ?= g++
LOCK_INC ?= include            # directory that contains lock.h (A1 file); override for a dev stub
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra -pthread
INC       = -Iinclude -I$(LOCK_INC)
BIN       = build

all: $(BIN)/bench $(BIN)/bench_pareto $(BIN)/test_astar $(BIN)/test_pareto $(BIN)/ex2_list

$(BIN)/bench: src/bench_main.cpp include/astar/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@
$(BIN)/bench_pareto: src/bench_pareto.cpp include/astar/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@
$(BIN)/test_pareto: tests/test_pareto.cpp include/astar/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@
$(BIN)/test_pareto_tsan: tests/test_pareto.cpp include/astar/*.h | $(BIN)
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread $(INC) $< -o $@
$(BIN)/test_astar: tests/test_astar.cpp include/astar/*.h | $(BIN)
	$(CXX) $(CXXFLAGS) $(INC) $< -o $@
$(BIN)/test_astar_tsan: tests/test_astar.cpp include/astar/*.h | $(BIN)
	$(CXX) -std=c++17 -O1 -g -fsanitize=thread -pthread $(INC) $< -o $@
# Section 2 standalone program (fine-grained and optimistic lists, Strategy pattern); C++14 is enough for it
$(BIN)/ex2_list: ex2/src/main.cpp ex2/src/*.h ex2/src/strategies/*.h | $(BIN)
	$(CXX) -std=c++14 -O2 -g -Wall -Wextra -pthread $< -o $@
$(BIN)/ex2_list_tsan: ex2/src/main.cpp ex2/src/*.h ex2/src/strategies/*.h | $(BIN)
	$(CXX) -std=c++14 -O1 -g -fsanitize=thread -pthread $< -o $@
$(BIN):
	mkdir -p $(BIN)

test: $(BIN)/test_astar $(BIN)/test_pareto $(BIN)/ex2_list
	python3 tools/make_profile.py config/profile.csv >/dev/null
	$(BIN)/test_astar data/snapshot_synth_small config/profile.csv
	$(BIN)/test_pareto data/snapshot_synth_small config/profile.csv
	$(BIN)/ex2_list both
test-tsan: $(BIN)/test_astar_tsan $(BIN)/test_pareto_tsan $(BIN)/ex2_list_tsan   # about 45 s + 2 min on 2 cores
	$(BIN)/test_astar_tsan data/snapshot_synth_small config/profile.csv
	$(BIN)/test_pareto_tsan data/snapshot_synth_small config/profile.csv
	$(BIN)/ex2_list_tsan both
test-tsan-stress: $(BIN)/test_astar_tsan   # adds the 48k-node stress graph; several minutes (about 6-10 on 2 cores with the real lock.h). Each stage prints its own time.
	python3 tools/synth_graph.py --rows 220 --cols 220 --seed 2 --out data/snapshot_synth_large
	$(BIN)/test_astar_tsan data/snapshot_synth_small config/profile.csv data/snapshot_synth_large
clean:
	rm -rf $(BIN)
.PHONY: all test test-tsan test-tsan-stress clean
