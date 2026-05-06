// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/insert_update_prev_instr.h"
#include "cinderx/Jit/hir/instr_effects.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace jit::hir;

class InsertUpdatePrevInstrTest : public RuntimeTest {};

namespace {

int countIf(const Function& func, auto pred) {
  int count = 0;
  for (const auto& block : func.cfg.blocks) {
    for (const auto& instr : block) {
      if (pred(instr)) {
        count++;
      }
    }
  }
  return count;
}

std::unique_ptr<Function> compileAndRunPass(
    RuntimeTest* test,
    const char* src) {
  std::unique_ptr<Function> irfunc;
  test->CompileToHIR(src, "test", irfunc);
  if (irfunc == nullptr) {
    return nullptr;
  }
  SSAify{}.Run(*irfunc);
  Simplify{}.Run(*irfunc);
  PhiElimination{}.Run(*irfunc);
  InsertUpdatePrevInstr{}.Run(*irfunc);
  return irfunc;
}

} // namespace

TEST_F(InsertUpdatePrevInstrTest, RedundantStoresEliminated) {
  // Four len() calls on consecutive lines produce four arbitrary-execution
  // points on different source lines. The additions produce more. Without
  // dead store elimination, each line change emits its own UpdatePrevInstr.
  // With the optimization, consecutive UpdatePrevInstr stores separated only
  // by non-arbitrary-execution instructions are collapsed.
  const char* src = R"(
def test(a):
  w = len(a)
  x = len(a)
  y = len(a)
  z = len(a)
  return w + x + y + z
)";
  auto irfunc = compileAndRunPass(this, src);
  ASSERT_NE(irfunc, nullptr);

  int update_count =
      countIf(*irfunc, [](const Instr& i) { return i.IsUpdatePrevInstr(); });
  int arbitrary_count = countIf(*irfunc, hasArbitraryExecution);

  // There must be at least one UpdatePrevInstr.
  ASSERT_GT(update_count, 0);
  // There must be multiple arbitrary-execution points (len calls + additions).
  ASSERT_GE(arbitrary_count, 7);
  // The optimization must eliminate at least one redundant store: each
  // consecutive pair of arbitrary-execution instructions on different lines
  // with nothing observable between them has its first store removed.
  EXPECT_LT(update_count, arbitrary_count);
}

TEST_F(InsertUpdatePrevInstrTest, SingleCallPreservesStore) {
  const char* src = R"(
def test(a):
  return len(a)
)";
  auto irfunc = compileAndRunPass(this, src);
  ASSERT_NE(irfunc, nullptr);

  int update_count =
      countIf(*irfunc, [](const Instr& i) { return i.IsUpdatePrevInstr(); });
  EXPECT_GT(update_count, 0);
}
