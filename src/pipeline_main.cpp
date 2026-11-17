#include "pipeline_cpu.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint32_t kCodeBase = 0x00400000;
constexpr std::uint32_t kDataBase = 0x10000000;

struct Options {
    std::size_t elements = 2;
    double addend = 0.5;
    model::TimingConfig timing{};
    std::string traceFile = "simulation.log";
};

class ScopedOutputRedirect {
public:
    ScopedOutputRedirect(std::ostream& output, std::ostream& destination)
        : output_(output), previous_(output.rdbuf(destination.rdbuf())) {}

    ~ScopedOutputRedirect() {
        output_.flush();
        output_.rdbuf(previous_);
    }

    ScopedOutputRedirect(const ScopedOutputRedirect&) = delete;
    ScopedOutputRedirect& operator=(const ScopedOutputRedirect&) = delete;

private:
    std::ostream& output_;
    std::streambuf* previous_;
};

std::uint64_t parseUnsigned(const char* value, const char* option) {
    std::size_t consumed = 0;
    const std::string text(value);
    const unsigned long long parsed = std::stoull(text, &consumed, 0);
    if (consumed != text.size()) throw std::invalid_argument(std::string("invalid value for ") + option);
    return static_cast<std::uint64_t>(parsed);
}

double parseDouble(const char* value, const char* option) {
    std::size_t consumed = 0;
    const std::string text(value);
    const double parsed = std::stod(text, &consumed);
    if (consumed != text.size() || !std::isfinite(parsed)) {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    return parsed;
}

Options parseOptions(int argc, char* argv[]) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string option(argv[i]);
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + option);
        const char* value = argv[++i];
        if (option == "--elements") {
            const auto parsed = parseUnsigned(value, "--elements");
            if (parsed == 0 || parsed > 10000) {
                throw std::invalid_argument("--elements must be in [1,10000]");
            }
            options.elements = static_cast<std::size_t>(parsed);
        } else if (option == "--cycle") {
            options.timing.cpuCyclePeriod = parseUnsigned(value, "--cycle");
        } else if (option == "--cpu-to-memory") {
            options.timing.cpuToMemoryLatency = parseUnsigned(value, "--cpu-to-memory");
        } else if (option == "--memory-to-cpu") {
            options.timing.memoryToCpuLatency = parseUnsigned(value, "--memory-to-cpu");
        } else if (option == "--add") {
            options.timing.addLatency = parseUnsigned(value, "--add");
        } else if (option == "--addend") {
            options.addend = parseDouble(value, "--addend");
        } else if (option == "--trace-file") {
            options.traceFile = value;
            if (options.traceFile.empty()) {
                throw std::invalid_argument("--trace-file path must not be empty");
            }
        } else {
            throw std::invalid_argument("unknown option: " + option);
        }
    }
    if (options.timing.cpuCyclePeriod == 0) {
        throw std::invalid_argument("--cycle must be greater than zero");
    }
    if (options.elements >
        (std::numeric_limits<std::uint32_t>::max() - kDataBase) / model::pipeline::kDoubleBytes) {
        throw std::invalid_argument("array does not fit in the 32-bit address space");
    }
    return options;
}

void loadProgram(model::pipeline::Memory& memory) {
    using model::pipeline::Instruction;
    memory.loadInstruction(kCodeBase + 0, Instruction::fld(0, 1, 0));
    memory.loadInstruction(kCodeBase + 4, Instruction::faddD(4, 0, 2));
    memory.loadInstruction(kCodeBase + 8, Instruction::fsd(4, 1, 0));
    memory.loadInstruction(kCodeBase + 12, Instruction::addi(1, 1, 8));
    memory.loadInstruction(kCodeBase + 16, Instruction::bne(1, 2, kCodeBase));
    memory.loadInstruction(kCodeBase + 20, Instruction::halt());
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        const Options options = parseOptions(argc, argv);
        model::Simulation simulation(true, true);
        model::pipeline::Memory memory(simulation, options.timing);
        loadProgram(memory);

        for (std::size_t i = 0; i < options.elements; ++i) {
            const auto address = kDataBase +
                static_cast<std::uint32_t>(i * model::pipeline::kDoubleBytes);
            memory.initializeDouble(address, static_cast<double>(i + 1));
        }

        model::pipeline::Cpu cpu(simulation, memory, options.timing, kCodeBase);
        cpu.setIntegerRegister(1, kDataBase);
        cpu.setIntegerRegister(2, kDataBase + static_cast<std::uint32_t>(
            options.elements * model::pipeline::kDoubleBytes));
        cpu.setFloatingRegister(2, options.addend);

        std::ostream pipelineConsole(std::cout.rdbuf());
        cpu.setPipelineDisplay(pipelineConsole);

        std::cout << "Eight-stage in-order pipeline; 32-bit addresses; 8-bit memory cells\n"
                  << "Clock=" << options.timing.cpuCyclePeriod << " ns, CPU->Memory="
                  << options.timing.cpuToMemoryLatency << " ns, Memory->CPU="
                  << options.timing.memoryToCpuLatency << " ns, ALU/FADD="
                  << options.timing.addLatency << " ns\n"
                  << "Program: fld, fadd.d, fsd, addi, bne Loop, halt\n"
                  << "Detailed trace: " << options.traceFile << '\n';
        std::cout.flush();

        {
            std::ofstream traceFile(options.traceFile);
            if (!traceFile) {
                throw std::runtime_error("cannot open trace file: " + options.traceFile);
            }
            ScopedOutputRedirect redirect(std::cout, traceFile);
            cpu.start();
            simulation.run();
        }
        if (!cpu.halted() || simulation.stopReason() != "ProgramHalt") {
            throw std::runtime_error("program did not halt before the event limit");
        }
        const std::uint64_t expectedRetired = 5 * options.elements + 1;
        if (cpu.retiredInstructions() != expectedRetired ||
            cpu.takenBranches() != options.elements - 1 ||
            cpu.branchFlushes() != options.elements - 1) {
            throw std::runtime_error("testbench retirement/branch count mismatch");
        }
        const auto endAddress = kDataBase + static_cast<std::uint32_t>(
            options.elements * model::pipeline::kDoubleBytes);
        if (cpu.integerRegister(1) != endAddress) {
            throw std::runtime_error("x1 did not reach the exclusive end address");
        }
        if (std::fabs(cpu.floatingRegister(4) -
                      (static_cast<double>(options.elements) + options.addend)) > 1e-12) {
            throw std::runtime_error("f4 does not contain the final loop result");
        }

        std::cout << "\nSimulation stopped at " << simulation.now() << " ns.\n"
                  << "Retired instructions: " << cpu.retiredInstructions() << '\n'
                  << "Clock edges: " << cpu.clockCycles() << '\n'
                  << "ID data-hazard stall cycles: " << cpu.dataHazardStalls() << '\n'
                  << "MEM wait cycles: " << cpu.memoryWaitCycles() << '\n'
                  << "Taken branches / flushes: " << cpu.takenBranches() << " / "
                  << cpu.branchFlushes() << '\n';
        for (std::size_t i = 0; i < options.elements; ++i) {
            const auto address = kDataBase +
                static_cast<std::uint32_t>(i * model::pipeline::kDoubleBytes);
            const double value = memory.inspectDouble(address);
            const double expected = static_cast<double>(i + 1) + options.addend;
            if (std::fabs(value - expected) > 1e-12) {
                throw std::runtime_error("testbench data mismatch at element " +
                                         std::to_string(i));
            }
            std::cout << std::setprecision(12) << "data[" << i << "] @ 0x"
                      << std::hex << address << std::dec << " = " << value << '\n';
        }
        std::cout << "Testbench checks: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
