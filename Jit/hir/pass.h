// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/hir.h"

#include <string>
#include <string_view>

namespace jit::hir {

class Pass {
 public:
  explicit Pass(std::string_view name) : name_{name} {}
  virtual ~Pass() = default;

  virtual void Run(Function& irfunc) = 0;

  constexpr std::string_view name() const {
    return name_;
  }

 protected:
  std::string name_;
};

// Recursively chase a list of assignments and get the original register value.
// If there are no assignments then just get the register back.
Register* chaseAssignOperand(Register* value);

} // namespace jit::hir
