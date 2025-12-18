// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/RuntimeTests/fixtures.h"

class CodePatcherTest : public RuntimeTest {
 public:
  std::unique_ptr<jit::CompiledFunction> generateCode(
      jit::codegen::NativeGenerator& ngen) {
    auto entry = ngen.getVectorcallEntry();
    if (entry == nullptr) {
      return nullptr;
    }
    std::span<const std::byte> code = ngen.getCodeBuffer();
    int stack_size = ngen.GetCompiledFunctionStackSize();
    int spill_stack_size = ngen.GetCompiledFunctionSpillStackSize();
    return std::make_unique<jit::CompiledFunction>(
        code,
        reinterpret_cast<vectorcallfunc>(entry),
        ngen.getStaticEntry(),
        stack_size,
        spill_stack_size,
        jit::hir::Function::InlineFunctionStats{},
        jit::hir::OpcodeCounts{},
        nullptr);
  }

 protected:
  asmjit::JitRuntime rt_;
};

class MyDeoptPatcher : public jit::JumpPatcher {
 public:
  explicit MyDeoptPatcher(int id) : id_(id) {}

  void onLink() override {
    on_link_ = true;
  }

  void onPatch() override {
    on_patch_ = true;
  }

  void onUnpatch() override {
    on_unpatch_ = true;
  }

  int id() const {
    return id_;
  }

  bool calledOnLink() const {
    return on_link_;
  }

  bool calledOnPatch() const {
    return on_patch_;
  }

  bool calledOnUnpatch() const {
    return on_unpatch_;
  }

 private:
  int id_{-1};
  bool on_link_{false};
  bool on_patch_{false};
  bool on_unpatch_{false};
};

TEST_F(CodePatcherTest, CodePatch) {
  // Intentionally leaving these together to catch accidental stack scribbling.
  uint16_t x = 123;
  uint16_t y = 456;
  uint16_t z = 789;

  std::array<uint8_t, 2> bytes{0xef, 0xbe};

  jit::CodePatcher patcher;
  EXPECT_FALSE(patcher.isLinked());
  EXPECT_FALSE(patcher.isPatched());

  patcher.link(reinterpret_cast<uintptr_t>(&y), bytes);
  EXPECT_TRUE(patcher.isLinked());
  EXPECT_FALSE(patcher.isPatched());
  EXPECT_EQ(x, 123);
  EXPECT_EQ(y, 456);
  EXPECT_EQ(z, 789);

  patcher.patch();
  EXPECT_TRUE(patcher.isPatched());
  EXPECT_EQ(x, 123);
  EXPECT_EQ(y, 0xbeef);
  EXPECT_EQ(z, 789);

  patcher.unpatch();
  EXPECT_FALSE(patcher.isPatched());
  EXPECT_EQ(x, 123);
  EXPECT_EQ(y, 456);
  EXPECT_EQ(z, 789);
}

TEST_F(CodePatcherTest, DeoptPatch) {
  const char* pycode = R"(
def func():
  a = 314159
  return a
)";

  Ref<PyFunctionObject> pyfunc(compileAndGet(pycode, "func"));
  ASSERT_NE(pyfunc, nullptr);

  auto irfunc = buildHIR(pyfunc);

  // Need to find the return instruction.  It should be the last instruction in
  // the last block.
  jit::hir::BasicBlock* entry = irfunc->cfg.entry_block;
  std::vector<jit::hir::BasicBlock*> postorder =
      irfunc->cfg.GetPostOrderTraversal(entry);
  ASSERT_GT(postorder.size(), 0);
  jit::hir::Instr* term = postorder[0]->GetTerminator();
  ASSERT_NE(term, nullptr);
  ASSERT_TRUE(term->IsReturn()) << *term;

  // Insert a patchpoint immediately before the return
  auto patcher = irfunc->allocateCodePatcher<MyDeoptPatcher>(123);
  irfunc->reifier =
      jit::ThreadedRef<>::create(jit::makeFrameReifier(pyfunc->func_code));
  EXPECT_EQ(patcher->id(), 123);
  auto patchpoint = jit::hir::DeoptPatchpoint::create(patcher);
  patchpoint->InsertBefore(*term);

  // Generate machine code and link the patcher
  jit::Compiler::runPasses(*irfunc, jit::PassConfig::kAllExceptInliner);
  jit::codegen::NativeGenerator ngen(irfunc.get());
  auto jitfunc = generateCode(ngen);
  ASSERT_NE(jitfunc, nullptr);
  EXPECT_TRUE(patcher->isLinked());
  EXPECT_TRUE(patcher->calledOnLink());
  EXPECT_FALSE(patcher->isPatched());
  EXPECT_FALSE(patcher->calledOnPatch());

  size_t deopts = 0;
  auto callback = [&deopts](const jit::DeoptMetadata&) { deopts += 1; };
  jit::Runtime* jit_rt = jit::Runtime::get();
  jit_rt->setGuardFailureCallback(callback);

  // Make sure things work in the nominal case.
  auto res = Ref<>::steal(jitfunc->invoke(pyfunc, nullptr, 0));
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(PyLong_AsLong(res), 314159);
  EXPECT_EQ(deopts, 0);
  EXPECT_FALSE(patcher->isPatched());
  EXPECT_FALSE(patcher->calledOnPatch());

  // Patch and verify that a deopt occurred.
  patcher->patch();
  auto res2 = Ref<>::steal(jitfunc->invoke(pyfunc, nullptr, 0));
  ASSERT_NE(res2, nullptr);
  ASSERT_EQ(PyLong_AsLong(res2), 314159);
  EXPECT_EQ(deopts, 1);
  EXPECT_TRUE(patcher->isPatched());
  EXPECT_TRUE(patcher->calledOnPatch());

  // Unpatch and verify that the deopt did not occur.
  patcher->unpatch();
  auto res3 = Ref<>::steal(jitfunc->invoke(pyfunc, nullptr, 0));
  ASSERT_NE(res3, nullptr);
  ASSERT_EQ(PyLong_AsLong(res3), 314159);
  EXPECT_EQ(deopts, 1);
  EXPECT_FALSE(patcher->isPatched());
  EXPECT_TRUE(patcher->calledOnUnpatch());

  jit_rt->clearGuardFailureCallback();
}
