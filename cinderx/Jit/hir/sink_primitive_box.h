// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/Jit/hir/pass.h"

namespace cinderx::jit::hir {

// Sink PrimitiveBox instructions onto deopt paths.
//
// When a boxed primitive's result has no data-operand uses (it is referenced
// only by deopt frame state) the box exists solely so the value can be
// materialized for the interpreter if a deopt occurs.  The deopt machinery can
// re-box an unboxed primitive from its LiveValue (via value_kind), so we
// rewrite those frame-state references to the unboxed source value, leaving the
// box dead. This keeps chained primitive arithmetic unboxed on the fast path
// while staying correct on deopt.  Currently limited to CDouble (floats).
class SinkPrimitiveBox final : public Pass {
 public:
  SinkPrimitiveBox() : Pass("SinkPrimitiveBox") {}

  void run(Function& irfunc) override;

  static std::unique_ptr<SinkPrimitiveBox> factory() {
    return std::make_unique<SinkPrimitiveBox>();
  }
};

} // namespace cinderx::jit::hir
