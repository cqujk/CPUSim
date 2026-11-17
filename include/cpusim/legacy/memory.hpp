#pragma once

#include "instruction.hpp"
#include "simulation.hpp"
#include "timing.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace model {

enum class RequestKind {
    FetchInstruction,
    ReadData,
    WriteData,
};

struct MemoryRequest {
    std::uint64_t id = 0;
    RequestKind kind = RequestKind::FetchInstruction;
    std::uint32_t address = 0;
    std::uint8_t writeData = 0;
};

struct MemoryResponse {
    std::uint64_t requestId = 0;
    RequestKind kind = RequestKind::FetchInstruction;
    std::uint32_t address = 0;
    Instruction instruction{};
    std::uint8_t data = 0;
};

class Memory {
public:
    using ResponseHandler = std::function<void(MemoryResponse)>;

    Memory(Simulation& simulation, TimingConfig timing);

    // Testbench setup/inspection operations are immediate, not timed accesses.
    void loadInstruction(std::uint32_t address, Instruction instruction);
    void initializeDataByte(std::uint32_t address, std::uint8_t value);
    std::uint8_t inspectDataByte(std::uint32_t address) const;

    // Called by the CPU->Memory arrival event, not when the CPU first issues it.
    void receiveRequest(MemoryRequest request, ResponseHandler responseHandler);

private:
    bool isInstructionSlotAddress(std::uint32_t address) const;
    void handleRequest(MemoryRequest request, ResponseHandler responseHandler);
    void scheduleResponse(MemoryResponse response,
                          ResponseHandler responseHandler);

    Simulation& simulation_;
    TimingConfig timing_;
    std::unordered_map<std::uint32_t, Instruction> instructions_;
    std::unordered_map<std::uint32_t, std::uint8_t> data_;
};

} // namespace model
