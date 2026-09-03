// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/pass.h"

#include <iosfwd>
#include <memory>

namespace cinderx::jit::hir {

// Check that func's CFG is well-formed and that its Register uses and defs are
// vald SSA, returning true iff no errors were found. Details of any errors
// will be written to err.
bool checkFunc(const Function& func, std::ostream& err);

class SSAify final : public Pass {
 public:
  SSAify() : Pass("SSAify") {}

  SSAify(const SSAify&) = delete;
  SSAify& operator=(const SSAify&) = delete;

  void run(Function& irfunc) override;
  void run(Function& irfunc, BasicBlock* block);

  static std::unique_ptr<SSAify> factory() {
    return std::make_unique<SSAify>();
  }
};

} // namespace cinderx::jit::hir
