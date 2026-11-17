#include "pipeline_cpu.hpp"

#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <unistd.h>

namespace model::pipeline {

namespace {

std::uint64_t doubleToBits(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

double bitsToDouble(std::uint64_t bits) {
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string registerName(RegisterRef ref) {
    return std::string(ref.file == RegisterClass::Integer ? "x" : "f") +
           std::to_string(static_cast<unsigned>(ref.index));
}

bool useTerminalColor() {
    const char* noColor = std::getenv("NO_COLOR");
    if (noColor != nullptr && *noColor != '\0') return false;
    const char* term = std::getenv("TERM");
    return ::isatty(STDOUT_FILENO) != 0 &&
           (term == nullptr || std::string(term) != "dumb");
}

} // namespace

Cpu::Cpu(Simulation& simulation, Memory& memory, TimingConfig timing,
         std::uint32_t entryAddress)
    : simulation_(simulation), memory_(memory), timing_(timing),
      nextFetchPc_(entryAddress) {
    if (timing_.cpuCyclePeriod == 0) {
        throw std::invalid_argument("CPU cycle period must be greater than zero");
    }
}

void Cpu::setIntegerRegister(std::size_t index, std::uint32_t value) {
    if (started_) throw std::logic_error("registers must be initialized before CPU start");
    registers_.writeInteger(index, value);
}

void Cpu::setFloatingRegister(std::size_t index, double value) {
    if (started_) throw std::logic_error("registers must be initialized before CPU start");
    registers_.writeFloating(index, value);
}

std::uint32_t Cpu::integerRegister(std::size_t index) const {
    return registers_.readInteger(index);
}

double Cpu::floatingRegister(std::size_t index) const {
    return registers_.readFloating(index);
}

void Cpu::start() {
    if (started_) throw std::logic_error("CPU can only start once");
    started_ = true;
    simulation_.scheduleAt(simulation_.now(), "CPU.ClockEdge",
                          [this] { onClockEdge(); }, EventPhase::CpuClock);
}

void Cpu::scheduleNextClock() {
    if (halted_ || simulation_.stopRequested()) return;
    if (timing_.cpuCyclePeriod >
        std::numeric_limits<Tick>::max() - simulation_.now()) {
        throw std::overflow_error("CPU clock time overflow");
    }
    simulation_.scheduleAfter(timing_.cpuCyclePeriod, "CPU.ClockEdge",
                              [this] { onClockEdge(); }, EventPhase::CpuClock);
}

void Cpu::issueFetch() {
    if (fetchStopped_ || pipeline_[static_cast<std::size_t>(Stage::IF)].valid) return;
    if (nextFetchPc_ % kInstructionBytes != 0) {
        throw std::runtime_error("attempted to fetch from an unaligned PC");
    }
    if (nextFetchPc_ > std::numeric_limits<std::uint32_t>::max() - kInstructionBytes) {
        throw std::overflow_error("program counter overflow");
    }

    PipelineEntry fetch;
    fetch.valid = true;
    fetch.pc = nextFetchPc_;
    fetch.sequenceNumber = nextSequenceNumber_++;
    fetch.fetchEpoch = fetchEpoch_;
    fetch.requestId = nextRequestId_++;
    pipeline_[static_cast<std::size_t>(Stage::IF)] = fetch;

    MemoryRequest request;
    request.id = fetch.requestId;
    request.sequenceNumber = fetch.sequenceNumber;
    request.fetchEpoch = fetch.fetchEpoch;
    request.kind = RequestKind::FetchInstruction;
    request.address = fetch.pc;
    outstandingRequests_.emplace(request.id, request.kind);
    nextFetchPc_ += kInstructionBytes;

    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] IF issues fetch seq="
                  << request.sequenceNumber << " PC=0x" << std::hex
                  << request.address << std::dec << '\n';
    }
    const std::string name = "Memory.RequestArrives(Fetch, id=" +
                             std::to_string(request.id) + ")";
    simulation_.scheduleAfter(
        timing_.cpuToMemoryLatency, name,
        [this, request]( ) {
            memory_.receiveRequest(
                request, [this](MemoryResponse response) {
                    onMemoryResponse(std::move(response));
                });
        }, EventPhase::External);
}

void Cpu::issueMemoryRequest(PipelineEntry& entry) {
    if (!entry.valid || entry.requestIssued) return;
    if (entry.instruction.opcode != Opcode::Fld &&
        entry.instruction.opcode != Opcode::Fsd) return;

    MemoryRequest request;
    request.id = nextRequestId_++;
    request.sequenceNumber = entry.sequenceNumber;
    request.kind = entry.instruction.opcode == Opcode::Fld
        ? RequestKind::ReadDouble : RequestKind::WriteDouble;
    request.address = entry.effectiveAddress;
    request.dataBits = entry.storeBits;
    entry.requestId = request.id;
    entry.requestIssued = true;
    outstandingRequests_.emplace(request.id, request.kind);

    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] MEM issues "
                  << (request.kind == RequestKind::ReadDouble ? "fld" : "fsd")
                  << " seq=" << request.sequenceNumber << " request=" << request.id
                  << " address=0x" << std::hex << request.address << std::dec << '\n';
    }
    const std::string name = "Memory.RequestArrives(" +
        std::string(request.kind == RequestKind::ReadDouble ? "Load" : "Store") +
        ", id=" + std::to_string(request.id) + ")";
    simulation_.scheduleAfter(
        timing_.cpuToMemoryLatency, name,
        [this, request]( ) {
            memory_.receiveRequest(
                request, [this](MemoryResponse response) {
                    onMemoryResponse(std::move(response));
                });
        }, EventPhase::External);
}

void Cpu::onMemoryResponse(MemoryResponse response) {
    const auto found = outstandingRequests_.find(response.requestId);
    if (found == outstandingRequests_.end() || found->second != response.kind) {
        throw std::logic_error("unknown or mismatched memory response");
    }
    outstandingRequests_.erase(found);
    const auto inserted = responseMailbox_.emplace(response.requestId,
                                                    std::move(response));
    if (!inserted.second) throw std::logic_error("duplicate memory response");
    const auto& saved = inserted.first->second;
    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] CPU response mailbox receives "
                  << saved.requestId << " for seq=" << saved.sequenceNumber << '\n';
    }
}

void Cpu::applyResponses() {
    auto& fetch = pipeline_[static_cast<std::size_t>(Stage::IF)];
    if (fetch.valid && !fetch.instructionReady) {
        const auto found = responseMailbox_.find(fetch.requestId);
        if (found != responseMailbox_.end()) {
            const auto& response = found->second;
            if (response.kind != RequestKind::FetchInstruction ||
                response.sequenceNumber != fetch.sequenceNumber) {
                throw std::logic_error("fetch response does not match IF slot");
            }
            if (response.fetchEpoch == fetchEpoch_ &&
                response.fetchEpoch == fetch.fetchEpoch) {
                fetch.instruction = response.instruction;
                fetch.instructionReady = true;
                if (simulation_.traceEvents()) {
                    std::cout << "[" << simulation_.now() << " ns] IF receives seq="
                              << fetch.sequenceNumber << " "
                              << fetch.instruction.toString() << '\n';
                }
                if (fetch.instruction.opcode == Opcode::Halt) fetchStopped_ = true;
            } else {
                fetch = PipelineEntry{};
            }
            responseMailbox_.erase(found);
        }
    }

    auto& mem = pipeline_[static_cast<std::size_t>(Stage::MEM)];
    if (mem.valid && mem.requestIssued && !mem.memoryComplete) {
        const auto found = responseMailbox_.find(mem.requestId);
        if (found != responseMailbox_.end()) {
            const auto& response = found->second;
            if (response.sequenceNumber != mem.sequenceNumber ||
                response.kind != (mem.instruction.opcode == Opcode::Fld
                    ? RequestKind::ReadDouble : RequestKind::WriteDouble)) {
                throw std::logic_error("data response does not match MEM slot");
            }
            if (mem.instruction.opcode == Opcode::Fld) {
                mem.resultBits = response.dataBits;
                mem.resultValid = true;
            }
            mem.memoryComplete = true;
            if (simulation_.traceEvents()) {
                std::cout << "[" << simulation_.now() << " ns] MEM completes seq="
                          << mem.sequenceNumber << '\n';
            }
            responseMailbox_.erase(found);
        }
    }
    pruneStaleFetchResponses();
}

void Cpu::pruneStaleFetchResponses() {
    for (auto it = responseMailbox_.begin(); it != responseMailbox_.end();) {
        if (it->second.kind == RequestKind::FetchInstruction &&
            it->second.fetchEpoch != fetchEpoch_) {
            it = responseMailbox_.erase(it);
        } else {
            ++it;
        }
    }
}

std::array<Cpu::SourceOperand, 2> Cpu::sources(const Instruction& instruction,
                                              std::size_t& count) const {
    count = 0;
    std::array<SourceOperand, 2> result{};
    const auto integer = [&result, &count](std::uint8_t index, const char* name) {
        result[count++] = {{RegisterClass::Integer, index}, name};
    };
    const auto floating = [&result, &count](std::uint8_t index, const char* name) {
        result[count++] = {{RegisterClass::FloatingPoint, index}, name};
    };
    switch (instruction.opcode) {
    case Opcode::Fld: integer(instruction.rs1, "base"); break;
    case Opcode::FaddD:
        floating(instruction.rs1, "rs1");
        floating(instruction.rs2, "rs2");
        break;
    case Opcode::Fsd:
        integer(instruction.rs1, "base");
        floating(instruction.rs2, "store-data");
        break;
    case Opcode::Addi: integer(instruction.rs1, "rs1"); break;
    case Opcode::Bne:
        integer(instruction.rs1, "rs1");
        integer(instruction.rs2, "rs2");
        break;
    case Opcode::Halt: break;
    }
    return result;
}

bool Cpu::writes(const PipelineEntry& entry, RegisterRef source) const {
    if (!entry.valid || !entry.instructionReady ||
        !entry.instruction.writesRegister()) return false;
    const RegisterRef destination = entry.instruction.destination();
    if (destination.file != source.file || destination.index != source.index) return false;
    return destination.file != RegisterClass::Integer || destination.index != 0;
}

Cpu::HazardDecision Cpu::checkIdHazard(const PipelineEntry& consumer) const {
    HazardDecision decision;
    if (!consumer.valid || !consumer.instructionReady) return decision;

    std::size_t sourceCount = 0;
    const auto sourceList = sources(consumer.instruction, sourceCount);
    for (std::size_t sourceIndex = 0; sourceIndex < sourceCount; ++sourceIndex) {
        const RegisterRef source = sourceList[sourceIndex].reg;
        for (std::size_t stage = static_cast<std::size_t>(Stage::EX);
             stage <= static_cast<std::size_t>(Stage::WB); ++stage) {
            const auto& producer = pipeline_[stage];
            if (!writes(producer, source)) continue;
            if (!producer.resultValid) {
                decision.stall = true;
                decision.reason = "seq=" + std::to_string(consumer.sequenceNumber) +
                    " waits for " + registerName(source) + " from seq=" +
                    std::to_string(producer.sequenceNumber) + " (" +
                    producer.instruction.toString() + ", " + stageName(stage) +
                    ", result not ready)";
            }
            break; // The youngest older writer owns the architectural value.
        }
        if (decision.stall) return decision;
    }
    return decision;
}

bool Cpu::stageDone(Stage stage, const PipelineEntry& entry) const {
    if (!entry.valid) return true;
    switch (stage) {
    case Stage::IF: return entry.instructionReady;
    case Stage::ID: return true;
    case Stage::EX: return entry.exComplete;
    case Stage::MEM:
        if (entry.instruction.opcode == Opcode::Fld ||
            entry.instruction.opcode == Opcode::Fsd) return entry.memoryComplete;
        return true;
    case Stage::WB: return true;
    }
    return false;
}

Cpu::TransferPlan Cpu::calculateTransfers() const {
    TransferPlan plan;
    for (std::size_t i = 0; i < pipeline_.size(); ++i) {
        plan.done[i] = stageDone(static_cast<Stage>(i), pipeline_[i]);
    }

    const std::size_t wb = static_cast<std::size_t>(Stage::WB);
    plan.willMove[wb] = pipeline_[wb].valid && plan.done[wb];
    plan.canAccept[wb] = !pipeline_[wb].valid || plan.willMove[wb];

    for (int stage = static_cast<int>(Stage::MEM);
         stage >= static_cast<int>(Stage::EX); --stage) {
        const auto i = static_cast<std::size_t>(stage);
        const auto next = i + 1;
        plan.willMove[i] = pipeline_[i].valid && plan.done[i] && plan.canAccept[next];
        plan.canAccept[i] = !pipeline_[i].valid || plan.willMove[i];
    }

    const std::size_t id = static_cast<std::size_t>(Stage::ID);
    const std::size_t ex = static_cast<std::size_t>(Stage::EX);
    plan.idHazard = checkIdHazard(pipeline_[id]);
    plan.willMove[id] = pipeline_[id].valid && plan.done[id] &&
                        plan.canAccept[ex] && !plan.idHazard.stall;
    plan.canAccept[id] = !pipeline_[id].valid || plan.willMove[id];

    const std::size_t fetch = static_cast<std::size_t>(Stage::IF);
    plan.willMove[fetch] = pipeline_[fetch].valid && plan.done[fetch] &&
                           plan.canAccept[id];
    plan.canAccept[fetch] = !pipeline_[fetch].valid || plan.willMove[fetch];
    return plan;
}

void Cpu::captureIdOperands(PipelineEntry& entry) const {
    if (!entry.valid || !entry.instructionReady) return;
    std::size_t sourceCount = 0;
    const auto sourceList = sources(entry.instruction, sourceCount);
    entry.idOperands = {};
    for (std::size_t i = 0; i < sourceCount; ++i) {
        entry.idOperands[i] = registers_.read(sourceList[i].reg);
    }
    entry.idOperandsValid = true;
}

std::optional<std::uint64_t> Cpu::resolveExOperand(RegisterRef source,
                                                  std::uint64_t idValue) const {
    for (std::size_t stage = static_cast<std::size_t>(Stage::MEM);
         stage <= static_cast<std::size_t>(Stage::WB); ++stage) {
        const auto& producer = pipeline_[stage];
        if (!writes(producer, source)) continue;
        if (!producer.resultValid) return std::nullopt;
        return producer.resultBits;
    }
    return idValue;
}

Tick Cpu::executionLatency(Opcode opcode, const TimingConfig& timing) {
    return opcode == Opcode::FaddD || opcode == Opcode::Addi
        ? timing.addLatency : 0;
}

void Cpu::startExWork(PipelineEntry& entry) {
    if (!entry.valid || entry.exStarted) return;
    std::size_t sourceCount = 0;
    const auto sourceList = sources(entry.instruction, sourceCount);
    std::array<std::uint64_t, 2> operands{};
    for (std::size_t i = 0; i < sourceCount; ++i) {
        if (!entry.idOperandsValid) captureIdOperands(entry);
        const auto value = resolveExOperand(sourceList[i].reg, entry.idOperands[i]);
        if (!value) return;
        operands[i] = *value;
    }
    entry.operandA = operands[0];
    entry.operandB = operands[1];
    entry.exStarted = true;
    const Tick latency = executionLatency(entry.instruction.opcode, timing_);
    if (latency > std::numeric_limits<Tick>::max() - simulation_.now()) {
        throw std::overflow_error("EX completion time overflow");
    }
    entry.exReadyAt = simulation_.now() + latency;
    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] EX starts seq="
                  << entry.sequenceNumber << " " << entry.instruction.toString()
                  << " using bypass-selected operands\n";
    }
    completeReadyExWork(entry);
}

void Cpu::completeReadyExWork(PipelineEntry& entry) {
    if (!entry.valid || !entry.exStarted || entry.exComplete ||
        simulation_.now() < entry.exReadyAt) return;

    const Instruction& instruction = entry.instruction;
    switch (instruction.opcode) {
    case Opcode::Fld:
        entry.effectiveAddress = static_cast<std::uint32_t>(entry.operandA) +
                                 static_cast<std::uint32_t>(instruction.immediate);
        break;
    case Opcode::FaddD:
        entry.resultBits = doubleToBits(bitsToDouble(entry.operandA) +
                                        bitsToDouble(entry.operandB));
        entry.resultValid = true;
        break;
    case Opcode::Fsd:
        entry.effectiveAddress = static_cast<std::uint32_t>(entry.operandA) +
                                 static_cast<std::uint32_t>(instruction.immediate);
        entry.storeBits = entry.operandB;
        break;
    case Opcode::Addi:
        entry.resultBits = static_cast<std::uint32_t>(entry.operandA) +
                           static_cast<std::uint32_t>(instruction.immediate);
        entry.resultValid = true;
        break;
    case Opcode::Bne:
        entry.branchTaken = static_cast<std::uint32_t>(entry.operandA) !=
                            static_cast<std::uint32_t>(entry.operandB);
        break;
    case Opcode::Halt:
        break;
    }
    entry.exComplete = true;
    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] EX completes seq="
                  << entry.sequenceNumber << " " << instruction.toString();
        if (instruction.opcode == Opcode::Fld || instruction.opcode == Opcode::Fsd) {
            std::cout << " effective-address=0x" << std::hex << entry.effectiveAddress
                      << std::dec;
        } else if (instruction.opcode == Opcode::Addi) {
            std::cout << " result=" << static_cast<std::uint32_t>(entry.resultBits);
        } else if (instruction.opcode == Opcode::FaddD) {
            std::cout << " result=" << bitsToDouble(entry.resultBits);
        } else if (instruction.opcode == Opcode::Bne) {
            std::cout << " taken=" << (entry.branchTaken ? "yes" : "no");
        }
        std::cout << '\n';
    }
}

void Cpu::writeBack(const PipelineEntry& entry) {
    if (!entry.valid) return;
    if (entry.instruction.writesRegister()) {
        if (!entry.resultValid) throw std::logic_error("WB result is not valid");
        const RegisterRef destination = entry.instruction.destination();
        registers_.write(destination, entry.resultBits);
        if (simulation_.traceEvents()) {
            std::cout << "[" << simulation_.now() << " ns] WB seq="
                      << entry.sequenceNumber << " writes " << registerName(destination)
                      << " = ";
            if (destination.file == RegisterClass::FloatingPoint) {
                std::cout << bitsToDouble(entry.resultBits);
            } else {
                std::cout << static_cast<std::uint32_t>(entry.resultBits);
            }
            std::cout << '\n';
        }
    }

    ++retiredInstructions_;
    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] WB retires seq="
                  << entry.sequenceNumber << " " << entry.instruction.toString() << '\n';
    }
    if (entry.instruction.opcode == Opcode::Halt) {
        halted_ = true;
        simulation_.requestStop("ProgramHalt");
    }
}

void Cpu::updatePipeline(const TransferPlan& plan, bool redirect,
                         std::uint32_t redirectTarget) {
    auto next = pipeline_;
    for (std::size_t i = 0; i < pipeline_.size(); ++i) {
        if (plan.willMove[i]) next[i] = PipelineEntry{};
    }
    for (std::size_t i = 0; i + 1 < pipeline_.size(); ++i) {
        if (plan.willMove[i]) next[i + 1] = pipeline_[i];
    }

    if (redirect) {
        const std::size_t ex = static_cast<std::size_t>(Stage::EX);
        const bool branchLeavesEx = plan.willMove[ex];
        next[static_cast<std::size_t>(Stage::IF)] = PipelineEntry{};
        next[static_cast<std::size_t>(Stage::ID)] = PipelineEntry{};
        next[ex] = branchLeavesEx ? PipelineEntry{} : pipeline_[ex];
        if (fetchEpoch_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("fetch epoch overflow");
        }
        ++fetchEpoch_;
        nextFetchPc_ = redirectTarget;
        fetchStopped_ = false;
        ++branchFlushes_;
        if (simulation_.traceEvents()) {
            std::cout << "[" << simulation_.now() << " ns] Branch flushes younger IF/ID; "
                      << "next PC=0x" << std::hex << redirectTarget << std::dec
                      << " epoch=" << fetchEpoch_ << '\n';
        }
    }
    pipeline_ = std::move(next);
}

void Cpu::startNewStageWork() {
    captureIdOperands(pipeline_[static_cast<std::size_t>(Stage::ID)]);
    startExWork(pipeline_[static_cast<std::size_t>(Stage::EX)]);
    issueMemoryRequest(pipeline_[static_cast<std::size_t>(Stage::MEM)]);
    requestFetchIfNeeded();
}

void Cpu::requestFetchIfNeeded() {
    issueFetch();
}

void Cpu::logPipeline(const TransferPlan& plan) const {
    if (!simulation_.traceEvents()) return;
    const bool color = useTerminalColor();
    if (color) std::cout << "\033[36m";
    std::cout << "[" << simulation_.now() << " ns] PIPE";
    for (std::size_t i = 0; i < pipeline_.size(); ++i) {
        std::cout << ' ' << stageName(i) << '=';
        if (!pipeline_[i].valid) {
            std::cout << '-';
        } else {
            std::cout << '#' << pipeline_[i].sequenceNumber;
            if (pipeline_[i].instructionReady) {
                std::cout << ':' << pipeline_[i].instruction.toString();
            } else {
                std::cout << ":fetch-wait";
            }
            if (!plan.done[i]) std::cout << "(wait)";
        }
    }
    if (plan.idHazard.stall) std::cout << " | ID stall: " << plan.idHazard.reason;
    if (color) std::cout << "\033[0m";
    std::cout << '\n';
}

void Cpu::onClockEdge() {
    if (halted_) return;
    ++clockCycles_;
    applyResponses();

    auto& ex = pipeline_[static_cast<std::size_t>(Stage::EX)];
    completeReadyExWork(ex);
    const TransferPlan plan = calculateTransfers();
    logPipeline(plan);
    if (plan.idHazard.stall) ++dataHazardStalls_;
    const auto& mem = pipeline_[static_cast<std::size_t>(Stage::MEM)];
    if (mem.valid && !stageDone(Stage::MEM, mem)) ++memoryWaitCycles_;

    const std::size_t wb = static_cast<std::size_t>(Stage::WB);
    if (plan.willMove[wb]) writeBack(pipeline_[wb]);
    captureIdOperands(pipeline_[static_cast<std::size_t>(Stage::ID)]);

    bool redirect = false;
    std::uint32_t redirectTarget = 0;
    if (ex.valid && ex.instruction.opcode == Opcode::Bne && ex.exComplete &&
        ex.branchTaken && !ex.branchRedirected) {
        redirect = true;
        redirectTarget = ex.instruction.target;
        ex.branchRedirected = true;
        ++takenBranches_;
    }

    updatePipeline(plan, redirect, redirectTarget);
    startNewStageWork();
    scheduleNextClock();
}

const char* Cpu::stageName(std::size_t index) {
    static constexpr const char* names[] = {"IF", "ID", "EX", "MEM", "WB"};
    if (index >= 5) throw std::out_of_range("pipeline stage index");
    return names[index];
}

} // namespace model::pipeline
