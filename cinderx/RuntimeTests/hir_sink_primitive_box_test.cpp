// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/sink_primitive_box.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <algorithm>

namespace cinderx {

using namespace cinderx::jit::hir;

// Named apart from the SinkPrimitiveBoxTest suite that
// hir_tests/sink_primitive_box_test.txt registers, which uses its own fixture.
class SinkPrimitiveBoxGuiltyRegTest : public RuntimeTest {};

// The HIR parser has no syntax for a guilty register, so this case cannot live
// in hir_tests/sink_primitive_box_test.txt with the rest of the pass tests.
TEST_F(SinkPrimitiveBoxGuiltyRegTest, KeepsBoxNamedByAGuiltyRegister) {
  Function func;
  auto block = func.cfg.allocateBlock();
  func.cfg.entry_block = block;

  auto value = func.env.allocateRegister();
  auto boxed = func.env.allocateRegister();
  auto result = func.env.allocateRegister();

  block->append<LoadConst>(value, Type::fromCInt(7, TCInt64));
  FrameState frame_state;
  block->append<PrimitiveBox>(boxed, value, TCInt64, frame_state);

  // Nothing consumes the box as a value, but the deopt machinery reports a
  // guilty register as a real object.
  auto patchpoint = block->append<DeoptPatchpoint>(nullptr);
  patchpoint->setGuiltyReg(boxed);

  block->append<LoadConst>(result, TNoneType);
  block->append<Return>(result);

  SinkPrimitiveBox{}.run(func);

  // Sinking the box would have rewritten the guilty register to the unboxed
  // value and left the box dead.
  EXPECT_EQ(patchpoint->guiltyReg(), boxed);
  EXPECT_EQ(
      std::count_if(
          block->begin(),
          block->end(),
          [](const Instr& instr) { return instr.isPrimitiveBox(); }),
      1);
}

} // namespace cinderx
