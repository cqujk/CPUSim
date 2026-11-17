#pragma once

#include <cstdint>
#include <string>

namespace model::pipeline {

enum class Opcode { Fld, FaddD, Fsd, Addi, Bne, Halt };
enum class RegisterClass { Integer, FloatingPoint };

struct RegisterRef {
    RegisterClass file = RegisterClass::Integer;
    std::uint8_t index = 0;
};

struct Instruction {
    Opcode opcode = Opcode::Halt;
    std::uint8_t rd = 0;
    std::uint8_t rs1 = 0;
    std::uint8_t rs2 = 0;
    std::int32_t immediate = 0;
    std::uint32_t target = 0;

    static Instruction fld(std::uint8_t fd, std::uint8_t base, std::int32_t offset = 0);
    static Instruction faddD(std::uint8_t fd, std::uint8_t fa, std::uint8_t fb);
    static Instruction fsd(std::uint8_t fs, std::uint8_t base, std::int32_t offset = 0);
    static Instruction addi(std::uint8_t rd, std::uint8_t rs1, std::int32_t immediate);
    static Instruction bne(std::uint8_t rs1, std::uint8_t rs2, std::uint32_t target);
    static Instruction halt();

    bool writesRegister() const noexcept;
    RegisterRef destination() const;
    std::string toString() const;
};

inline constexpr std::uint32_t kInstructionBytes = 4;
inline constexpr std::uint32_t kDoubleBytes = 8;

} // namespace model::pipeline
