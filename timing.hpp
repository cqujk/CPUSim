#pragma once

#include "simulation.hpp"

namespace model {

struct TimingConfig {
    // One-way delays for CPU/Memory request and response delivery.
    Tick cpuToMemoryLatency = 2;
    Tick memoryToCpuLatency = 2;

    // ALU delay for the Add instruction.
    Tick addLatency = 1;
};

} // namespace model
