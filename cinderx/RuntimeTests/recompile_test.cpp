// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <limits>

namespace cinderx {

using namespace cinderx::jit;
using namespace cinderx::jit::hir;

class RecompileTest : public RuntimeTest {
 protected:
  void SetUp() override {
    RuntimeTest::SetUp();
    saved_config_ = getConfig();
    getMutableConfig().recompile_threshold = 2;
  }

  void TearDown() override {
    getMutableConfig() = saved_config_;
    RuntimeTest::TearDown();
  }

  Config saved_config_;
};

TEST_F(RecompileTest, DisabledAndReplacementCompilationsOmitCounter) {
  Ref<PyFunctionObject> func = compileAndGet("def func(a): return a", "func");
  ASSERT_NE(func, nullptr);
  getMutableConfig().recompile_threshold = 0;
  auto initial = Compiler().compile(func);
  ASSERT_TRUE(initial.has_value());
  EXPECT_EQ(
      initial->hir_opcode_counts[static_cast<size_t>(
          Opcode::kLoadRecompileCounterAddr)],
      0);
  EXPECT_EQ(
      initial
          ->hir_opcode_counts[static_cast<size_t>(Opcode::kCallStaticRetVoid)],
      0);
  getMutableConfig().recompile_threshold = 2;
  auto replacement = Compiler().compile(func, CompileMode::kRecompile);
  ASSERT_TRUE(replacement.has_value());
  EXPECT_EQ(
      replacement->hir_opcode_counts[static_cast<size_t>(
          Opcode::kLoadRecompileCounterAddr)],
      0);
  EXPECT_EQ(
      replacement
          ->hir_opcode_counts[static_cast<size_t>(Opcode::kCallStaticRetVoid)],
      0);
}

TEST_F(RecompileTest, StaticCompilationOmitsCounter) {
  Ref<PyFunctionObject> func =
      compileStaticAndGet("def func(a: int) -> int: return a", "func");
  ASSERT_NE(func, nullptr);
  auto compiled = Compiler().compile(func);
  ASSERT_TRUE(compiled.has_value());
  EXPECT_EQ(
      compiled->hir_opcode_counts[static_cast<size_t>(
          Opcode::kLoadRecompileCounterAddr)],
      0);
  // This output is never installed, so release its runtime references here.
  compiled->owner = getContext();
  compiled->releaseRuntimeResources();
}

TEST_F(RecompileTest, CounterThresholdRoundTripsFullUnsignedRange) {
  auto func = HIRParser{}.parseHIR(R"(
fun test {
  bb 0 {
    v0 = LoadRecompileCounterAddr
    v1 = LoadConst<CUInt64[-1]>
    Return v1
  }
}
)");
  ASSERT_NE(func, nullptr);
  auto it = func->cfg.entry_block->begin();
  auto threshold = (++it)->as<LoadConst>().type();
  ASSERT_TRUE(threshold.hasIntSpec());
  EXPECT_EQ(
      threshold,
      Type::fromCUInt(std::numeric_limits<uint64_t>::max(), TCUInt64));
  EXPECT_EQ(
      static_cast<uint64_t>(threshold.intSpec()),
      std::numeric_limits<uint64_t>::max());
  auto text = HIRPrinter{}.toString(*func);
  auto roundtrip = HIRParser{}.parseHIR(text.c_str());
  ASSERT_NE(roundtrip, nullptr);
  EXPECT_EQ(HIRPrinter{}.toString(*roundtrip), text);
}

#if CINDER_CAN_IDENTIFY_JIT_FRAMES
class RecompileCodegenTest : public RecompileTest {
 protected:
  void SetUp() override {
    RecompileTest::SetUp();
    probe_count_ = 0;
    store_count_ = 0;
    expected_runtime_ = nullptr;
  }

  static void probe(PyFunctionObject* func) {
    ++probe_count_;
    auto* frame = currentFrame(PyThreadState_Get());
    EXPECT_EQ(jitFrameGetCodeRuntime(frame), expected_runtime_);
    EXPECT_EQ(jitFrameGetFunction(frame), func);
  }

  static void recordStore(uint64_t value) {
    ++store_count_;
    EXPECT_EQ(value, 1);
  }

  std::unique_ptr<Function> instrument(BorrowedRef<PyFunctionObject> func) {
    preloader_ = Preloader::make(func, makeFrameReifier(func->func_code));
    if (preloader_ == nullptr) {
      return nullptr;
    }
    auto irfunc = hir::buildHIR(*preloader_, getConfig().recompile_threshold);
    bool found_helper = false;
    for (auto& block : irfunc->cfg.blocks) {
      for (auto it = block.begin(); it != block.end();) {
        auto& instr = *it++;
        if (instr.isStoreField() &&
            instr.as<StoreField>()
                .receiver()
                ->instr()
                ->isLoadRecompileCounterAddr()) {
          auto* record = CallStaticRetVoid::create(
              1, reinterpret_cast<void*>(recordStore), "recordStore");
          record->setOperand(0, instr.as<StoreField>().value());
          record->insertAfter(instr);
        }
        if (!instr.isCallStaticRetVoid() ||
            instr.as<CallStaticRetVoid>().addr() !=
                reinterpret_cast<void*>(rt::checkRecompile)) {
          continue;
        }
        auto* arg = instr.getOperand(0);
        auto* replacement = CallStaticRetVoid::create(
            1, reinterpret_cast<void*>(probe), "probe");
        replacement->setOperand(0, arg);
        instr.replaceWith(*replacement);
        delete &instr;
        found_helper = true;
      }
    }
    if (!found_helper) {
      return nullptr;
    }
    Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);
    return irfunc;
  }

  Ref<CompiledFunction> generate(Function& irfunc) {
    auto gen = factory_(&irfunc);
    if (gen->getVectorcallEntry() == nullptr) {
      return nullptr;
    }
    gen->transferReferences();
    gen->codeRuntime()->transferReferences(irfunc.env.stealStrongReferences());
    preloader_.reset();
    CompiledFunctionData data;
    data.code = gen->getCodeBuffer();
    data.vectorcall_entry =
        reinterpret_cast<vectorcallfunc>(gen->getVectorcallEntry());
    data.stack_size = gen->getCompiledFunctionStackSize();
    data.spill_stack_size = gen->getCompiledFunctionSpillStackSize();
    data.runtime = gen->codeRuntime();
    data.code_patchers = std::move(irfunc.code_patchers);
#ifndef ENABLE_PREFORK_MODEL
    data.stable_storage = gen->takeStableStorage();
#endif
    expected_runtime_ = data.runtime;
    auto compiled =
        CompiledFunction::create(std::move(data), *getContext(), false);
    return compiled;
  }

  void TearDown() override {
    preloader_.reset();
    RecompileTest::TearDown();
  }

  std::unique_ptr<Preloader> preloader_;
  codegen::NativeGeneratorFactory factory_;
  static inline int probe_count_;
  static inline int store_count_;
  static inline CodeRuntime* expected_runtime_;
};

TEST_F(RecompileCodegenTest, InitialCompilationEmitsCounter) {
  Ref<PyFunctionObject> func = compileAndGet("def func(a): return a", "func");
  auto compiled = Compiler().compile(func);
  ASSERT_TRUE(compiled.has_value());
  EXPECT_EQ(
      compiled->hir_opcode_counts[static_cast<size_t>(
          Opcode::kLoadRecompileCounterAddr)],
      1);
  EXPECT_EQ(
      compiled
          ->hir_opcode_counts[static_cast<size_t>(Opcode::kCallStaticRetVoid)],
      1);
  EXPECT_EQ(
      compiled->hir_opcode_counts[static_cast<size_t>(Opcode::kCheckNeg)], 0);
}

TEST_F(RecompileCodegenTest, PollsEveryCallAtThresholdAndPreservesArguments) {
  Ref<PyFunctionObject> func = compileAndGet(
      R"(
def func(a, b, c, d, e, f, g, h):
    return (a, b, c, d, e, f, g, h)
)",
      "func");
  ASSERT_NE(func, nullptr);
  auto irfunc = instrument(func);
  ASSERT_NE(irfunc, nullptr);
  auto gen = generate(*irfunc);
  ASSERT_NE(gen, nullptr);
  auto entry = gen->vectorcallEntry();
  auto expected = Ref<>::steal(PyTuple_New(8));
  ASSERT_NE(expected, nullptr);
  PyObject* args[8];
  for (int i = 0; i < 8; ++i) {
    args[i] = PyLong_FromLong(i + 1000);
    ASSERT_NE(args[i], nullptr);
    PyTuple_SET_ITEM(expected.get(), i, args[i]);
  }
  for (int i = 0; i < 5; ++i) {
    auto result = Ref<>::steal(entry(func, args, 8, nullptr));
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(PyObject_RichCompareBool(result, expected, Py_EQ), 1);
    EXPECT_EQ(probe_count_, i);
    EXPECT_EQ(store_count_, 1);
  }
}

TEST_F(RecompileCodegenTest, ThresholdOnePollsEveryCallWithoutStores) {
  getMutableConfig().recompile_threshold = 1;
  Ref<PyFunctionObject> func = compileAndGet("def func(a): return a", "func");
  ASSERT_NE(func, nullptr);
  auto irfunc = instrument(func);
  ASSERT_NE(irfunc, nullptr);
  auto gen = generate(*irfunc);
  ASSERT_NE(gen, nullptr);
  auto arg = Ref<>::steal(PyLong_FromLong(1000));
  PyObject* args[] = {arg};
  for (int i = 1; i <= 5; ++i) {
    auto result = Ref<>::steal(gen->vectorcallEntry()(func, args, 1, nullptr));
    ASSERT_EQ(result, arg);
    EXPECT_EQ(probe_count_, i);
    EXPECT_EQ(store_count_, 0);
  }
}

#endif

} // namespace cinderx
