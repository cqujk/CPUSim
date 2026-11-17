#include "cpu.hpp"

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
    throw std::logic_error("invalid CPU request kind");
}

} // namespace

Cpu::Cpu(Simulation& simulation, Memory& memory, TimingConfig timing,
         std::uint32_t entryAddress)
    : simulation_(simulation), memory_(memory), timing_(timing),
      pc_(entryAddress) {}

void Cpu::start() {
    if (state_ != CpuState::Ready) {
        throw std::logic_error("CPU can only start once");
    }

    simulation_.scheduleAt(simulation_.now(), "CPU.Start",
                           [this] { beginFetch(); });
}

void Cpu::beginFetch() {
    if (state_ != CpuState::Ready) {
        throw std::logic_error("CPU is not ready to fetch");
    }

    currentInstructionPc_ = pc_;
    state_ = CpuState::WaitingInstruction;

    std::cout << "[" << simulation_.now()
              << " ns] CPU issues instruction fetch for PC=0x" << std::hex
              << pc_ << std::dec << '\n';

    issueRequest(RequestKind::FetchInstruction, pc_);
}

void Cpu::issueRequest(RequestKind kind, std::uint32_t address,
                       std::uint8_t writeData) {
    if (outstandingRequestId_ != 0) {
        throw std::logic_error(
            "CPU already has an outstanding memory request");
    }

    const std::uint64_t requestId = nextRequestId_++;
    outstandingRequestId_ = requestId;

    MemoryRequest request{requestId, kind, address, writeData};
    Memory::ResponseHandler responseHandler =
        [this](MemoryResponse response) {
            onMemoryResponse(std::move(response));
        };

    const std::string eventName =
        "CPU.RequestArrivesAtMemory(" + std::string(requestKindName(kind)) +
        ", id=" + std::to_string(requestId) + ")";

    std::cout << "[" << simulation_.now()
              << " ns] CPU schedules request delivery: "
              << requestKindName(kind) << " id=" << requestId << " address=0x"
              << std::hex << address << std::dec << '\n';

    // The CPU puts the request-arrival event directly in the global queue.
    // Memory is called only when this event fires after the link latency.
    simulation_.scheduleAfter(
        timing_.cpuToMemoryLatency, eventName,
        [this, request = std::move(request),
         responseHandler = std::move(responseHandler)]() mutable {
            memory_.receiveRequest(std::move(request),
                                   std::move(responseHandler));
        });
}

void Cpu::onMemoryResponse(MemoryResponse response) {
    if (response.requestId != outstandingRequestId_) {
        throw std::logic_error(
            "memory response does not match outstanding request");
    }
    outstandingRequestId_ = 0;

    switch (state_) {
    case CpuState::WaitingInstruction:
        if (response.kind != RequestKind::FetchInstruction) {
            throw std::logic_error("expected an instruction response");
        }
        currentInstruction_ = response.instruction;
        std::cout << "[" << simulation_.now() << " ns] CPU receives "
                  << currentInstruction_.toString() << " at PC=0x" << std::hex
                  << currentInstructionPc_ << std::dec << '\n';
        executeFetchedInstruction();
        break;

    case CpuState::WaitingLoadData:
        if (response.kind != RequestKind::ReadData) {
            throw std::logic_error("expected a data-read response");
        }
        completeLoad(response);
        break;

    case CpuState::WaitingStoreAck:
        if (response.kind != RequestKind::WriteData) {
            throw std::logic_error("expected a store acknowledgement");
        }
        completeStore(response);
        break;

    case CpuState::Ready:
    case CpuState::ExecutingAdd:
    case CpuState::Halted:
        throw std::logic_error(
            "CPU received a response in an invalid state");
    }
}

void Cpu::executeFetchedInstruction() {
    switch (currentInstruction_.opcode) {
    case Opcode::Load:
        if (currentInstruction_.rd >= 32) {
            throw std::out_of_range(
                "Load destination register is out of range");
        }

        std::cout << "[" << simulation_.now()
                  << " ns] CPU decodes Load: read data address "
                  << currentInstruction_.address << " for r"
                  << static_cast<unsigned int>(currentInstruction_.rd) << '\n';

        state_ = CpuState::WaitingLoadData;
        issueRequest(RequestKind::ReadData, currentInstruction_.address);
        break;

    case Opcode::Add: {
        const auto [sourceA, sourceB] =
            registers_.readTwoPorts(currentInstruction_.rs1,
                                    currentInstruction_.rs2);
        pendingAddResult_ = sourceA + sourceB;
        state_ = CpuState::ExecutingAdd;

        std::cout << "[" << simulation_.now()
                  << " ns] CPU decodes Add: r"
                  << static_cast<unsigned int>(currentInstruction_.rs1) << "="
                  << sourceA << ", r"
                  << static_cast<unsigned int>(currentInstruction_.rs2) << "="
                  << sourceB << ", pending result=" << pendingAddResult_
                  << '\n';

        simulation_.scheduleAfter(
            timing_.addLatency, "CPU.AddComplete",
            [this] { completeAdd(); });
        break;
    }

    case Opcode::Store: {
        const std::uint32_t sourceValue =
            registers_.read(currentInstruction_.rs1);
        const auto byte = static_cast<std::uint8_t>(sourceValue);

        std::cout << "[" << simulation_.now()
                  << " ns] CPU decodes Store: send byte "
                  << static_cast<unsigned int>(byte) << " from r"
                  << static_cast<unsigned int>(currentInstruction_.rs1)
                  << " to data address " << currentInstruction_.address
                  << '\n';

        state_ = CpuState::WaitingStoreAck;
        issueRequest(RequestKind::WriteData, currentInstruction_.address,
                     byte);
        break;
    }

    case Opcode::Halt:
        state_ = CpuState::Halted;
        std::cout << "[" << simulation_.now()
                  << " ns] CPU completes Halt\n";

        simulation_.scheduleAt(
            simulation_.now(), "Simulation.Exit(ProgramHalt)",
            [this] { simulation_.requestStop("ProgramHalt"); });
        break;
    }
}

void Cpu::completeLoad(const MemoryResponse& response) {
    registers_.write(currentInstruction_.rd, response.data);

    std::cout << "[" << simulation_.now() << " ns] CPU writes r"
              << static_cast<unsigned int>(currentInstruction_.rd) << " <- "
              << static_cast<unsigned int>(response.data) << '\n';

    completeOrdinaryInstruction();
}

void Cpu::completeStore(const MemoryResponse& response) {
    std::cout << "[" << simulation_.now()
              << " ns] CPU receives Store acknowledgement for address "
              << response.address << '\n';

    completeOrdinaryInstruction();
}

void Cpu::completeAdd() {
    if (state_ != CpuState::ExecutingAdd) {
        throw std::logic_error(
            "Add completion arrived in an invalid CPU state");
    }

    registers_.write(currentInstruction_.rd, pendingAddResult_);

    std::cout << "[" << simulation_.now() << " ns] CPU writes r"
              << static_cast<unsigned int>(currentInstruction_.rd) << " <- "
              << pendingAddResult_ << '\n';

    completeOrdinaryInstruction();
}

void Cpu::advancePc() {
    constexpr auto maxAddress = std::numeric_limits<std::uint32_t>::max();
    if (currentInstructionPc_ >
        maxAddress - kInstructionSlotBytes) {
        throw std::overflow_error("program counter overflow");
    }
    pc_ = currentInstructionPc_ + kInstructionSlotBytes;
}

void Cpu::completeOrdinaryInstruction() {
    advancePc();
    ++completedInstructions_;
    lastInstructionCompletionTime_ = simulation_.now();

    std::cout << "[" << simulation_.now()
              << " ns] CPU completes instruction at 0x" << std::hex
              << currentInstructionPc_ << "; next PC=0x" << pc_ << std::dec
              << '\n';

    state_ = CpuState::Ready;
    beginFetch();
}

} // namespace model
