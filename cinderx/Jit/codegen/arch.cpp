// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/codegen/arch.h"

namespace jit::codegen {

std::ostream& operator<<(std::ostream& out, const PhyLocation& loc) {
  return out << loc.toString();
}

} // namespace jit::codegen
