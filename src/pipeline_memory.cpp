#include "pipeline_memory.hpp"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace model::pipeline {

namespace {

const char* requestName(RequestKind kind) {
    switch (kind) {
    case RequestKind::FetchInstruction: return "FetchInstruction";
    case RequestKind::ReadDouble: return "ReadDouble";
    case RequestKind::WriteDouble: return "WriteDouble";
    }
    throw std::logic_error("invalid memory request kind");
}

} // namespace

Memory::Memory(Simulation& simulation, TimingConfig timing)
    : simulation_(simulation), timing_(timing) {}

bool Memory::overlapsCode(std::uint32_t address, std::uint32_t size) const {
    const std::uint64_t first = address;
    const std::uint64_t last = first + size;
    for (const auto& [pc, instruction] : instructions_) {
        (void)instruction;
        const std::uint64_t codeFirst = pc;
        const std::uint64_t codeLast = codeFirst + kInstructionBytes;
        if (first < codeLast && codeFirst < last) return true;
    }
    return false;
}

void Memory::loadInstruction(std::uint32_t address, Instruction instruction) {
    if (address % kInstructionBytes != 0) {
        throw std::invalid_argument("instruction address must be 4-byte aligned");
    }
    if (static_cast<std::uint64_t>(address) + kInstructionBytes - 1 >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("instruction exceeds the 32-bit address space");
    }
    if (instructions_.count(address) != 0) {
        throw std::invalid_argument("duplicate instruction address");
    }
    if (overlapsCode(address, kInstructionBytes)) {
        throw std::invalid_argument("overlapping instruction addresses");
    }
    for (std::uint32_t offset = 0; offset < kInstructionBytes; ++offset) {
        if (data_.count(address + offset) != 0) {
            throw std::invalid_argument("instruction and data address overlap");
        }
    }
    instructions_.emplace(address, instruction);
}

void Memory::initializeDouble(std::uint32_t address, double value) {
    if (address % kDoubleBytes != 0) {
        throw std::invalid_argument("double address must be 8-byte aligned");
    }
    if (static_cast<std::uint64_t>(address) + kDoubleBytes - 1 >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument("double exceeds the 32-bit address space");
    }
    if (overlapsCode(address, kDoubleBytes)) {
        throw std::invalid_argument("instruction and data address overlap");
    }
    std::uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    writeBits(address, bits);
}

double Memory::inspectDouble(std::uint32_t address) const {
    std::uint64_t bits = readBits(address);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::uint64_t Memory::readBits(std::uint32_t address) const {
    if (address % kDoubleBytes != 0) {
        throw std::runtime_error("double read address is not 8-byte aligned");
    }
    if (static_cast<std::uint64_t>(address) + kDoubleBytes - 1 >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("double read exceeds the 32-bit address space");
    }
    if (overlapsCode(address, kDoubleBytes)) {
        throw std::runtime_error("data read overlaps an instruction");
    }
    std::uint64_t bits = 0;
    for (std::uint32_t offset = 0; offset < kDoubleBytes; ++offset) {
        const auto found = data_.find(address + offset);
        const std::uint8_t byte = found == data_.end() ? 0 : found->second;
        bits |= static_cast<std::uint64_t>(byte) << (offset * 8);
    }
    return bits;
}

void Memory::writeBits(std::uint32_t address, std::uint64_t bits) {
    if (address % kDoubleBytes != 0) {
        throw std::runtime_error("double write address is not 8-byte aligned");
    }
    if (static_cast<std::uint64_t>(address) + kDoubleBytes - 1 >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("double write exceeds the 32-bit address space");
    }
    if (overlapsCode(address, kDoubleBytes)) {
        throw std::runtime_error("data write overlaps an instruction");
    }
    for (std::uint32_t offset = 0; offset < kDoubleBytes; ++offset) {
        data_[address + offset] = static_cast<std::uint8_t>(bits >> (offset * 8));
    }
}

void Memory::receiveRequest(MemoryRequest request, ResponseHandler responseHandler) {
    if (!responseHandler) throw std::invalid_argument("empty memory response handler");
    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] Memory receives "
                  << requestName(request.kind) << " id=" << request.id << " address=0x"
                  << std::hex << request.address << std::dec << '\n';
    }

    MemoryResponse response;
    response.requestId = request.id;
    response.sequenceNumber = request.sequenceNumber;
    response.fetchEpoch = request.fetchEpoch;
    response.kind = request.kind;
    response.address = request.address;
    switch (request.kind) {
    case RequestKind::FetchInstruction: {
        const auto found = instructions_.find(request.address);
        if (found == instructions_.end()) {
            throw std::runtime_error("no instruction at requested PC");
        }
        response.instruction = found->second;
        if (simulation_.traceEvents()) {
            std::cout << "[" << simulation_.now() << " ns] Memory fetches "
                      << response.instruction.toString() << '\n';
        }
        break;
    }
    case RequestKind::ReadDouble:
        response.dataBits = readBits(request.address);
        break;
    case RequestKind::WriteDouble:
        writeBits(request.address, request.dataBits);
        break;
    }

    const std::string eventName = "Memory.Response(id=" +
                                  std::to_string(request.id) + ")";
    simulation_.scheduleAfter(
        timing_.memoryToCpuLatency, eventName,
        [response = std::move(response), handler = std::move(responseHandler)]() mutable {
            handler(std::move(response));
        }, EventPhase::External);
}

} // namespace model::pipeline
