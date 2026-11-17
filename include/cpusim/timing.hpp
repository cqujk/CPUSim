#pragma once

#include "simulation.hpp"

namespace model {

struct TimingConfig {
    // One-way delays for CPU/Memory request and response delivery.
    Tick cpuToMemoryLatency = 2;
    Tick memoryToCpuLatency = 2;

    // CPU clock interval and floating-point/ALU execution time.
    Tick cpuCyclePeriod = 2;
    Tick addLatency = 1;
};

} // namespace model
