#include "pipeline_instruction.hpp"

#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace model::pipeline {

Instruction Instruction::fld(std::uint8_t fd, std::uint8_t base, std::int32_t offset) {
    return {Opcode::Fld, fd, base, 0, offset, 0};
}

Instruction Instruction::faddD(std::uint8_t fd, std::uint8_t fa, std::uint8_t fb) {
    return {Opcode::FaddD, fd, fa, fb, 0, 0};
}

Instruction Instruction::fsd(std::uint8_t fs, std::uint8_t base, std::int32_t offset) {
    return {Opcode::Fsd, 0, base, fs, offset, 0};
}

Instruction Instruction::addi(std::uint8_t dest, std::uint8_t source,
                              std::int32_t value) {
    return {Opcode::Addi, dest, source, 0, value, 0};
}

Instruction Instruction::bne(std::uint8_t a, std::uint8_t b,
                             std::uint32_t branchTarget) {
    return {Opcode::Bne, 0, a, b, 0, branchTarget};
}

Instruction Instruction::halt() {
    return {Opcode::Halt, 0, 0, 0, 0, 0};
}

bool Instruction::writesRegister() const noexcept {
    return opcode == Opcode::Fld || opcode == Opcode::FaddD || opcode == Opcode::Addi;
}

RegisterRef Instruction::destination() const {
    if (opcode == Opcode::Fld || opcode == Opcode::FaddD) {
        return {RegisterClass::FloatingPoint, rd};
    }
    if (opcode == Opcode::Addi) {
        return {RegisterClass::Integer, rd};
    }
    throw std::logic_error("instruction does not write a register");
}

std::string Instruction::toString() const {
    std::ostringstream out;
    switch (opcode) {
    case Opcode::Fld:
        out << "fld f" << static_cast<unsigned>(rd) << ", " << immediate
            << "(x" << static_cast<unsigned>(rs1) << ')';
        break;
    case Opcode::FaddD:
        out << "fadd.d f" << static_cast<unsigned>(rd) << ", f"
            << static_cast<unsigned>(rs1) << ", f" << static_cast<unsigned>(rs2);
        break;
    case Opcode::Fsd:
        out << "fsd f" << static_cast<unsigned>(rs2) << ", " << immediate
            << "(x" << static_cast<unsigned>(rs1) << ')';
        break;
    case Opcode::Addi:
        out << "addi x" << static_cast<unsigned>(rd) << ", x"
            << static_cast<unsigned>(rs1) << ", " << immediate;
        break;
    case Opcode::Bne:
        out << "bne x" << static_cast<unsigned>(rs1) << ", x"
            << static_cast<unsigned>(rs2) << ", 0x" << std::hex << target;
        break;
    case Opcode::Halt:
        out << "halt";
        break;
    }
    return out.str();
}

} // namespace model::pipeline
