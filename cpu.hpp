#pragma once
#include "instruction.hpp"
#include "memory.hpp"
#include "register_file.hpp"
#include "simulation.hpp"
#include "timing.hpp"
#include <cstddef>
#include <cstdint>
#include <optional>
namespace model {
enum class CpuState { Ready, WaitingInstruction, WaitingLoadData, WaitingStoreAck, ExecutingAdd, Halted };
class Cpu {
public:
 Cpu(Simulation& simulation,Memory& memory,TimingConfig timing,std::uint32_t entryAddress);
 void start();
 CpuState state()const noexcept{return state_;}
 bool halted()const noexcept{return state_==CpuState::Halted;}
 std::uint32_t pc()const noexcept{return pc_;}
 std::uint32_t registerValue(std::size_t i)const{return registers_.read(i);}
 std::uint64_t completedInstructions()const noexcept{return completedInstructions_;}
 std::optional<Tick> lastInstructionCompletionTime()const noexcept{return lastInstructionCompletionTime_;}
private:
 void beginFetch();
 void issueRequest(RequestKind kind,std::uint32_t address,std::uint8_t writeData=0);
 void onMemoryResponse(MemoryResponse response);
 void executeFetchedInstruction();
 void completeLoad(const MemoryResponse& response);
 void completeStore(const MemoryResponse& response);
 void completeAdd();
 void completeOrdinaryInstruction();
 void advancePc();
 Simulation& simulation_;
 Memory& memory_;
 TimingConfig timing_;
 RegisterFile registers_;
 CpuState state_=CpuState::Ready;
 std::uint32_t pc_=0,currentInstructionPc_=0;
 Instruction currentInstruction_{};
 std::uint32_t pendingAddResult_=0;
 std::uint64_t nextRequestId_=1,outstandingRequestId_=0;
 std::uint64_t completedInstructions_=0;
 std::optional<Tick> lastInstructionCompletionTime_;
};
}
