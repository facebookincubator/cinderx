// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/hir/opcode.h"

namespace jit::hir {

std::optional<Opcode> opcodeFromName(std::string_view name) {
#define CASE_OP(OP)       \
  if (name == #OP) {      \
    return Opcode::k##OP; \
  }
  FOREACH_OPCODE(CASE_OP)
#undef CASE_OP
  return std::nullopt;
}

} // namespace jit::hir
