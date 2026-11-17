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
        std::cout << "[" << simulation_.now() << " ns] MEM1 issues "
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

    auto& mem2 = pipeline_[static_cast<std::size_t>(Stage::MEM2)];
    if (mem2.valid && mem2.requestIssued && !mem2.memoryComplete) {
        const auto found = responseMailbox_.find(mem2.requestId);
        if (found != responseMailbox_.end()) {
            const auto& response = found->second;
            if (response.sequenceNumber != mem2.sequenceNumber ||
                response.kind != (mem2.instruction.opcode == Opcode::Fld
                    ? RequestKind::ReadDouble : RequestKind::WriteDouble)) {
                throw std::logic_error("data response does not match MEM2 slot");
            }
            if (mem2.instruction.opcode == Opcode::Fld) {
                mem2.resultBits = response.dataBits;
                mem2.resultValid = true;
            }
            mem2.memoryComplete = true;
            if (simulation_.traceEvents()) {
                std::cout << "[" << simulation_.now() << " ns] MEM2 completes seq="
                          << mem2.sequenceNumber << '\n';
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
        for (std::size_t stage = static_cast<std::size_t>(Stage::EX1);
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
            break; // The nearest older writer determines this operand value.
        }
        if (decision.stall) return decision;
    }
    return decision;
}

bool Cpu::stageDone(Stage stage, const PipelineEntry& entry) const {
    if (!entry.valid) return true;
    switch (stage) {
    case Stage::IF: return entry.instructionReady;
    case Stage::ID1: return entry.decoded;
    case Stage::ID2: return true;
    case Stage::EX1: return entry.ex1Complete;
    case Stage::EX2: return entry.ex2Complete;
    case Stage::MEM1:
        if (entry.instruction.opcode == Opcode::Fld ||
            entry.instruction.opcode == Opcode::Fsd) return entry.requestIssued;
        return true;
    case Stage::MEM2:
        if (entry.instruction.opcode == Opcode::Fld ||
            entry.instruction.opcode == Opcode::Fsd) return entry.memoryComplete;
        return true;
    case Stage::WB: return true;
    case Stage::Count: return false;
    }
    return false;
}

Cpu::TransferPlan Cpu::calculateTransfers() const {
    TransferPlan plan;
    for (std::size_t i = 0; i < pipeline_.size(); ++i) {
        plan.done[i] = stageDone(static_cast<Stage>(i), pipeline_[i]);
    }

    const std::size_t id2 = static_cast<std::size_t>(Stage::ID2);
    plan.id2Hazard = checkIdHazard(pipeline_[id2]);

    const std::size_t wb = static_cast<std::size_t>(Stage::WB);
    for (int stage = static_cast<int>(Stage::WB); stage >= 0; --stage) {
        const auto i = static_cast<std::size_t>(stage);
        if (i == wb) {
            plan.willMove[i] = pipeline_[i].valid && plan.done[i];
        } else {
            const bool dataReady = i != id2 || !plan.id2Hazard.stall;
            plan.willMove[i] = pipeline_[i].valid && plan.done[i] &&
                               plan.canAccept[i + 1] && dataReady;
        }
        plan.canAccept[i] = !pipeline_[i].valid || plan.willMove[i];
    }
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
    for (std::size_t stage = static_cast<std::size_t>(Stage::EX2);
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

void Cpu::startEx1Work(PipelineEntry& entry) {
    if (!entry.valid || entry.ex1Started) return;
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
    entry.ex1Started = true;
    const Tick latency = entry.instruction.opcode == Opcode::Addi
        ? executionLatency(entry.instruction.opcode, timing_) : 0;
    if (latency > std::numeric_limits<Tick>::max() - simulation_.now()) {
        throw std::overflow_error("EX1 completion time overflow");
    }
    entry.ex1ReadyAt = simulation_.now() + latency;
    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] EX1 starts seq="
                  << entry.sequenceNumber << " " << entry.instruction.toString()
                  << " using bypass-selected operands\n";
    }
    completeReadyEx1Work(entry);
}

void Cpu::completeReadyEx1Work(PipelineEntry& entry) {
    if (!entry.valid || !entry.ex1Started || entry.ex1Complete ||
        simulation_.now() < entry.ex1ReadyAt) return;

    const Instruction& instruction = entry.instruction;
    switch (instruction.opcode) {
    case Opcode::Fld:
    case Opcode::Fsd:
        entry.effectiveAddress = static_cast<std::uint32_t>(entry.operandA) +
                                 static_cast<std::uint32_t>(instruction.immediate);
        if (instruction.opcode == Opcode::Fsd) entry.storeBits = entry.operandB;
        break;
    case Opcode::Addi:
        entry.resultBits = static_cast<std::uint32_t>(entry.operandA) +
                           static_cast<std::uint32_t>(instruction.immediate);
        entry.resultValid = true;
        break;
    case Opcode::FaddD:
    case Opcode::Bne:
    case Opcode::Halt:
        break;
    }
    entry.ex1Complete = true;
    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] EX1 completes seq="
                  << entry.sequenceNumber << " " << instruction.toString();
        if (instruction.opcode == Opcode::Fld || instruction.opcode == Opcode::Fsd) {
            std::cout << " effective-address=0x" << std::hex << entry.effectiveAddress
                      << std::dec;
        } else if (instruction.opcode == Opcode::Addi) {
            std::cout << " result=" << static_cast<std::uint32_t>(entry.resultBits);
        }
        std::cout << '\n';
    }
}

void Cpu::startEx2Work(PipelineEntry& entry) {
    if (!entry.valid || entry.ex2Started) return;
    entry.ex2Started = true;
    const Tick latency = entry.instruction.opcode == Opcode::FaddD
        ? executionLatency(entry.instruction.opcode, timing_) : 0;
    if (latency > std::numeric_limits<Tick>::max() - simulation_.now()) {
        throw std::overflow_error("EX2 completion time overflow");
    }
    entry.ex2ReadyAt = simulation_.now() + latency;
    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] EX2 starts seq="
                  << entry.sequenceNumber << " " << entry.instruction.toString()
                  << '\n';
    }
    completeReadyEx2Work(entry);
}

void Cpu::completeReadyEx2Work(PipelineEntry& entry) {
    if (!entry.valid || !entry.ex2Started || entry.ex2Complete ||
        simulation_.now() < entry.ex2ReadyAt) return;

    const Instruction& instruction = entry.instruction;
    switch (instruction.opcode) {
    case Opcode::FaddD:
        entry.resultBits = doubleToBits(bitsToDouble(entry.operandA) +
                                        bitsToDouble(entry.operandB));
        entry.resultValid = true;
        break;
    case Opcode::Bne:
        entry.branchTaken = static_cast<std::uint32_t>(entry.operandA) !=
                            static_cast<std::uint32_t>(entry.operandB);
        break;
    case Opcode::Fld:
    case Opcode::Fsd:
    case Opcode::Addi:
    case Opcode::Halt:
        break;
    }
    entry.ex2Complete = true;
    if (simulation_.traceEvents()) {
        std::cout << "[" << simulation_.now() << " ns] EX2 completes seq="
                  << entry.sequenceNumber << " " << instruction.toString();
        if (instruction.opcode == Opcode::FaddD) {
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
        const std::size_t ex1 = static_cast<std::size_t>(Stage::EX1);
        const std::size_t ex2 = static_cast<std::size_t>(Stage::EX2);
        const bool branchLeavesEx2 = plan.willMove[ex2];
        for (std::size_t i = 0; i <= ex1; ++i) {
            next[i] = PipelineEntry{};
        }
        next[ex2] = branchLeavesEx2 ? PipelineEntry{} : pipeline_[ex2];
        if (fetchEpoch_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("fetch epoch overflow");
        }
        ++fetchEpoch_;
        nextFetchPc_ = redirectTarget;
        fetchStopped_ = false;
        ++branchFlushes_;
        if (simulation_.traceEvents()) {
            std::cout << "[" << simulation_.now()
                      << " ns] Branch flushes younger IF..EX1; next PC=0x"
                      << std::hex << redirectTarget << std::dec
                      << " epoch=" << fetchEpoch_ << '\n';
        }
    }
    pipeline_ = std::move(next);
}

void Cpu::startNewStageWork() {
    auto& id1 = pipeline_[static_cast<std::size_t>(Stage::ID1)];
    if (id1.valid && !id1.decoded && id1.instructionReady) {
        id1.decoded = true;
        if (simulation_.traceEvents()) {
            std::cout << "[" << simulation_.now() << " ns] ID1 decodes seq="
                      << id1.sequenceNumber << " " << id1.instruction.toString()
                      << '\n';
        }
    }
    captureIdOperands(pipeline_[static_cast<std::size_t>(Stage::ID2)]);
    startEx1Work(pipeline_[static_cast<std::size_t>(Stage::EX1)]);
    startEx2Work(pipeline_[static_cast<std::size_t>(Stage::EX2)]);
    issueMemoryRequest(pipeline_[static_cast<std::size_t>(Stage::MEM1)]);
    requestFetchIfNeeded();
}

void Cpu::requestFetchIfNeeded() {
    issueFetch();
}

void Cpu::logPipeline(const TransferPlan& plan) const {
    if (!simulation_.traceEvents()) return;
    const bool color = simulation_.traceColor() && useTerminalColor();
    std::ostringstream line;
    if (color) line << "\033[36m";
    line << "[" << simulation_.now() << " ns] PIPE";
    for (std::size_t i = 0; i < pipeline_.size(); ++i) {
        line << ' ' << stageName(i) << '=';
        if (!pipeline_[i].valid) {
            line << '-';
        } else {
            line << '#' << pipeline_[i].sequenceNumber;
            if (pipeline_[i].instructionReady) {
                line << ':' << pipeline_[i].instruction.toString();
            } else {
                line << ":fetch-wait";
            }
            if (!plan.done[i]) line << "(wait)";
        }
    }
    if (plan.id2Hazard.stall) line << " | ID2 stall: " << plan.id2Hazard.reason;
    if (color) line << "\033[0m";
    line << '\n';

    const std::string snapshot = line.str();
    std::cout << snapshot;
    if (pipelineDisplay_ != nullptr) {
        *pipelineDisplay_ << snapshot;
        pipelineDisplay_->flush();
    }
}

void Cpu::onClockEdge() {
    if (halted_) return;
    ++clockCycles_;
    const auto& waitingMem2 = pipeline_[static_cast<std::size_t>(Stage::MEM2)];
    if (waitingMem2.valid && !stageDone(Stage::MEM2, waitingMem2)) {
        ++memoryWaitCycles_;
    }
    applyResponses();

    completeReadyEx1Work(pipeline_[static_cast<std::size_t>(Stage::EX1)]);
    auto& ex2 = pipeline_[static_cast<std::size_t>(Stage::EX2)];
    completeReadyEx2Work(ex2);

    const TransferPlan plan = calculateTransfers();
    if (plan.id2Hazard.stall) ++dataHazardStalls_;

    const std::size_t wb = static_cast<std::size_t>(Stage::WB);
    if (plan.willMove[wb]) writeBack(pipeline_[wb]);
    captureIdOperands(pipeline_[static_cast<std::size_t>(Stage::ID2)]);

    bool redirect = false;
    std::uint32_t redirectTarget = 0;
    if (ex2.valid && ex2.instruction.opcode == Opcode::Bne && ex2.ex2Complete &&
        ex2.branchTaken && !ex2.branchRedirected) {
        redirect = true;
        redirectTarget = ex2.instruction.target;
        ex2.branchRedirected = true;
        ++takenBranches_;
    }

    updatePipeline(plan, redirect, redirectTarget);
    startNewStageWork();
    logPipeline(calculateTransfers());
    scheduleNextClock();
}

const char* Cpu::stageName(std::size_t index) {
    static constexpr const char* names[] = {
        "IF", "ID1", "ID2", "EX1", "EX2", "MEM1", "MEM2", "WB"
    };
    if (index >= kPipelineStages) throw std::out_of_range("pipeline stage index");
    return names[index];
}

} // namespace model::pipeline
