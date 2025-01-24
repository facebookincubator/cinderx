// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/x86_64.h"

#include <algorithm>
#include <utility>

namespace jit::codegen {

std::ostream& operator<<(std::ostream& os, const PhyLocation& loc) {
  return os << loc.toString();
}

std::string PhyLocation::toString() const {
  if (is_memory()) {
    return fmt::format("[RBP({})]", loc);
  } else if (bitSize == 32) {
    return std::string{name32(static_cast<RegId>(loc))};
  } else if (bitSize == 16) {
    return std::string{name16(static_cast<RegId>(loc))};
  } else if (bitSize == 8) {
    return std::string{name8(static_cast<RegId>(loc))};
  }
  return std::string{name(static_cast<RegId>(loc))};
}

// Parse the string given in name to the physical location.  Currently only
// parsing physical register names is supported.
PhyLocation PhyLocation::parse(const std::string& name) {
#define FIND_GP_REG(V64, V32, V16, V8)  \
  if (name == #V64) {                   \
    return PhyLocation{RegId::V64, 64}; \
  }                                     \
  if (name == #V32) {                   \
    return PhyLocation{RegId::V64, 32}; \
  }                                     \
  if (name == #V16) {                   \
    return PhyLocation{RegId::V64, 16}; \
  }                                     \
  if (name == #V8) {                    \
    return PhyLocation{RegId::V64, 8};  \
  }

#define FIND_XMM_REG(V)                \
  if (name == #V) {                    \
    return PhyLocation{RegId::V, 128}; \
  }

  FOREACH_GP(FIND_GP_REG)
  FOREACH_XMM(FIND_XMM_REG)
#undef FIND_GP_REG
#undef FIND_XMM_REG
  JIT_ABORT("Unrecognized register {}", name);
}

} // namespace jit::codegen
