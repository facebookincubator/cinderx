// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/RuntimeTests/fixtures.h"

class DeoptPatcherTest : public RuntimeTest {
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
        jit::hir::OpcodeCounts{});
  }

 protected:
  asmjit::JitRuntime rt_;
};

class MyDeoptPatcher : public jit::DeoptPatcher {
 public:
  explicit MyDeoptPatcher(int id) : id_(id) {}

  void onLink() override {
    linked_ = true;
  }

  void onPatch() override {
    patched_ = true;
  }

  bool isLinked() const {
    return linked_;
  }

  bool isPatched() const {
    return patched_;
  }

  int id() const {
    return id_;
  }

 private:
  int id_{-1};
  bool linked_{false};
  bool patched_{false};
};

TEST_F(DeoptPatcherTest, Patch) {
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
  jit::Runtime* jit_rt = jit::Runtime::get();
  auto patcher = jit_rt->allocateDeoptPatcher<MyDeoptPatcher>(123);
  EXPECT_EQ(patcher->id(), 123);
  auto patchpoint = jit::hir::DeoptPatchpoint::create(patcher);
  patchpoint->InsertBefore(*term);

  // Generate machine code and link the patcher
  jit::Compiler::runPasses(*irfunc, jit::PassConfig::kAllExceptInliner);
  jit::codegen::NativeGenerator ngen(irfunc.get());
  auto jitfunc = generateCode(ngen);
  ASSERT_NE(jitfunc, nullptr);
  EXPECT_TRUE(patcher->isLinked());
  EXPECT_FALSE(patcher->isPatched());

  // Make sure things work in the nominal case
  bool did_deopt = false;
  auto callback = [&did_deopt](const jit::DeoptMetadata&) { did_deopt = true; };
  jit_rt->setGuardFailureCallback(callback);
  auto res = Ref<>::steal(jitfunc->invoke(pyfunc, nullptr, 0));
  ASSERT_NE(res, nullptr);
  ASSERT_EQ(PyLong_AsLong(res), 314159);
  EXPECT_FALSE(did_deopt);
  EXPECT_FALSE(patcher->isPatched());

  // Patch and verify that a deopt occurred
  patcher->patch();
  auto res2 = Ref<>::steal(jitfunc->invoke(pyfunc, nullptr, 0));
  jit_rt->clearGuardFailureCallback();
  ASSERT_NE(res2, nullptr);
  ASSERT_EQ(PyLong_AsLong(res2), 314159);
  EXPECT_TRUE(did_deopt);
  EXPECT_TRUE(patcher->isPatched());
}
