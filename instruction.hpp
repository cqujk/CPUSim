#pragma once
#include <cstdint>
#include <string>
namespace model {
enum class Opcode { Load, Add, Store, Halt };
struct Instruction {
 Opcode opcode=Opcode::Halt;
 std::uint8_t rd=0,rs1=0,rs2=0;
 std::uint32_t address=0;
 static Instruction load(std::uint8_t rd,std::uint32_t address);
 static Instruction add(std::uint8_t rd,std::uint8_t rs1,std::uint8_t rs2);
 static Instruction store(std::uint8_t rs,std::uint32_t address);
 static Instruction halt();
 std::string toString() const;
};
constexpr std::uint32_t kInstructionSlotBytes=8;
}
