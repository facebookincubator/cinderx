// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/runtime_offsets.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/instr_effects.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/function.h"
#include "cinderx/Jit/lir/generator.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/module_c_state.h"

#include <memory>

namespace cinderx::jit::hir {

TEST(FrameEvaluatorEffectsTest, CheckReadsStateWithoutInvokingPython) {
  Environment env;
  std::unique_ptr<Instr> check(CallStatic::create(
      0,
      env.allocateRegister(),
      reinterpret_cast<void*>(rt::isFrameEvaluatorCompatible),
      TCInt64,
      "isFrameEvaluatorCompatible"));
  EXPECT_EQ(memoryEffects(*check).may_store, AEmpty);
  EXPECT_FALSE(memoryEffects(*check).borrows_output);
  EXPECT_FALSE(hasArbitraryExecution(*check));
  // A hook can change between entries. This must never become a replayable,
  // common-subexpression-eliminated computation merely because it has no
  // Python side effects and no operands.
  EXPECT_FALSE(check->isReplayable());
}

TEST(FrameEvaluatorEffectsTest, OtherStaticCallsRemainConservative) {
  Environment env;
  std::unique_ptr<Instr> call(CallStatic::create(
      0, env.allocateRegister(), nullptr, TCInt64, "unknown"));
  EXPECT_EQ(memoryEffects(*call).may_store, AManagedHeapAny);
  EXPECT_TRUE(hasArbitraryExecution(*call));
  EXPECT_FALSE(call->isReplayable());
}

} // namespace cinderx::jit::hir

namespace cinderx::jit {

namespace {

struct EntryCapture {
  PyCodeObject* code;
  PyObject* value;
  PyObject* scale;
  _PyFrameEvalFunction previous;
  int calls{0};
  bool raw_arguments{false};
  bool initial_instruction{false};
  bool uninitialized_freevars{false};
};

EntryCapture* active_capture = nullptr;

PyObject* captureEntry(
    PyThreadState* tstate,
    _PyInterpreterFrame* frame,
    int throwflag) {
  auto& capture = *active_capture;
  if (frameCode(frame) == capture.code) {
    capture.calls++;
#if PY_VERSION_HEX >= 0x030E0000
    capture.raw_arguments =
        PyStackRef_AsPyObjectBorrow(frame->localsplus[0]) == capture.value &&
        PyStackRef_AsPyObjectBorrow(frame->localsplus[2]) == capture.scale;
    capture.initial_instruction =
        frame->instr_ptr == _PyCode_CODE(capture.code);
    capture.uninitialized_freevars = PyStackRef_IsNull(
        frame->localsplus[capture.code->co_nlocalsplus - 1]);
#else
    capture.raw_arguments = frame->localsplus[0] == capture.value &&
        frame->localsplus[2] == capture.scale;
    capture.initial_instruction =
        frame->prev_instr == _PyCode_CODE(capture.code) - 1;
    capture.uninitialized_freevars =
        frame->localsplus[capture.code->co_nlocalsplus - 1] == nullptr;
#endif
  }
  return capture.previous(tstate, frame, throwflag);
}

} // namespace

class FrameEvaluatorEntryTest : public RuntimeTest {};

TEST_F(FrameEvaluatorEntryTest, LoweringReloadsIdentityWithoutCallingHelper) {
#ifndef ENABLE_PEP523_HOOK
  GTEST_SKIP() << "requires PEP 523 evaluator admission";
#endif
  Ref<PyFunctionObject> function{
      compileAndGet("def target(value): return value\n", "target")};
  auto preloader = hir::Preloader::make(
      function, makeFrameReifier(function->func_code));
  ASSERT_NE(preloader, nullptr);
  auto ir = hir::HIRBuilder(
                *preloader, 0, nullptr, /*check_frame_evaluator=*/true)
                .buildHIR();
  Compiler::runPasses(*ir, PassConfig::kAllExceptInliner);
  codegen::Environ env;
  env.ctx = getContext();
  env.reifier = ir->env.reifier;
  env.code_rt = env.allocateCodeRuntime(ir->code, ir->builtins, ir->globals);
  auto lowered = lir::LIRGenerator(ir.get(), &env).translateFunction();
  bool seen_check = false;
  bool calls_helper = false;
  bool reloads_identity = false;
  bool runtime_evaluator_offset = false;
  int64_t expected_evaluator_offset = offsetof(PyInterpreterState, eval_frame);
#if PY_VERSION_HEX >= 0x030E0000
  expected_evaluator_offset += Ci_InterpPreDelta();
#endif
  for (const auto* block : lowered->basicBlocks()) {
    for (const auto& instruction : block->instructions()) {
      auto origin = instruction->origin();
      if (origin == nullptr || !origin->isCallStatic() ||
          origin->as<hir::CallStatic>().addr() !=
              reinterpret_cast<void*>(rt::isFrameEvaluatorCompatible)) {
        continue;
      }
      seen_check = true;
      calls_helper |= instruction->isCall();
      if (instruction->isLoad() &&
          instruction->memoryOrder() == lir::MemoryOrder::kRelaxed &&
          instruction->getInput(0)->isInd()) {
        runtime_evaluator_offset |=
            instruction->getInput(0)->getMemoryIndirect()->getOffset() ==
            expected_evaluator_offset;
      }
      if (instruction->isLoad() &&
          instruction->memoryOrder() == lir::MemoryOrder::kRelaxed &&
          instruction->getInput(0)->isMem()) {
        reloads_identity |= instruction->getInput(0)->getMemoryAddress() ==
            Ci_GetEvalFrameFuncStorage();
      }
    }
  }
  EXPECT_TRUE(seen_check);
  if constexpr (rt::kInlineFrameEvaluatorCheck) {
    EXPECT_FALSE(calls_helper);
    EXPECT_TRUE(reloads_identity);
    EXPECT_TRUE(runtime_evaluator_offset);
  } else {
    EXPECT_TRUE(calls_helper);
  }
#if !defined(__cpp_lib_atomic_ref) || __cpp_lib_atomic_ref < 201806L || \
    defined(CINDERX_DISABLE_FRAME_EVALUATOR_FAST_PATH)
  EXPECT_EQ(Ci_GetEvalFrameFuncStorage(), nullptr);
#endif
}

TEST_F(FrameEvaluatorEntryTest, NativeEntriesObserveIdentityRemovalAndRestore) {
#ifndef ENABLE_PEP523_HOOK
  GTEST_SKIP() << "requires PEP 523 evaluator admission";
#endif
  runCode(R"(
import cinderx.jit
def target(value, unused, *, scale=2):
    return value * scale + 5
cinderx.jit.force_compile(target)
assert cinderx.jit.is_jit_compiled(target)
)");
  auto target = getGlobal("target");
  auto args = Ref<>::steal(Py_BuildValue("(iO)", 7, Py_None));
  auto kwargs = Ref<>::steal(Py_BuildValue("{s:i}", "scale", 3));
  ASSERT_NE(args, nullptr);
  ASSERT_NE(kwargs, nullptr);
  auto interp = PyThreadState_Get()->interp;
  auto original_identity = Ci_GetEvalFrameFunc();
  EntryCapture capture{
      reinterpret_cast<PyCodeObject*>(PyFunction_GetCode(target)),
      PyTuple_GET_ITEM(args.get(), 0),
      PyDict_GetItemString(kwargs, "scale"),
      _PyInterpreterState_GetEvalFrameFunc(interp)};
  active_capture = &capture;
  _PyInterpreterState_SetEvalFrameFunc(interp, captureEntry);
  Ci_SetEvalFrameFunc(captureEntry);
  auto native = Ref<>::steal(PyObject_Call(target, args, kwargs));
  int calls_before_removal = capture.calls;
  Ci_SetEvalFrameFunc(nullptr);
  auto interpreted = Ref<>::steal(PyObject_Call(target, args, kwargs));
  int calls_after_removal = capture.calls;
  Ci_SetEvalFrameFunc(captureEntry);
  auto restored = Ref<>::steal(PyObject_Call(target, args, kwargs));
  int calls_after_restore = capture.calls;
  Ci_SetEvalFrameFunc(original_identity);
  _PyInterpreterState_SetEvalFrameFunc(interp, capture.previous);
  active_capture = nullptr;
  ASSERT_NE(native, nullptr);
  ASSERT_NE(interpreted, nullptr);
  ASSERT_NE(restored, nullptr);
  EXPECT_TRUE(isIntEquals(native, 26));
  EXPECT_TRUE(isIntEquals(interpreted, 26));
  EXPECT_TRUE(isIntEquals(restored, 26));
  EXPECT_EQ(calls_before_removal, 0);
  EXPECT_EQ(calls_after_removal, 1);
  EXPECT_EQ(calls_after_restore, 1);
}

TEST_F(FrameEvaluatorEntryTest, CellParametersHaveRawFreshEntryLayout) {
#ifndef ENABLE_PEP523_HOOK
  GTEST_SKIP() << "requires PEP 523 evaluator admission";
#endif
  runCode(R"(
import cinderx.jit
def factory(offset):
    def target(value, unused, *, scale=2):
        def inner():
            return value * scale + offset
        return inner()
    return target
target = factory(5)
cinderx.jit.force_compile(target)
assert cinderx.jit.is_jit_compiled(target)
)");
  auto target = getGlobal("target");
  auto args = Ref<>::steal(Py_BuildValue("(iO)", 7, Py_None));
  auto kwargs = Ref<>::steal(Py_BuildValue("{s:i}", "scale", 3));
  ASSERT_NE(args, nullptr);
  ASSERT_NE(kwargs, nullptr);
  auto interp = PyThreadState_Get()->interp;
  EntryCapture capture{
      reinterpret_cast<PyCodeObject*>(PyFunction_GetCode(target)),
      PyTuple_GET_ITEM(args.get(), 0),
      PyDict_GetItemString(kwargs, "scale"),
      _PyInterpreterState_GetEvalFrameFunc(interp)};
  active_capture = &capture;
  _PyInterpreterState_SetEvalFrameFunc(interp, captureEntry);
  auto result = Ref<>::steal(PyObject_Call(target, args, kwargs));
  _PyInterpreterState_SetEvalFrameFunc(interp, capture.previous);
  active_capture = nullptr;
  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(isIntEquals(result, 26));
  EXPECT_EQ(capture.calls, 1);
  EXPECT_TRUE(capture.raw_arguments);
  EXPECT_TRUE(capture.initial_instruction);
  EXPECT_TRUE(capture.uninitialized_freevars);
}

} // namespace cinderx::jit
