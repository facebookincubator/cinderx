// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/RuntimeTests/fixtures.h"

using namespace cinderx;
using namespace cinderx::jit;

class CodePatcherTest : public RuntimeTest {
 public:
  Ref<CompiledFunction> generateCode(codegen::NativeGenerator& ngen) {
    auto entry = ngen.getVectorcallEntry();
    if (entry == nullptr) {
      return nullptr;
    }
    std::span<const std::byte> code = ngen.getCodeBuffer();
    int stack_size = ngen.getCompiledFunctionStackSize();
    int spill_stack_size = ngen.getCompiledFunctionSpillStackSize();
    CompiledFunctionData data;
    data.code = code;
    data.vectorcall_entry = reinterpret_cast<vectorcallfunc>(entry);
    data.stack_size = stack_size;
    data.spill_stack_size = spill_stack_size;
    return CompiledFunction::create(std::move(data), false);
  }

 protected:
  asmjit::JitRuntime rt_;
};

class MyDeoptPatcher : public JumpPatcher {
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
  struct PatchpointMemory {
    uint64_t before;
    alignas(8) std::array<uint8_t, 8> patchpoint;
    uint64_t after;
  };

  const std::array<uint8_t, 8> unpatched{
      0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
  const std::array<uint8_t, 8> patched{
      0xef, 0xbe, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

  constexpr uint64_t kBefore = 0x123456789abcdef0;
  constexpr uint64_t kAfter = 0xfedcba9876543210;
  PatchpointMemory memory{kBefore, unpatched, kAfter};
  std::array<uint8_t, 2> bytes{0xef, 0xbe};

  CodePatcher patcher;
  EXPECT_FALSE(patcher.isLinked());
  EXPECT_FALSE(patcher.isPatched());

  patcher.link(reinterpret_cast<uintptr_t>(memory.patchpoint.data()), bytes);
  EXPECT_TRUE(patcher.isLinked());
  EXPECT_FALSE(patcher.isPatched());
  EXPECT_EQ(memory.before, kBefore);
  EXPECT_EQ(memory.patchpoint, unpatched);
  EXPECT_EQ(memory.after, kAfter);

  patcher.patch();
  EXPECT_TRUE(patcher.isPatched());
  EXPECT_EQ(memory.before, kBefore);
  EXPECT_EQ(memory.patchpoint, patched);
  EXPECT_EQ(memory.after, kAfter);

  patcher.unpatch();
  EXPECT_FALSE(patcher.isPatched());
  EXPECT_EQ(memory.before, kBefore);
  EXPECT_EQ(memory.patchpoint, unpatched);
  EXPECT_EQ(memory.after, kAfter);
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
  hir::BasicBlock* entry = irfunc->cfg.entry_block;
  std::vector<hir::BasicBlock*> postorder =
      irfunc->cfg.getPostOrderTraversal(entry);
  ASSERT_GT(postorder.size(), 0);
  hir::Instr* term = postorder[0]->getTerminator();
  ASSERT_NE(term, nullptr);
  ASSERT_TRUE(term->isReturn()) << *term;

  // Insert a patchpoint immediately before the return
  auto patcher = irfunc->allocateCodePatcher<MyDeoptPatcher>(123);
  EXPECT_EQ(patcher->id(), 123);
  auto patchpoint = hir::DeoptPatchpoint::create(patcher);
  patchpoint->insertBefore(*term);

  // Generate machine code and link the patcher
  Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);
  codegen::NativeGeneratorFactory factory;
  codegen::NativeGenerator ngen(irfunc.get(), factory);
  auto jitfunc = generateCode(ngen);
  ASSERT_NE(jitfunc, nullptr);
  EXPECT_TRUE(patcher->isLinked());
  EXPECT_TRUE(patcher->calledOnLink());
  EXPECT_FALSE(patcher->isPatched());
  EXPECT_FALSE(patcher->calledOnPatch());

  size_t deopts = 0;
  auto callback = [&deopts](const DeoptMetadata&) { deopts += 1; };
  Context* jit_ctx = getContext();
  jit_ctx->setGuardFailureCallback(callback);

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

  jit_ctx->clearGuardFailureCallback();
}
