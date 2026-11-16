CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -O2 -MMD -MP

SOURCES := main.cpp simulation.cpp instruction.cpp memory.cpp cpu.cpp
OBJECTS := $(SOURCES:.cpp=.o)
DEPENDS := $(OBJECTS:.o=.d)
TARGET := simulator

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

-include $(DEPENDS)

clean:
	rm -f $(OBJECTS) $(DEPENDS) $(TARGET)
