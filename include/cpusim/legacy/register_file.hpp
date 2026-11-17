#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
namespace model {
class RegisterFile {
public:
 std::pair<std::uint32_t,std::uint32_t> readTwoPorts(std::size_t a,std::size_t b) const {
  check(a);check(b);return {registers_[a],registers_[b]};
 }
 std::uint32_t read(std::size_t index) const {check(index);return registers_[index];}
 void write(std::size_t index,std::uint32_t value){check(index);registers_[index]=value;}
private:
 static void check(std::size_t index){if(index>=32)throw std::out_of_range("register index must be in [0,31]");}
 std::array<std::uint32_t,32> registers_{};
};
}
