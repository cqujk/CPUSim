#include "pipeline_cpu.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t kCodeBase = 0x00400000;
constexpr std::uint32_t kDataBase = 0x10000000;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error("test failed: " + message);
}

struct Result {
    std::vector<double> data;
    double finalF4 = 0.0;
    std::uint32_t finalX1 = 0;
    std::uint64_t retired = 0;
    std::uint64_t taken = 0;
    std::uint64_t flushes = 0;
    std::uint64_t hazards = 0;
    std::uint64_t memWaits = 0;
    model::Tick finishTime = 0;
};

Result runProgram(std::size_t count, model::TimingConfig timing, double addend) {
    model::Simulation simulation(false);
    model::pipeline::Memory memory(simulation, timing);
    memory.loadInstruction(kCodeBase + 0, model::pipeline::Instruction::fld(0, 1));
    memory.loadInstruction(kCodeBase + 4, model::pipeline::Instruction::faddD(4, 0, 2));
    memory.loadInstruction(kCodeBase + 8, model::pipeline::Instruction::fsd(4, 1));
    memory.loadInstruction(kCodeBase + 12, model::pipeline::Instruction::addi(1, 1, 8));
    memory.loadInstruction(kCodeBase + 16, model::pipeline::Instruction::bne(1, 2, kCodeBase));
    memory.loadInstruction(kCodeBase + 20, model::pipeline::Instruction::halt());

    for (std::size_t i = 0; i < count; ++i) {
        memory.initializeDouble(kDataBase + static_cast<std::uint32_t>(i * 8),
                                static_cast<double>(i + 1));
    }
    model::pipeline::Cpu cpu(simulation, memory, timing, kCodeBase);
    cpu.setIntegerRegister(1, kDataBase);
    cpu.setIntegerRegister(2, kDataBase + static_cast<std::uint32_t>(count * 8));
    cpu.setFloatingRegister(2, addend);
    cpu.start();
    simulation.run(1000000);
    require(cpu.halted(), "pipeline reached HALT");
    require(simulation.stopReason() == "ProgramHalt", "simulation stop reason");

    Result result;
    for (std::size_t i = 0; i < count; ++i) {
        result.data.push_back(memory.inspectDouble(
            kDataBase + static_cast<std::uint32_t>(i * 8)));
    }
    result.finalF4 = cpu.floatingRegister(4);
    result.finalX1 = cpu.integerRegister(1);
    result.retired = cpu.retiredInstructions();
    result.taken = cpu.takenBranches();
    result.flushes = cpu.branchFlushes();
    result.hazards = cpu.dataHazardStalls();
    result.memWaits = cpu.memoryWaitCycles();
    result.finishTime = simulation.now();
    return result;
}

void testTwoElementLoop() {
    const auto result = runProgram(2, model::TimingConfig{}, 0.5);
    require(result.data.size() == 2, "two output elements");
    require(std::fabs(result.data[0] - 1.5) < 1e-12, "first output value");
    require(std::fabs(result.data[1] - 2.5) < 1e-12, "second output value");
    require(result.retired == 11, "ten loop instructions and HALT retire");
    require(result.taken == 1 && result.flushes == 1, "taken branch flush count");
    require(result.hazards > 0, "RAW hazards cause ID stalls");
    require(result.memWaits > 0, "MEM waits for data responses");
    require(result.finalX1 == kDataBase + 16, "pointer reaches exclusive end address");
    require(std::fabs(result.finalF4 - 2.5) < 1e-12, "final f4 value");
}

void testOneElementAndAdjustedTiming() {
    model::TimingConfig timing;
    timing.cpuCyclePeriod = 3;
    timing.cpuToMemoryLatency = 1;
    timing.memoryToCpuLatency = 2;
    timing.addLatency = 5;
    const auto result = runProgram(1, timing, -2.25);
    require(std::fabs(result.data[0] + 1.25) < 1e-12, "single-element result");
    require(result.retired == 6, "five loop instructions and HALT retire");
    require(result.taken == 0 && result.flushes == 0, "final branch is not taken");
    require(result.finalX1 == kDataBase + 8, "single-element pointer result");
    require(std::fabs(result.finalF4 + 1.25) < 1e-12, "single-element f4 value");
    require(result.finishTime > 0, "adjusted timing advances simulation");
}

void testExternalEventWinsSameTimeTie() {
    model::Simulation simulation(false);
    std::vector<std::string> order;
    simulation.scheduleAt(5, "CPU", [&] { order.push_back("cpu"); },
                          model::EventPhase::CpuClock);
    simulation.scheduleAt(5, "Memory", [&] { order.push_back("memory"); },
                          model::EventPhase::External);
    simulation.run();
    require(order.size() == 2, "both tied events execute");
    require(order[0] == "memory" && order[1] == "cpu",
            "external response precedes CPU edge regardless of insertion order");
}

} // namespace

int main() {
    try {
        testTwoElementLoop();
        testOneElementAndAdjustedTiming();
        testExternalEventWinsSameTimeTie();
        std::cout << "Pipeline simulator tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
