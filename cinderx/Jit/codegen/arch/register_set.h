// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <bit>
#include <climits>
#include <concepts>
#include <span>

namespace cinderx::jit::codegen {

template <typename PhyLocationType, std::unsigned_integral StorageType>
class RegisterSet {
 public:
  constexpr RegisterSet() = default;
  explicit constexpr RegisterSet(PhyLocationType reg) : rs_{BitAt(reg.loc)} {}
  explicit constexpr RegisterSet(std::span<const PhyLocationType> regs) {
    for (auto reg : regs) {
      rs_ |= BitAt(reg.loc);
    }
  }

  constexpr RegisterSet operator|(PhyLocationType reg) const {
    RegisterSet set;
    set.rs_ = rs_ | BitAt(reg.loc);
    return set;
  }

  constexpr RegisterSet operator|(const RegisterSet& rs) const {
    RegisterSet res;
    res.rs_ = rs_ | rs.rs_;
    return res;
  }

  RegisterSet& operator|=(const RegisterSet& rs) {
    rs_ |= rs.rs_;
    return *this;
  }

  constexpr RegisterSet operator-(PhyLocationType reg) const {
    return operator-(RegisterSet(reg));
  }

  constexpr RegisterSet operator-(RegisterSet rs) const {
    RegisterSet set;
    set.rs_ = rs_ & ~(rs.rs_);
    return set;
  }

  constexpr RegisterSet operator&(RegisterSet rs) const {
    RegisterSet set;
    set.rs_ = rs_ & rs.rs_;
    return set;
  }

  constexpr bool operator==(const RegisterSet& rs) const {
    return rs_ == rs.rs_;
  }

  constexpr bool Empty() const {
    return rs_ == StorageType{0};
  }

  constexpr int count() const {
    return std::popcount(rs_);
  }

  constexpr PhyLocationType GetFirst() const {
    return std::countr_zero(rs_);
  }

  constexpr PhyLocationType GetLast() const {
    return GetLastBit();
  }

  constexpr void RemoveFirst() {
    rs_ &= (rs_ - StorageType{1});
  }

  constexpr void RemoveLast() {
    rs_ &= ~BitAt(GetLastBit());
  }

  constexpr void Set(PhyLocationType reg) {
    rs_ |= BitAt(reg.loc);
  }

  constexpr void Reset(PhyLocationType reg) {
    rs_ &= ~BitAt(reg.loc);
  }

  constexpr void ResetAll() {
    rs_ = StorageType{0};
  }

  constexpr bool Has(PhyLocationType reg) const {
    return rs_ & BitAt(reg.loc);
  }

 private:
  StorageType rs_{0};

  static constexpr StorageType BitAt(int n) {
    return StorageType{1} << n;
  }

  constexpr int GetLastBit() const {
    return (sizeof(rs_) * CHAR_BIT - 1) - std::countl_zero(rs_);
  }
};

} // namespace cinderx::jit::codegen
