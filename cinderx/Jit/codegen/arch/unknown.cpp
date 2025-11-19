// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/arch/unknown.h"

// NOLINTNEXTLINE(facebook-unused-include-check)
#include "cinderx/Jit/codegen/arch/detection.h"

#ifdef CINDER_UNKNOWN

namespace jit::codegen {

PhyLocation PhyLocation::parse(std::string_view name) {
#define FIND_GP_REG(V)                \
  if (name == #V) {                   \
    return PhyLocation{RegId::V, 64}; \
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
    return fmt::format("[FP({})]", loc);
  }
  return std::string{name(static_cast<RegId>(loc))};
}

} // namespace jit::codegen

#endif
