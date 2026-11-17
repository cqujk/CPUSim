#pragma once

#include "pipeline_memory.hpp"
#include "pipeline_registers.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace model::pipeline {

enum class Stage : std::size_t { IF = 0, ID = 1, EX = 2, MEM = 3, WB = 4 };

struct PipelineEntry {
    bool valid = false;
    bool instructionReady = false;
    Instruction instruction{};
    std::uint32_t pc = 0;
    std::uint64_t sequenceNumber = 0;
    std::uint64_t fetchEpoch = 0;
    std::uint64_t requestId = 0;
    bool requestIssued = false;
    bool memoryComplete = false;
    bool resultValid = false;
    std::uint64_t resultBits = 0;
    bool exStarted = false;
    bool exComplete = false;
    Tick exReadyAt = 0;
    bool idOperandsValid = false;
    std::array<std::uint64_t, 2> idOperands{};
    std::uint64_t operandA = 0;
    std::uint64_t operandB = 0;
    std::uint32_t effectiveAddress = 0;
    std::uint64_t storeBits = 0;
    bool branchTaken = false;
    bool branchRedirected = false;
};

class Cpu {
public:
    Cpu(Simulation& simulation, Memory& memory, TimingConfig timing,
        std::uint32_t entryAddress);

    void start();
    void setIntegerRegister(std::size_t index, std::uint32_t value);
    void setFloatingRegister(std::size_t index, double value);
    std::uint32_t integerRegister(std::size_t index) const;
    double floatingRegister(std::size_t index) const;
    std::uint64_t retiredInstructions() const noexcept { return retiredInstructions_; }
    std::uint64_t clockCycles() const noexcept { return clockCycles_; }
    std::uint64_t dataHazardStalls() const noexcept { return dataHazardStalls_; }
    std::uint64_t memoryWaitCycles() const noexcept { return memoryWaitCycles_; }
    std::uint64_t takenBranches() const noexcept { return takenBranches_; }
    std::uint64_t branchFlushes() const noexcept { return branchFlushes_; }
    bool halted() const noexcept { return halted_; }
    const std::array<PipelineEntry, 5>& pipeline() const noexcept { return pipeline_; }

private:
    struct SourceOperand { RegisterRef reg; const char* name; };
    struct HazardDecision { bool stall = false; std::string reason; };
    struct TransferPlan {
        std::array<bool, 5> done{};
        std::array<bool, 5> willMove{};
        std::array<bool, 5> canAccept{};
        HazardDecision idHazard;
    };

    void onClockEdge();
    void scheduleNextClock();
    void issueFetch();
    void issueMemoryRequest(PipelineEntry& entry);
    void onMemoryResponse(MemoryResponse response);
    void applyResponses();
    void pruneStaleFetchResponses();
    void startExWork(PipelineEntry& entry);
    void completeReadyExWork(PipelineEntry& entry);
    void captureIdOperands(PipelineEntry& entry) const;
    std::optional<std::uint64_t> resolveExOperand(RegisterRef source,
                                                  std::uint64_t idValue) const;
    HazardDecision checkIdHazard(const PipelineEntry& consumer) const;
    std::array<SourceOperand, 2> sources(const Instruction& instruction,
                                         std::size_t& count) const;
    bool writes(const PipelineEntry& entry, RegisterRef source) const;
    bool stageDone(Stage stage, const PipelineEntry& entry) const;
    TransferPlan calculateTransfers() const;
    void writeBack(const PipelineEntry& entry);
    void updatePipeline(const TransferPlan& plan, bool redirect,
                        std::uint32_t redirectTarget);
    void startNewStageWork();
    void requestFetchIfNeeded();
    void logPipeline(const TransferPlan& plan) const;
    static const char* stageName(std::size_t index);
    static Tick executionLatency(Opcode opcode, const TimingConfig& timing);

    Simulation& simulation_;
    Memory& memory_;
    TimingConfig timing_;
    RegisterFile registers_;
    std::array<PipelineEntry, 5> pipeline_{};
    std::unordered_map<std::uint64_t, MemoryResponse> responseMailbox_;
    std::unordered_map<std::uint64_t, RequestKind> outstandingRequests_;
    std::uint32_t nextFetchPc_ = 0;
    std::uint64_t nextSequenceNumber_ = 1;
    std::uint64_t nextRequestId_ = 1;
    std::uint64_t fetchEpoch_ = 0;
    bool fetchStopped_ = false;
    bool started_ = false;
    bool halted_ = false;
    std::uint64_t retiredInstructions_ = 0;
    std::uint64_t clockCycles_ = 0;
    std::uint64_t dataHazardStalls_ = 0;
    std::uint64_t memoryWaitCycles_ = 0;
    std::uint64_t takenBranches_ = 0;
    std::uint64_t branchFlushes_ = 0;
};

} // namespace model::pipeline
