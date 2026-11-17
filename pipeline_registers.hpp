#pragma once

#include "pipeline_instruction.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace model::pipeline {

class RegisterFile {
public:
    std::uint32_t readInteger(std::size_t index) const {
        check(index);
        return index == 0 ? 0 : integer_[index];
    }

    void writeInteger(std::size_t index, std::uint32_t value) {
        check(index);
        if (index != 0) integer_[index] = value;
    }

    double readFloating(std::size_t index) const {
        check(index);
        double value = 0.0;
        const std::uint64_t bits = floatingBits_[index];
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    std::uint64_t readFloatingBits(std::size_t index) const {
        check(index);
        return floatingBits_[index];
    }

    void writeFloating(std::size_t index, double value) {
        check(index);
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        floatingBits_[index] = bits;
    }

    void writeFloatingBits(std::size_t index, std::uint64_t bits) {
        check(index);
        floatingBits_[index] = bits;
    }

    std::uint64_t read(RegisterRef ref) const {
        return ref.file == RegisterClass::Integer
            ? readInteger(ref.index) : readFloatingBits(ref.index);
    }

    void write(RegisterRef ref, std::uint64_t value) {
        if (ref.file == RegisterClass::Integer) {
            writeInteger(ref.index, static_cast<std::uint32_t>(value));
        } else {
            writeFloatingBits(ref.index, value);
        }
    }

private:
    static void check(std::size_t index) {
        if (index >= 32) throw std::out_of_range("register index must be in [0,31]");
    }

    std::array<std::uint32_t, 32> integer_{};
    std::array<std::uint64_t, 32> floatingBits_{};
};

} // namespace model::pipeline
