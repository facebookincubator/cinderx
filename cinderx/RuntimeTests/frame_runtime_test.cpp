// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

namespace cinderx {

using namespace cinderx::jit;

#if CINDER_CAN_IDENTIFY_JIT_FRAMES
class FrameRuntimeTest : public RuntimeTest {
 public:
  Ref<CompiledFunction> compile(const char* name) {
    Ref<PyFunctionObject> func{getGlobal(name)};
    EXPECT_EQ(compileFunction(func), Result::OK);
    return Ref<CompiledFunction>::create(context()->lookupFunc(func));
  }

  Context* context() {
    return static_cast<Context*>(getModuleState()->jit_context.get());
  }

  void detach(BorrowedRef<PyFunctionObject> func, CompiledFunction& compiled) {
    compiled.removeFunction(func);
    context()->forgetCode(func);
    EXPECT_EQ(context()->lookupCodeRuntime(func), nullptr);
  }

  void checkActiveFrame(bool expect_inline) {
    static PyMethodDef probe_def = {"probe", probe, METH_NOARGS, nullptr};
    Ref<> capsule = Ref<>::steal(PyCapsule_New(this, nullptr, nullptr));
    ASSERT_NE(capsule, nullptr);
    Ref<> probe_func = Ref<>::steal(PyCFunction_New(&probe_def, capsule));
    ASSERT_NE(probe_func, nullptr);
    runCode(
        expect_inline ? R"(
def helper():
    probe()
    return 42

def func():
    return helper()
)"
                      : R"(
def func():
    probe()
    return 42
)");
    Ref<PyFunctionObject> func{getGlobal("func")};
    ASSERT_EQ(PyDict_SetItemString(func->func_globals, "probe", probe_func), 0);
    expect_inline_ = expect_inline;
    compiled_ = compile("func");
    ASSERT_NE(compiled_, nullptr);
    Ref<> result = Ref<>::steal(PyObject_CallNoArgs(getGlobal("func")));
    EXPECT_TRUE(isIntEquals(result, 42));
    EXPECT_EQ(probes_, 1);
    compiled_.reset();
  }

 private:
  static PyObject* probe(PyObject* self, PyObject*) {
    auto* test =
        static_cast<FrameRuntimeTest*>(PyCapsule_GetPointer(self, nullptr));
    _PyInterpreterFrame* frame = currentFrame(PyThreadState_Get());
    test->probes_++;
    EXPECT_EQ(isInlinedFrame(frame), test->expect_inline_);
    EXPECT_EQ(jitFrameGetCodeRuntime(frame), test->compiled_->runtime());
    if (test->expect_inline_) {
      frame = frame->previous;
      EXPECT_FALSE(isInlinedFrame(frame));
      EXPECT_EQ(jitFrameGetCodeRuntime(frame), test->compiled_->runtime());
    }
    Ref<PyFunctionObject> func{test->getGlobal("func")};
    CodeRuntime* original_runtime = test->compiled_->runtime();
    test->detach(func, *test->compiled_);
    Ref<CompiledFunction> replacement = test->compile("func");
    EXPECT_NE(replacement, nullptr);
    if (replacement == nullptr) {
      Py_RETURN_NONE;
    }
    EXPECT_NE(replacement->runtime(), original_runtime);
    EXPECT_EQ(jitFrameGetCodeRuntime(frame), original_runtime);
    Ref<PyFrameObject> reified =
        Ref<PyFrameObject>::steal(PyThreadState_GetFrame(PyThreadState_Get()));
    EXPECT_NE(reified, nullptr);
    Py_RETURN_NONE;
  }

  Ref<CompiledFunction> compiled_;
  bool expect_inline_{false};
  int probes_{0};
};

TEST_F(FrameRuntimeTest, ActiveFrameUsesOriginalRuntimeAfterRebinding) {
  checkActiveFrame(false);
}

TEST_F(FrameRuntimeTest, InlinedFrameUsesRootCompilationRuntime) {
  if constexpr (!kLightweightFrames) {
    SKIP("Inlining requires lightweight frames");
  }
  checkActiveFrame(true);
}

TEST_F(FrameRuntimeTest, SuspendedGeneratorUsesOriginalRuntimeAfterRebinding) {
  runCode(R"(
def func():
    yield 1
    yield 2
)");
  Ref<CompiledFunction> compiled = compile("func");
  ASSERT_NE(compiled, nullptr);
  Ref<PyFunctionObject> func{getGlobal("func")};
  Ref<> gen = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(gen, nullptr);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyIter_Next(gen)), 1));
  auto* frame = generatorFrame(reinterpret_cast<PyGenObject*>(gen.get()));
  CodeRuntime* original_runtime = compiled->runtime();
  EXPECT_EQ(jitFrameGetCodeRuntime(frame), original_runtime);
  detach(func, *compiled);
  Ref<CompiledFunction> replacement = compile("func");
  ASSERT_NE(replacement, nullptr);
  EXPECT_NE(replacement->runtime(), original_runtime);
  EXPECT_EQ(jitFrameGetCodeRuntime(frame), original_runtime);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyIter_Next(gen)), 2));
  EXPECT_EQ(PyIter_Next(gen), nullptr);
  EXPECT_FALSE(PyErr_Occurred());
}
#endif

} // namespace cinderx
