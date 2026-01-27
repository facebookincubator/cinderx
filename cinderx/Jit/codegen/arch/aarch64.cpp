// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/arch/aarch64.h"

#include "cinderx/Jit/codegen/arch/detection.h"

#ifdef CINDER_AARCH64

namespace jit::codegen {

PhyLocation PhyLocation::parse(std::string_view name) {
#define FIND_GP_REG(V64, V32)           \
  if (name == #V64) {                   \
    return PhyLocation{RegId::V64, 64}; \
  }                                     \
  if (name == #V32) {                   \
    return PhyLocation{RegId::V64, 32}; \
  }

#define FIND_VECD_REG(V)              \
  if (name == #V) {                   \
    return PhyLocation{RegId::V, 64}; \
  }

  FOREACH_GP(FIND_GP_REG)
  FOREACH_VECD(FIND_VECD_REG)
  if (name == "SP") {
    return PhyLocation{SP, 64};
  }
#undef FIND_GP_REG
#undef FIND_VECD_REG
  JIT_ABORT("Unrecognized register {}", name);
}

std::string PhyLocation::toString() const {
  if (is_memory()) {
    return fmt::format("[X29({})]", loc);
  } else if (bitSize == 32 || bitSize == 16 || bitSize == 8) {
    return std::string{name32(static_cast<RegId>(loc))};
  }
  return std::string{name(static_cast<RegId>(loc))};
}

} // namespace jit::codegen

#endif
