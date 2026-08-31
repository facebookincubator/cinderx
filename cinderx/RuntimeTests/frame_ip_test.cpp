// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <memory>

namespace cinderx {

using namespace cinderx::jit;

// Reifying a JIT frame recovers the bytecode offset it is stopped at from the
// unit's current instruction pointer, which means finding where the native
// frame the unit is running in saved that address.
class FrameIPTest : public RuntimeTest {
 public:
  // Compile through the interpreter's own JIT context: reifying a frame looks
  // the unit's CodeRuntime up there, and in the prefork model the compiled
  // function is immortalized rather than recorded on the function object, so a
  // private context leaves nothing for that lookup to find.
  void compile(const char* name) {
    Ref<PyFunctionObject> func{getGlobal(name)};
    ASSERT_TRUE(PyFunction_Check(func)) << name << " is not a function";
    std::unique_ptr<hir::Preloader> preloader{
        hir::Preloader::make(func, makeFrameReifier(func->func_code))};
    ASSERT_NE(preloader, nullptr) << "Failed preloading " << name;
    auto jit_ctx = reinterpret_cast<CompilerContext<Compiler>*>(
        cinderx::getModuleState()->jit_context.get());
    ASSERT_NE(jit_ctx, nullptr) << "JIT context was not initialized";
    ASSERT_EQ(
        compilePreloaderImpl(jit_ctx, *preloader, Ref<>::create(func)).first,
        Result::OK)
        << "Failed compiling " << name;
  }

  Ref<> call(const char* name) {
    Ref<> func{getGlobal(name)};
    return Ref<>::steal(PyObject_CallNoArgs(func));
  }
};

namespace {

// Each function below reports the line it is stopped at relative to its own
// `def`, so the expected value stays correct no matter where in this file the
// source happens to sit.
const char* kSource = R"(
import sys

def line_in_caller(depth):
    return sys._getframe(depth).f_lineno

def plain():
    return line_in_caller(1) - plain.__code__.co_firstlineno

def generator():
    yield line_in_caller(1) - generator.__code__.co_firstlineno

def calls_generator():
    gen = inner_generator()
    line = next(gen)
    # Finish the generator here: left suspended, deferred reference counting
    # can keep it alive until the interpreter is torn down, by which point the
    # test has already released the compiled code it refers to.
    gen.close()
    return line - calls_generator.__code__.co_firstlineno

def inner_generator():
    yield line_in_caller(2)
)";

} // namespace

TEST_F(FrameIPTest, ReifyingRunningJitFrameReportsLineBeingExecuted) {
  runCode(kSource);
  compile("plain");

  EXPECT_TRUE(isIntEquals(call("plain"), 1));
}

TEST_F(FrameIPTest, ReifyingRunningJitGeneratorReportsLineBeingExecuted) {
  runCode(kSource);
  compile("generator");

  Ref<> gen = call("generator");
  ASSERT_NE(gen, nullptr);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyIter_Next(gen)), 1));
}

// A resumed generator runs with the frame pointer aimed at its heap-allocated
// data, so reaching the frame that called it means walking through a frame
// record that does not live on the stack.
TEST_F(FrameIPTest, ReifyingJitFrameBelowRunningGeneratorReportsItsOwnLine) {
  runCode(kSource);
  compile("calls_generator");
  compile("inner_generator");

  EXPECT_TRUE(isIntEquals(call("calls_generator"), 2));
}

} // namespace cinderx
