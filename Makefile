CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -MMD -MP

COMMON_SOURCES := simulation.cpp pipeline_instruction.cpp pipeline_memory.cpp pipeline_cpu.cpp
COMMON_OBJECTS := $(COMMON_SOURCES:.cpp=.o)
COMMON_DEPENDS := $(COMMON_OBJECTS:.o=.d)

.PHONY: all test clean
all: simulator

simulator: pipeline_main.o $(COMMON_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

simulator_tests: pipeline_tests.o $(COMMON_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

test: simulator_tests
	./simulator_tests

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(COMMON_DEPENDS) pipeline_main.d pipeline_tests.d

clean:
	rm -f $(COMMON_OBJECTS) $(COMMON_DEPENDS) pipeline_main.o pipeline_main.d \
		pipeline_tests.o pipeline_tests.d simulator simulator_tests
