CXX := g++
CPPFLAGS := -Iinclude/cpusim
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -MMD -MP
BUILD_DIR := build
OBJECT_DIR := $(BUILD_DIR)/obj

PIPELINE_SOURCES := \
	src/simulation.cpp \
	src/pipeline_instruction.cpp \
	src/pipeline_memory.cpp \
	src/pipeline_cpu.cpp
PIPELINE_OBJECTS := $(patsubst src/%.cpp,$(OBJECT_DIR)/%.o,$(PIPELINE_SOURCES))
PIPELINE_MAIN := $(OBJECT_DIR)/pipeline_main.o
TEST_OBJECT := $(OBJECT_DIR)/tests/pipeline_tests.o

.PHONY: all test clean
all: $(BUILD_DIR)/simulator

$(BUILD_DIR)/simulator: $(PIPELINE_MAIN) $(PIPELINE_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD_DIR)/simulator_tests: $(TEST_OBJECT) $(PIPELINE_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

test: $(BUILD_DIR)/simulator_tests
	./$(BUILD_DIR)/simulator_tests

$(OBJECT_DIR)/%.o: src/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

$(OBJECT_DIR)/tests/%.o: tests/%.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c -o $@ $<

-include $(PIPELINE_MAIN:.o=.d) $(PIPELINE_OBJECTS:.o=.d) $(TEST_OBJECT:.o=.d)

clean:
	rm -rf $(BUILD_DIR)
