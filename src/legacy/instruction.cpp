#include "instruction.hpp"
#include <stdexcept>
namespace model {
Instruction Instruction::load(std::uint8_t dest,std::uint32_t addr){return {Opcode::Load,dest,0,0,addr};}
Instruction Instruction::add(std::uint8_t dest,std::uint8_t a,std::uint8_t b){return {Opcode::Add,dest,a,b,0};}
Instruction Instruction::store(std::uint8_t src,std::uint32_t addr){return {Opcode::Store,0,src,0,addr};}
Instruction Instruction::halt(){return {Opcode::Halt,0,0,0,0};}
std::string Instruction::toString() const {
 switch(opcode){
 case Opcode::Load:return "Load r"+std::to_string(rd)+", #"+std::to_string(address);
 case Opcode::Add:return "Add r"+std::to_string(rd)+", r"+std::to_string(rs1)+", r"+std::to_string(rs2);
 case Opcode::Store:return "Store r"+std::to_string(rs1)+", #"+std::to_string(address);
 case Opcode::Halt:return "Halt";
 }
 throw std::logic_error("invalid opcode");
}
}
