#pragma once

#include "pipeline_instruction.hpp"
#include "simulation.hpp"
#include "timing.hpp"

#include <cstdint>
#include <functional>
#include <unordered_map>

namespace model::pipeline {

enum class RequestKind { FetchInstruction, ReadDouble, WriteDouble };

struct MemoryRequest {
    std::uint64_t id = 0;
    std::uint64_t sequenceNumber = 0;
    std::uint64_t fetchEpoch = 0;
    RequestKind kind = RequestKind::FetchInstruction;
    std::uint32_t address = 0;
    std::uint64_t dataBits = 0;
};

struct MemoryResponse {
    std::uint64_t requestId = 0;
    std::uint64_t sequenceNumber = 0;
    std::uint64_t fetchEpoch = 0;
    RequestKind kind = RequestKind::FetchInstruction;
    std::uint32_t address = 0;
    Instruction instruction{};
    std::uint64_t dataBits = 0;
};

class Memory {
public:
    using ResponseHandler = std::function<void(MemoryResponse)>;

    Memory(Simulation& simulation, TimingConfig timing);
    void loadInstruction(std::uint32_t address, Instruction instruction);
    void initializeDouble(std::uint32_t address, double value);
    double inspectDouble(std::uint32_t address) const;
    void receiveRequest(MemoryRequest request, ResponseHandler responseHandler);

private:
    bool overlapsCode(std::uint32_t address, std::uint32_t size) const;
    std::uint64_t readBits(std::uint32_t address) const;
    void writeBits(std::uint32_t address, std::uint64_t bits);

    Simulation& simulation_;
    TimingConfig timing_;
    std::unordered_map<std::uint32_t, Instruction> instructions_;
    std::unordered_map<std::uint32_t, std::uint8_t> data_;
};

} // namespace model::pipeline
