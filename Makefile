CXX      := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -g -Isrc

BUILD_DIR := build
OBJ_DIR   := $(BUILD_DIR)/obj

CORE_SRCS := $(wildcard src/strata/*.cpp)
CORE_OBJS := $(patsubst src/strata/%.cpp,$(OBJ_DIR)/%.o,$(CORE_SRCS))
CORE_LIB  := $(BUILD_DIR)/libstrata_core.a

.PHONY: all test clean phase1-check

all: $(BUILD_DIR)/test_trivial $(BUILD_DIR)/test_wal $(BUILD_DIR)/test_gorilla $(BUILD_DIR)/test_l0 $(BUILD_DIR)/strata_tool

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: src/strata/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(CORE_LIB): $(CORE_OBJS)
	ar rcs $@ $(CORE_OBJS)

$(BUILD_DIR)/test_trivial: tests/test_trivial.cpp $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $< $(CORE_LIB) -o $@

$(BUILD_DIR)/test_wal: tests/test_wal.cpp $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $< $(CORE_LIB) -o $@

$(BUILD_DIR)/test_gorilla: tests/test_gorilla.cpp $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $< $(CORE_LIB) -o $@

$(BUILD_DIR)/test_l0: tests/test_l0.cpp $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $< $(CORE_LIB) -o $@

$(BUILD_DIR)/strata_tool: tools/strata_tool.cpp $(CORE_LIB)
	$(CXX) $(CXXFLAGS) $< $(CORE_LIB) -o $@

test: all
	$(BUILD_DIR)/test_trivial && echo "test_trivial: OK"
	$(BUILD_DIR)/test_wal
	$(BUILD_DIR)/test_gorilla
	$(BUILD_DIR)/test_l0

phase1-check: $(BUILD_DIR)/strata_tool
	tests/phase1_crash_recovery.sh

clean:
	rm -rf $(BUILD_DIR)
