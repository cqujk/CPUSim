#include "cpu.hpp"
#include "instruction.hpp"
#include "memory.hpp"
#include "simulation.hpp"
#include "timing.hpp"
#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
namespace {
std::uint8_t parseByte(std::string_view s,const char* name){
 unsigned int v=0;const auto[end,ec]=std::from_chars(s.data(),s.data()+s.size(),v);
 if(ec!=std::errc{}||end!=s.data()+s.size()||v>255)
  throw std::invalid_argument(std::string(name)+" must be an integer in [0,255]");
 return static_cast<std::uint8_t>(v);
}
void require(bool ok,const std::string& message){if(!ok)throw std::runtime_error("testbench failed: "+message);}
}
int main(int argc,char*argv[]){
 try{
  if(argc!=1&&argc!=3){std::cerr<<"Usage: "<<argv[0]<<" [memory[0] memory[1]]\n";return 2;}
  const auto input0=argc==3?parseByte(argv[1],"memory[0]"):static_cast<std::uint8_t>(10);
  const auto input1=argc==3?parseByte(argv[2],"memory[1]"):static_cast<std::uint8_t>(20);
  model::Simulation simulation;
  const model::TimingConfig timing{};
  model::Memory memory(simulation,timing);
  constexpr std::uint32_t entry=0x100;
  memory.initializeDataByte(0,input0);memory.initializeDataByte(1,input1);memory.initializeDataByte(3,0);
  // Structured instruction records use logical slots in a separate code map.
  memory.loadInstruction(entry,model::Instruction::load(1,0));
  memory.loadInstruction(entry+8,model::Instruction::load(2,1));
  memory.loadInstruction(entry+16,model::Instruction::add(3,1,2));
  memory.loadInstruction(entry+24,model::Instruction::store(3,3));
  memory.loadInstruction(entry+32,model::Instruction::halt());
  model::Cpu cpu(simulation,memory,timing,entry);
  std::cout<<"Model: request=2 ns, response=2 ns, memory service=0 ns, Add=1 ns\n";
  std::cout<<"Initial data: memory[0]="<<static_cast<unsigned int>(input0)
   <<", memory[1]="<<static_cast<unsigned int>(input1)<<'\n';
  std::cout<<"Entry PC: 0x"<<std::hex<<entry<<std::dec<<'\n';
  cpu.start();simulation.run();
  const auto sum=static_cast<std::uint32_t>(input0)+static_cast<std::uint32_t>(input1);
  require(cpu.halted(),"CPU did not execute Halt");
  require(simulation.stopReason()=="ProgramHalt","wrong simulation exit reason");
  require(cpu.registerValue(1)==input0,"r1 mismatch");
  require(cpu.registerValue(2)==input1,"r2 mismatch");
  require(cpu.registerValue(3)==sum,"r3 mismatch");
  require(memory.inspectDataByte(3)==static_cast<std::uint8_t>(sum),"memory[3] mismatch");
  require(cpu.completedInstructions()==4,"expected four non-Halt instructions");
  require(cpu.lastInstructionCompletionTime()==model::Tick{29},"last business instruction should complete at 29 ns");
  require(simulation.now()==33,"Halt should stop simulation at 33 ns");
  std::cout<<"\nSimulation stopped at "<<simulation.now()<<" ns ("<<simulation.stopReason()<<").\n";
  std::cout<<"Business instructions completed: "<<cpu.completedInstructions()<<'\n';
  std::cout<<"Last business instruction completed at "<<*cpu.lastInstructionCompletionTime()<<" ns.\n";
  std::cout<<"r1="<<cpu.registerValue(1)<<", r2="<<cpu.registerValue(2)
   <<", r3="<<cpu.registerValue(3)<<'\n';
  std::cout<<"memory[3]="<<static_cast<unsigned int>(memory.inspectDataByte(3))<<'\n';
  std::cout<<"Processed events: "<<simulation.processedEvents()<<'\n';
  std::cout<<"Testbench checks: PASS\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<"Error: "<<e.what()<<'\n';return 1;}
}
