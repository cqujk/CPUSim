#include "memory.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace model {

namespace {

const char* requestKindName(RequestKind kind) {
    switch (kind) {
    case RequestKind::FetchInstruction:
        return "FetchInstruction";
    case RequestKind::ReadData:
        return "ReadData";
    case RequestKind::WriteData:
        return "WriteData";
    }
    throw std::logic_error("invalid memory request kind");
}

} // namespace

Memory::Memory(Simulation& simulation, TimingConfig timing)
    : simulation_(simulation), timing_(timing) {}

bool Memory::isInstructionSlotAddress(std::uint32_t address) const {
    for (const auto& [startAddress, instruction] : instructions_) {
        (void)instruction;
        const std::uint64_t start = startAddress;
        const std::uint64_t end = start + kInstructionSlotBytes;
        if (address >= start && static_cast<std::uint64_t>(address) < end) {
            return true;
        }
    }
    return false;
}

void Memory::loadInstruction(std::uint32_t address, Instruction instruction) {
    if (address % kInstructionSlotBytes != 0) {
        throw std::invalid_argument("instruction address must be 8-byte aligned");
    }
    if (static_cast<std::uint64_t>(address) + kInstructionSlotBytes - 1 >
        std::numeric_limits<std::uint32_t>::max()) {
        throw std::invalid_argument(
            "instruction slot exceeds the 32-bit address space");
    }
    if (instructions_.count(address) != 0) {
        throw std::invalid_argument("duplicate instruction address");
    }

    for (std::uint32_t offset = 0; offset < kInstructionSlotBytes; ++offset) {
        if (data_.count(address + offset) != 0) {
            throw std::invalid_argument(
                "instruction and data address overlap");
        }
    }

    instructions_.emplace(address, std::move(instruction));
}

void Memory::initializeDataByte(std::uint32_t address, std::uint8_t value) {
    if (isInstructionSlotAddress(address)) {
        throw std::invalid_argument("instruction and data address overlap");
    }
    data_[address] = value;
}

std::uint8_t Memory::inspectDataByte(std::uint32_t address) const {
    const auto found = data_.find(address);
    return found == data_.end() ? 0 : found->second;
}

void Memory::receiveRequest(MemoryRequest request,
                            ResponseHandler responseHandler) {
    if (!responseHandler) {
        throw std::invalid_argument(
            "memory response handler must not be empty");
    }

    std::cout << "[" << simulation_.now()
              << " ns] Memory receives " << requestKindName(request.kind)
              << " id=" << request.id << " address=0x" << std::hex
              << request.address << std::dec;
    if (request.kind == RequestKind::WriteData) {
        std::cout << " data="
                  << static_cast<unsigned int>(request.writeData);
    }
    std::cout << '\n';

    handleRequest(std::move(request), std::move(responseHandler));
}

void Memory::handleRequest(MemoryRequest request,
                           ResponseHandler responseHandler) {
    MemoryResponse response;
    response.requestId = request.id;
    response.kind = request.kind;
    response.address = request.address;

    switch (request.kind) {
    case RequestKind::FetchInstruction: {
        const auto found = instructions_.find(request.address);
        if (found == instructions_.end()) {
            throw std::runtime_error(
                "no instruction at requested address");
        }

        response.instruction = found->second;
        std::cout << "[" << simulation_.now() << " ns] Memory returns "
                  << response.instruction.toString() << " for instruction "
                  << "address 0x" << std::hex << request.address << std::dec
                  << '\n';
        break;
    }

    case RequestKind::ReadData:
        if (isInstructionSlotAddress(request.address)) {
            throw std::runtime_error(
                "data read overlaps an instruction slot");
        }
        response.data = inspectDataByte(request.address);
        std::cout << "[" << simulation_.now() << " ns] Memory reads "
                  << static_cast<unsigned int>(response.data)
                  << " from data address " << request.address << '\n';
        break;

    case RequestKind::WriteData:
        if (isInstructionSlotAddress(request.address)) {
            throw std::runtime_error(
                "data write overlaps an instruction slot");
        }
        data_[request.address] = request.writeData;
        std::cout << "[" << simulation_.now() << " ns] Memory writes "
                  << static_cast<unsigned int>(request.writeData)
                  << " to data address " << request.address << '\n';
        break;
    }

    scheduleResponse(std::move(response), std::move(responseHandler));
}

void Memory::scheduleResponse(MemoryResponse response,
                              ResponseHandler responseHandler) {
    const std::string eventName =
        "Memory.ResponseArrivesAtCPU(id=" +
        std::to_string(response.requestId) + ")";

    simulation_.scheduleAfter(
        timing_.memoryToCpuLatency, eventName,
        [response = std::move(response),
         responseHandler = std::move(responseHandler)]() mutable {
            responseHandler(std::move(response));
        });
}

} // namespace model
