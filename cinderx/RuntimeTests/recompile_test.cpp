// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <array>
#include <limits>

#ifndef WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

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
class RecompileBackgroundTest : public RecompileTest {
 protected:
#ifdef ENABLE_PREFORK_MODEL
  void TearDown() override {
    // Immortal prefork compilations keep the test globals alive until the JIT
    // finalizes, so drop what they hold from _cinderx to let it finalize.
    runCode("globals().clear()");
    RecompileTest::TearDown();
  }
#endif

  Ref<> trackRuntime(BorrowedRef<CompiledFunction> compiled) {
    runCode("class RuntimeMarker: pass");
    auto marker = Ref<>::steal(PyObject_CallNoArgs(getGlobal("RuntimeMarker")));
    if (marker == nullptr) {
      return nullptr;
    }
    auto weakref = Ref<>::steal(PyWeakref_NewRef(marker, nullptr));
    compiled->runtime()->addReference(marker);
    return weakref;
  }

  Ref<> startDeoptimizedWorker() {
    getMutableConfig().recompile_threshold = 1;
    runCode(R"(
import threading
from cinderx.jit import disable, wait_for_background_compiles
entered = threading.Event()
release = threading.Event()
def func():
    wait_for_background_compiles()
    disable(deopt_all=True)
    entered.set()
    release.wait()
worker = threading.Thread(target=func)
)");
    Ref<PyFunctionObject> func{getGlobal("func")};
    if (compileFunction(func) != Result::OK) {
      return nullptr;
    }
    auto weakref = trackRuntime(getContext()->lookupFunc(func));
    runCode("worker.start()\nassert entered.wait(10)");
    return weakref;
  }

  void drain() {
    runCode(
        "from cinderx.jit import wait_for_background_compiles\n"
        "wait_for_background_compiles()\n");
  }
};

TEST_F(RecompileBackgroundTest, ThresholdPublishesToTriggeringFunction) {
  Ref<PyFunctionObject> func = compileAndGet("def func(a): return a", "func");
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  auto arg = Ref<>::steal(PyList_New(0));
  ASSERT_EQ(Ref<>::steal(PyObject_CallOneArg(func, arg)), arg);
  EXPECT_EQ(getContext()->lookupFunc(func), initial);
  ASSERT_EQ(Ref<>::steal(PyObject_CallOneArg(func, arg)), arg);
  drain();
  auto replacement =
      Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  ASSERT_NE(replacement, nullptr);
  ASSERT_NE(replacement, initial);
  EXPECT_EQ(func->vectorcall, replacement->vectorcallEntry());
  EXPECT_EQ(initial->numFunctions(), 0);
  EXPECT_EQ(replacement->numFunctions(), 1);
  EXPECT_EQ(replacement->runtime()->compilationData(), replacement->data());
  EXPECT_EQ(
      replacement->hirOpcodeCounts()[static_cast<size_t>(
          Opcode::kLoadRecompileCounterAddr)],
      0);
  EXPECT_EQ(
      replacement
          ->hirOpcodeCounts()[static_cast<size_t>(Opcode::kCallStaticRetVoid)],
      0);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(Ref<>::steal(PyObject_CallOneArg(func, arg)), arg);
  }
  drain();
  EXPECT_EQ(getContext()->lookupFunc(func), replacement);
  EXPECT_FALSE(getContext()->hasActiveCompile(CompilationKey{func}));
}

#ifndef ENABLE_PREFORK_MODEL
TEST_F(RecompileBackgroundTest, SpecializesAndCarriesBinaryOpProfile) {
  getMutableConfig().binary_op_caches = true;
  getMutableConfig().specialized_opcodes = false;
  getMutableConfig().recompile_threshold = 3;
  Ref<PyFunctionObject> func =
      compileAndGet("def func(a, b): return a + b", "func");
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  ASSERT_NE(initial, nullptr);
  EXPECT_EQ(
      initial->hirOpcodeCounts()[static_cast<size_t>(Opcode::kBinaryOp)], 1);
  EXPECT_EQ(
      initial->hirOpcodeCounts()[static_cast<size_t>(Opcode::kLongBinaryOp)],
      0);
  auto args = Ref<>::steal(Py_BuildValue("(ii)", 1, 2));
  for (int i = 0; i < 2; ++i) {
    EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallObject(func, args)), 3));
  }
  drain();
  EXPECT_EQ(getContext()->lookupFunc(func), initial);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallObject(func, args)), 3));
  drain();

  auto compiled = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  ASSERT_NE(compiled, nullptr);
  ASSERT_NE(compiled, initial);
  EXPECT_EQ(func->vectorcall, compiled->vectorcallEntry());
  EXPECT_EQ(initial->numFunctions(), 0);
  EXPECT_EQ(compiled->numFunctions(), 1);
  EXPECT_FALSE(getContext()->hasActiveCompile(CompilationKey{func}));
  for (int generation = 0; generation < 2; ++generation) {
    const auto& counts = compiled->hirOpcodeCounts();
    EXPECT_EQ(counts[static_cast<size_t>(Opcode::kBinaryOp)], 0);
    EXPECT_EQ(counts[static_cast<size_t>(Opcode::kLongBinaryOp)], 1);
    EXPECT_EQ(
        counts[static_cast<size_t>(Opcode::kLoadRecompileCounterAddr)], 0);
    auto profile = compiled->snapshotProfile();
    ASSERT_EQ(profile.entries().size(), 1);
    const auto* payload =
        profile.find<BinaryOpProfile>(profile.entries().begin()->first);
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(payload->left_type, TLongCompact);
    EXPECT_EQ(payload->right_type, TLongCompact);
    EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallObject(func, args)), 3));
    if (generation == 0) {
      scheduleRecompile(func, compiled);
      drain();
      auto next = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
      ASSERT_NE(next, compiled);
      compiled = std::move(next);
    }
  }

  auto strings = Ref<>::steal(Py_BuildValue("(ss)", "left", "right"));
  auto result = Ref<>::steal(PyObject_CallObject(func, strings));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(result, "leftright"), 0);
}

TEST_F(RecompileBackgroundTest, SpecializesInlinedBinaryOpFromProfile) {
  getMutableConfig().binary_op_caches = true;
  getMutableConfig().specialized_opcodes = false;
  getMutableConfig().recompile_threshold = 3;
  Ref<PyFunctionObject> func = compileAndGet(
      "def add(a, b): return a + b\ndef func(a, b): return add(a, b)", "func");
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  ASSERT_EQ(initial->inlinedFunctionsStats().num_inlined_functions, 1);
  auto args = Ref<>::steal(Py_BuildValue("(dd)", 1.5, 2.5));
  for (int i = 0; i < 3; ++i) {
    auto result = Ref<>::steal(PyObject_CallObject(func, args));
    ASSERT_NE(result, nullptr);
    EXPECT_DOUBLE_EQ(PyFloat_AsDouble(result), 4.0);
  }
  drain();
  auto compiled = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  ASSERT_NE(compiled, initial);
  EXPECT_EQ(compiled->inlinedFunctionsStats().num_inlined_functions, 1);
  EXPECT_EQ(
      compiled->hirOpcodeCounts()[static_cast<size_t>(Opcode::kBinaryOp)], 0);
  EXPECT_EQ(
      compiled->hirOpcodeCounts()[static_cast<size_t>(Opcode::kDoubleBinaryOp)],
      1);
  auto result = Ref<>::steal(PyObject_CallObject(func, args));
  ASSERT_NE(result, nullptr);
  EXPECT_DOUBLE_EQ(PyFloat_AsDouble(result), 4.0);
  auto strings = Ref<>::steal(Py_BuildValue("(ss)", "left", "right"));
  result = Ref<>::steal(PyObject_CallObject(func, strings));
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(PyUnicode_CompareWithASCIIString(result, "leftright"), 0);
}
#endif

// Prefork compilations are immortal, so their runtime references are not
// reclaimed.
#ifndef ENABLE_PREFORK_MODEL
TEST_F(RecompileBackgroundTest, PublicationPreservesAnExecutingOriginalBody) {
  runCode(R"(
import gc
from cinderx.jit import wait_for_background_compiles
def func(a):
    wait_for_background_compiles()
    gc.collect()
    return a
)");
  auto func = Ref<PyFunctionObject>::create(getGlobal("func"));
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = getContext()->lookupFunc(func);
  auto* runtime = initial->runtime();
  auto weakref = trackRuntime(initial);
  ASSERT_NE(weakref, nullptr);
  auto arg = Ref<>::steal(PyList_New(0));
  ASSERT_EQ(Ref<>::steal(PyObject_CallOneArg(func, arg)), arg);
  ASSERT_EQ(Ref<>::steal(PyObject_CallOneArg(func, arg)), arg);
  EXPECT_NE(getContext()->lookupFunc(func), initial);
  EXPECT_TRUE(runtime->compilationData()->retired);
  EXPECT_FALSE(runtime->isCleared());
  EXPECT_NE(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
  getContext()->processDeferredCleanup();
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
}
#endif

TEST_F(RecompileBackgroundTest, ExecutingRetiredDataKeepsTypeWatches) {
  getMutableConfig().recompile_threshold = 1;
  runCode(R"(
import gc
import cinderx.jit
class Watched:
    x = 1
def func():
    cinderx.jit.wait_for_background_compiles()
    gc.collect()
    Watched.x = 2
    return 1
)");
  Ref<PyFunctionObject> func{getGlobal("func")};
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = getContext()->lookupFunc(func);
  BorrowedRef<PyTypeObject> type{getGlobal("Watched")};
  auto name_obj = Ref<>::steal(PyUnicode_InternFromString("x"));
  ASSERT_NE(name_obj, nullptr);
  BorrowedRef<PyUnicodeObject> name{name_obj};
  auto target = Ref<>::steal(PyObject_GetAttr(type, name));
  ASSERT_NE(target, nullptr);
  auto patcher = std::make_unique<TypeAttrDeoptPatcher>(type, name, target);
  auto* watch = patcher.get();
  std::array<uint8_t, 8> scratch{};
  watch->linkJump(
      reinterpret_cast<uintptr_t>(scratch.data()),
      reinterpret_cast<uintptr_t>(scratch.data()));
  initial->runtime()->addReference(type);
  initial->runtime()->addReference(name_obj);
  initial->runtime()->addReference(target);
  initial->data()->code_patchers.emplace_back(std::move(patcher));
  getContext()->watchType(type, watch);

  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallNoArgs(func)), 1));
  getContext()->notifyTypeModified(type, type);
  EXPECT_TRUE(watch->isPatched());
  getContext()->processDeferredCleanup();
  runCode("Watched.x = 3");
  getContext()->notifyTypeModified(type, type);
}

#ifndef ENABLE_PREFORK_MODEL
TEST_F(RecompileBackgroundTest, ReplacementFrameDoesNotRetainOriginalRuntime) {
  Ref<PyFunctionObject> func = compileAndGet(
      "import gc\ndef func():\n    gc.collect()\n    return 1", "func");
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = getContext()->lookupFunc(func);
  auto weakref = trackRuntime(initial);
  ASSERT_NE(weakref, nullptr);
  scheduleRecompile(func, initial);
  drain();
  ASSERT_NE(getContext()->lookupFunc(func), initial);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallNoArgs(func)), 1));
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
}
#endif

#if CINDER_FRAME_HAS_JIT_MARKER
TEST_F(
    RecompileBackgroundTest,
    InterpreterHandoffRetainsOnlyExecutingGeneration) {
  getMutableConfig().recompile_threshold = 1;
  Ref<PyFunctionObject> func = compileAndGet(
      R"(
import gc
from cinderx.jit import disable, wait_for_background_compiles
def func():
    wait_for_background_compiles()
    disable(deopt_all=True)
    gc.collect()
    return 1
)",
      "func");
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = getContext()->lookupFunc(func);
  auto initial_weakref = trackRuntime(initial);
  ASSERT_NE(initial_weakref, nullptr);
  scheduleRecompile(func, initial);
  drain();
  auto executing = getContext()->lookupFunc(func);
  ASSERT_NE(executing, initial);
  auto weakref = trackRuntime(executing);
  ASSERT_NE(weakref, nullptr);
  scheduleRecompile(func, executing);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallNoArgs(func)), 1));
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(initial_weakref)), Py_None);
  EXPECT_NE(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
  getContext()->processDeferredCleanup();
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
  runCode("from cinderx.jit import enable\nenable()");
}

TEST_F(
    RecompileBackgroundTest,
    NestedInterpreterHandoffsRetainBothGenerations) {
  getMutableConfig().recompile_threshold = 1;
  runCode(R"(
import gc
from cinderx.jit import disable, enable, wait_for_background_compiles
def inner():
    wait_for_background_compiles()
    disable(deopt_all=True)
    gc.collect()
    assert inner_marker() is not None
    assert outer_marker() is not None
    return 1
def outer():
    wait_for_background_compiles()
    disable(deopt_all=True)
    enable()
    result = inner()
    gc.collect()
    assert inner_marker() is None
    assert outer_marker() is not None
    return result
)");
  Ref<PyFunctionObject> inner{getGlobal("inner")};
  Ref<PyFunctionObject> outer{getGlobal("outer")};
  ASSERT_EQ(compileFunction(inner), Result::OK);
  ASSERT_EQ(compileFunction(outer), Result::OK);
  auto inner_marker = trackRuntime(getContext()->lookupFunc(inner));
  auto outer_marker = trackRuntime(getContext()->lookupFunc(outer));
  ASSERT_NE(inner_marker, nullptr);
  ASSERT_NE(outer_marker, nullptr);
  ASSERT_EQ(
      PyDict_SetItemString(inner->func_globals, "inner_marker", inner_marker),
      0);
  ASSERT_EQ(
      PyDict_SetItemString(outer->func_globals, "outer_marker", outer_marker),
      0);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallNoArgs(outer)), 1));
  getContext()->processDeferredCleanup();
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(outer_marker)), Py_None);
  runCode("enable()");
}

TEST_F(
    RecompileBackgroundTest,
    AnotherThreadsInterpreterHandoffRetainsItsGeneration) {
  auto weakref = startDeoptimizedWorker();
  SCOPE_EXIT({ runCode("release.set()\nworker.join(10)"); });
  ASSERT_NE(weakref, nullptr);
  runCode("from cinderx.jit import enable\nenable()");
  Ref<PyFunctionObject> func{getGlobal("func")};
  auto replacement = getContext()->lookupFunc(func);
  auto replacement_marker = trackRuntime(replacement);
  ASSERT_NE(replacement_marker, nullptr);
  scheduleRecompile(func, replacement);
  drain();
  getContext()->processDeferredCleanup();
  EXPECT_NE(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(replacement_marker)), Py_None);
  runCode("release.set()\nworker.join(10)\nassert not worker.is_alive()");
  getContext()->processDeferredCleanup();
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
}

#ifndef WIN32
TEST_F(RecompileBackgroundTest, ForkWithAnotherThreadsInterpreterHandoff) {
  auto weakref = startDeoptimizedWorker();
  SCOPE_EXIT({ runCode("release.set()\nworker.join(10)"); });
  ASSERT_NE(weakref, nullptr);
  runCode("from cinderx.jit import enable\nenable()");
  PyOS_BeforeFork();
  pid_t child = fork();
  if (child == 0) {
    PyOS_AfterFork_Child();
    getContext()->processDeferredCleanup();
    // References held by vanished threads' C stacks remain owned in the child.
    _exit(Ref<>::steal(PyObject_CallNoArgs(weakref)) != Py_None ? 0 : 1);
  }
  PyOS_AfterFork_Parent();
  if (child == -1) {
    FAIL() << "fork() failed";
  }
  int status;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  getContext()->processDeferredCleanup();
  EXPECT_NE(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
}
#endif

#endif

#ifndef ENABLE_PREFORK_MODEL
TEST_F(RecompileBackgroundTest, SuspendedGeneratorKeepsRetiredRuntime) {
  Ref<PyFunctionObject> func =
      compileAndGet("def func():\n    yield 1\n    yield 2", "func");
  ASSERT_NE(func, nullptr);
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = getContext()->lookupFunc(func);
  auto weakref = trackRuntime(initial);
  ASSERT_NE(weakref, nullptr);
  auto generator = Ref<>::steal(PyObject_CallNoArgs(func));
  ASSERT_NE(generator, nullptr);
  scheduleRecompile(func, initial);
  drain();
  ASSERT_NE(getContext()->lookupFunc(func), initial);
  getContext()->processDeferredCleanup();
  EXPECT_NE(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyIter_Next(generator)), 1));
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyIter_Next(generator)), 2));
  EXPECT_EQ(PyIter_Next(generator), nullptr);
  EXPECT_FALSE(PyErr_Occurred());
  generator.reset();
  getContext()->processDeferredCleanup();
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
}

TEST_F(RecompileBackgroundTest, AnotherThreadsFrameKeepsRetiredRuntime) {
  runCode(R"(
import threading
entered = threading.Event()
release = threading.Event()
def func():
    entered.set()
    release.wait()
worker = threading.Thread(target=func)
)");
  Ref<PyFunctionObject> func{getGlobal("func")};
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = getContext()->lookupFunc(func);
  auto weakref = trackRuntime(initial);
  ASSERT_NE(weakref, nullptr);
  runCode("worker.start()\nassert entered.wait(10)");
  scheduleRecompile(func, initial);
  drain();
  EXPECT_NE(getContext()->lookupFunc(func), initial);
  auto replacement = getContext()->lookupFunc(func);
  auto replacement_weakref = trackRuntime(replacement);
  ASSERT_NE(replacement_weakref, nullptr);
  scheduleRecompile(func, replacement);
  drain();
  EXPECT_NE(getContext()->lookupFunc(func), replacement);
  getContext()->processDeferredCleanup();
  EXPECT_NE(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(replacement_weakref)), Py_None);
  runCode("release.set()\nworker.join(10)\nassert not worker.is_alive()");
  getContext()->processDeferredCleanup();
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
}
#endif

TEST_F(RecompileBackgroundTest, SiblingsRetainAndAdoptTheirOwnGeneration) {
  runCode(R"(
def make(value):
    def func():
        return value
    return func
first = make(1)
second = make(2)
)");
  auto first = Ref<PyFunctionObject>::create(getGlobal("first"));
  auto second = Ref<PyFunctionObject>::create(getGlobal("second"));
  ASSERT_EQ(compileFunction(first), Result::OK);
  ASSERT_EQ(compileFunction(second), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(first));
  for (int i = 0; i < 10; ++i) {
    scheduleRecompile(first, initial);
  }
  drain();
  auto replacement =
      Ref<CompiledFunction>::create(getContext()->lookupFunc(first));
  ASSERT_NE(replacement, initial);
  EXPECT_EQ(getContext()->lookupFunc(second), initial);
  EXPECT_EQ(
      getContext()->lookupCode(
          first->func_code, first->func_builtins, first->func_globals),
      replacement);
  // Immortal prefork compilations are omitted from GC traversal.
#ifndef ENABLE_PREFORK_MODEL
  struct VisitState {
    CompiledFunction* expected;
    bool found;
  } state{initial, false};
  auto visitor = [](PyObject* obj, void* arg) {
    auto* state = static_cast<VisitState*>(arg);
    state->found |= obj == reinterpret_cast<PyObject*>(state->expected);
    return 0;
  };
  ASSERT_EQ(Py_TYPE(second)->tp_traverse(second, visitor, &state), 0);
  EXPECT_TRUE(state.found);
#endif
  runCode("import cinderx.jit\ncinderx.jit.disable(deopt_all=True)\n");
  EXPECT_EQ(getContext()->deoptedCompile(second), initial);
  EXPECT_EQ(getContext()->deoptedCompile(first), replacement);
  runCode("cinderx.jit.enable()\n");
  EXPECT_EQ(getContext()->lookupFunc(second), initial);
  EXPECT_GE(PyGC_Collect(), 0);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallNoArgs(second)), 2));
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallNoArgs(second)), 2));
  drain();
  EXPECT_EQ(getContext()->lookupFunc(second), replacement);
  EXPECT_EQ(initial->numFunctions(), 0);
  EXPECT_EQ(replacement->numFunctions(), 2);
  EXPECT_TRUE(isIntEquals(Ref<>::steal(PyObject_CallNoArgs(first)), 1));
}

TEST_F(RecompileBackgroundTest, DiscardingStaleCandidatePreservesCurrentCode) {
  Ref<PyFunctionObject> func = compileAndGet("def func(a): return a", "func");
  Ref<PyFunctionObject> other =
      compileAndGet("def other(a): return None", "other");
  ASSERT_NE(func, nullptr);
  ASSERT_NE(other, nullptr);
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  CompilationKey key{func};
  auto data = Compiler().compile(func, CompileMode::kRecompile);
  ASSERT_TRUE(data.has_value());
  BackgroundCompileTask task{
      Ref<PyFunctionObject>::create(func),
      {},
      Ref<PyCodeObject>::create(func->func_code),
      Ref<PyDictObject>::create(func->func_builtins),
      Ref<PyDictObject>::create(func->func_globals),
      Ref<CompiledFunction>::create(initial),
      std::make_unique<CompiledFunctionData>(std::move(*data)),
      {}};
  ASSERT_EQ(PyObject_SetAttrString(func, "__code__", other->func_code), 0);
  getContext()->completeRecompile(task);
  EXPECT_EQ(
      getContext()->lookupCode(key.code, key.builtins, key.globals), initial);
  EXPECT_FALSE(getContext()->hasActiveCompile(key));
}

TEST_F(RecompileBackgroundTest, ConcurrentCallsShareOneReplacement) {
  Ref<PyFunctionObject> func = compileAndGet("def func(a): return a", "func");
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  runCode(R"(
from concurrent.futures import ThreadPoolExecutor
with ThreadPoolExecutor(max_workers=4) as executor:
    assert list(executor.map(func, range(200))) == list(range(200))
)");
  drain();
  auto replacement =
      Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  ASSERT_NE(replacement, initial);
  scheduleRecompile(func, initial);
  drain();
  EXPECT_EQ(getContext()->lookupFunc(func), replacement);
  EXPECT_FALSE(getContext()->hasActiveCompile(CompilationKey{func}));
  EXPECT_EQ(replacement->numFunctions(), 1);
}

#ifndef ENABLE_PREFORK_MODEL
TEST_F(RecompileBackgroundTest, RetiredEntryDoesNotRequestRecompile) {
  getMutableConfig().recompile_threshold = 1;
  Ref<PyFunctionObject> func = compileAndGet("def func(a): return a", "func");
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  const auto entry = initial->vectorcallEntry();
  auto data = Ref<CompiledFunctionData>::create(initial->data());
  initial->removeFunction(func);
  initial.reset();
  ASSERT_TRUE(data->retired);

  PyObject* args[] = {Py_True};
  auto result = Ref<>::steal(entry(func.getObj(), args, 1, nullptr));
  EXPECT_EQ(result, Py_True);
  drain();
  EXPECT_EQ(getContext()->lookupFunc(func), nullptr);
  EXPECT_FALSE(getContext()->hasActiveCompile(CompilationKey{func}));
  data.reset();
  getContext()->processDeferredCleanup();
}
#endif

TEST_F(RecompileBackgroundTest, CollectedTriggerAllowsAnotherReplacement) {
  auto globals = Ref<>::steal(PyDict_New());
  ASSERT_NE(globals, nullptr);
  ASSERT_EQ(
      PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()), 0);
  auto result = Ref<>::steal(PyRun_String(
      R"(
from types import FunctionType
def first(a): return a
second = FunctionType(first.__code__, globals())
)",
      Py_file_input,
      globals,
      globals));
  ASSERT_NE(result, nullptr);
  auto first =
      Ref<PyFunctionObject>::create(PyDict_GetItemString(globals, "first"));
  auto second =
      Ref<PyFunctionObject>::create(PyDict_GetItemString(globals, "second"));
  ASSERT_EQ(compileFunction(first), Result::OK);
  ASSERT_EQ(compileFunction(second), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(first));
  CompilationKey key{first};
  scheduleRecompile(first, initial);
  drain();
  ASSERT_NE(getContext()->lookupFunc(first), initial);
  auto weakref = Ref<>::steal(PyWeakref_NewRef(first, nullptr));
  ASSERT_EQ(PyDict_DelItemString(globals, "first"), 0);
  first.reset();
  preloaderManager().extract().clear();
  EXPECT_GE(PyGC_Collect(), 0);
  EXPECT_EQ(Ref<>::steal(PyObject_CallNoArgs(weakref)), Py_None);
  ASSERT_EQ(
      getContext()->lookupCode(key.code, key.builtins, key.globals), nullptr);
  EXPECT_EQ(getContext()->lookupFunc(second), initial);
  scheduleRecompile(second, initial);
  drain();
  EXPECT_NE(getContext()->lookupFunc(second), initial);
  EXPECT_FALSE(getContext()->hasActiveCompile(key));
}

TEST_F(RecompileBackgroundTest, CancellationReleasesQueuedReplacement) {
  Ref<PyFunctionObject> func = compileAndGet("def func(a): return a", "func");
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  CompilationKey key{func};
  auto& registry = getContext()->backgroundCompileRegistry();
  // Leave the request queued so cancellation does not depend on worker timing.
  registry.worker_started = true;
  scheduleRecompile(func, initial);
  EXPECT_EQ(registry.queue.size(), 1);
  EXPECT_TRUE(getContext()->hasActiveCompile(key));
  cancelBackgroundCompiles();
  EXPECT_TRUE(registry.queue.empty());
  EXPECT_EQ(registry.in_flight_count, 0);
  EXPECT_FALSE(getContext()->hasActiveCompile(key));
  EXPECT_EQ(func->vectorcall, initial->vectorcallEntry());
}

#ifndef WIN32
TEST_F(RecompileBackgroundTest, ForkDiscardsInheritedReplacementReservation) {
  Ref<PyFunctionObject> func = compileAndGet("def func(a): return a", "func");
  Ref<PyFunctionObject> foreground =
      compileAndGet("def foreground(): return None", "foreground");
  ASSERT_EQ(compileFunction(func), Result::OK);
  auto initial = Ref<CompiledFunction>::create(getContext()->lookupFunc(func));
  CompilationKey key{func};
  CompilationKey foreground_key{foreground};
  auto& registry = getContext()->backgroundCompileRegistry();
  ASSERT_TRUE(
      getContext()->addActiveCompile(key, CompilationOrigin::kBackground));
  ASSERT_TRUE(getContext()->addActiveCompile(foreground_key));
  registry.in_flight_count = 1;
  PyOS_BeforeFork();
  pid_t child = fork();
  if (child == 0) {
    PyOS_AfterFork_Child();
    if (getContext()->hasActiveCompile(key) ||
        !getContext()->hasActiveCompile(foreground_key) ||
        registry.in_flight_count != 0) {
      _exit(1);
    }
    scheduleRecompile(func, initial);
    drain();
    _exit(getContext()->lookupFunc(func) != initial ? 0 : 2);
  }
  PyOS_AfterFork_Parent();
  EXPECT_TRUE(getContext()->hasActiveCompile(key));
  EXPECT_TRUE(getContext()->hasActiveCompile(foreground_key));
  getContext()->removeActiveCompile(key);
  getContext()->removeActiveCompile(foreground_key);
  registry.in_flight_count = 0;
  if (child == -1) {
    FAIL() << "fork() failed";
  }
  int status;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

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
