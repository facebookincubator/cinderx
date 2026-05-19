// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <cstddef>
#include <cstdint>

namespace jit::codegen {

template <typename RegIdType, int VecDRegBase, int NumRegs>
struct PhyLocationBase {
  static constexpr int REG_INVALID = -1;

  int32_t loc{REG_INVALID};
  uint32_t bitSize{64};

  PhyLocationBase() = default;

  /* implicit */ constexpr PhyLocationBase(RegIdType reg, size_t size = 64)
      : PhyLocationBase{static_cast<int>(reg), size} {}

  /* implicit */ constexpr PhyLocationBase(RegIdType reg, int size)
      : PhyLocationBase{static_cast<int>(reg), static_cast<size_t>(size)} {}

  /* implicit */ constexpr PhyLocationBase(int loc, size_t size = 64)
      : loc{loc}, bitSize{static_cast<uint32_t>(size)} {}

  /* implicit */ constexpr PhyLocationBase(int loc, int size)
      : PhyLocationBase{loc, static_cast<size_t>(size)} {}

  constexpr bool is_memory() const {
    return loc < 0;
  }

  constexpr bool is_register() const {
    return loc >= 0 && loc < NumRegs;
  }

  constexpr bool is_gp_register() const {
    return is_register() && loc < VecDRegBase;
  }

  constexpr bool is_fp_register() const {
    return is_register() && loc >= VecDRegBase;
  }

  // Comparisons are based only on the register ID.
  //
  // TODO: This doesn't account for aliasing in stack slots, e.g.
  // PhyLocation(loc=-8, bitSize=64) and PhyLocation(loc=-12, bitSize=32).
  constexpr bool operator==(const PhyLocationBase& rhs) const {
    return loc == rhs.loc;
  }

  constexpr bool operator!=(const PhyLocationBase& rhs) const {
    return loc != rhs.loc;
  }
};

} // namespace jit::codegen
