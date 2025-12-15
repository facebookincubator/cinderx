// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include "cinderx/python.h"

#include <gtest/gtest.h>

#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/pass.h"
#include "cinderx/Jit/hir/preload.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/module_state.h"

#define JIT_TEST_MOD_NAME "jittestmodule"

#define THROW(...)                                      \
  {                                                     \
    if (PyErr_Occurred()) {                             \
      PyErr_Print();                                    \
    }                                                   \
    throw std::runtime_error{fmt::format(__VA_ARGS__)}; \
  }

class RuntimeTest : public ::testing::Test {
 public:
  enum Flags {
    kJit = 1 << 0,
    // Use CinderX's bytecode compiler implemented in Python.
    kCinderCompiler = 1 << 1,
    // Use the Static Python bytecode compiler.
    kStaticCompiler = 1 << 2,
  };

  static constexpr Flags kDefaultFlags{
      static_cast<Flags>(kJit | kCinderCompiler)};

  RuntimeTest(Flags flags = kDefaultFlags) : flags_{flags} {
    // TASK(T190613453): Python compiler doesn't work with 3.12 yet.
    if constexpr (PY_VERSION_HEX >= 0x030C0000) {
      flags_ = static_cast<Flags>(flags_ & ~kCinderCompiler);
    }

    JIT_CHECK(
        !isCinderCompiler() || !isStaticCompiler(),
        "Cannot use both the static and the cinder compiler");
  }

  void SetUp() override {
    ASSERT_FALSE(jit::isJitUsable())
        << "Haven't called Py_Initialize yet but the JIT says it's enabled";

    bool jit = isJit();
    if (jit) {
      jit::getMutableConfig().force_init = true;
    }

    Py_Initialize();
    ASSERT_TRUE(Py_IsInitialized());

    auto mod_state = cinderx::getModuleState();
    ASSERT_NE(mod_state, nullptr) << "Could not load the CinderX module state, "
                                     "CinderX has not initialized properly";

    // Generally the first failure we see when the JIT is incorrectly
    // initialized is trying to use the code allocator and crashing, so check
    // that here.
    if (jit) {
      auto code_allocator = mod_state->codeAllocator();
      ASSERT_NE(code_allocator, nullptr)
          << "Configured to use the JIT but it wasn't initialized";
    }

    globals_ = isStaticCompiler() ? MakeGlobalsStrict() : MakeGlobals();
    ASSERT_NE(globals_, nullptr);

    isolated_preloaders_.emplace();
  }

  void TearDown() override {
    isolated_preloaders_.reset();

    globals_.reset();
    int result = Py_FinalizeEx();
    ASSERT_EQ(result, 0) << "Failed finalizing the interpreter";

    ASSERT_EQ(jit::getConfig().state, jit::State::kNotInitialized);
    ASSERT_FALSE(jit::isJitUsable())
        << "JIT should be disabled with Py_FinalizeEx";

    cinderx::ModuleState* mod_state = cinderx::getModuleState();
    ASSERT_EQ(mod_state, nullptr)
        << "Module state should be destroyed with Py_FinalizeEx";
  }

  void runCode(const char* src) {
    if (isCinderCompiler()) {
      runCinderCode(src);
    } else if (isStaticCompiler()) {
      runStaticCode(src);
    } else {
      runStockCode(src);
    }
  }

  void runCinderCode(const char* src) {
    runCodeModuleExec(src, "cinderx.compiler", "exec_cinder");
  }

  void runStaticCode(const char* src) {
    runCodeModuleExec(src, "cinderx.compiler.static", "exec_static");
  }

  // Compile code with the stock CPython bytecode compiler and then run it.
  void runStockCode(const char* src) {
    const char* filename = "fake_runtime_tests_filename.py";
    // Code isn't limited to being a single expression or statement.
    int start = Py_file_input;
    auto code = Ref<>::steal(Py_CompileString(src, filename, start));
    if (code == nullptr) {
      THROW("Failed to compile code using the CPython compiler");
    }
    auto result = Ref<>::steal(PyEval_EvalCode(code, globals_, globals_));
    if (result == nullptr) {
      THROW("Failed to execute code that was compiled by the CPython compiler");
    }
  }

  void runCodeModuleExec(
      const char* src,
      const char* compiler_module,
      const char* exec_fn) {
    auto compiler = Ref<>::steal(PyImport_ImportModule(compiler_module));
    if (compiler == nullptr) {
      THROW("Failed to load compiler module '{}'", compiler_module);
    }
    auto exec_static = Ref<>::steal(PyObject_GetAttrString(compiler, exec_fn));
    if (exec_static == nullptr) {
      THROW(
          "Failed to load function '{}' from compiler module '{}'",
          exec_fn,
          compiler_module);
    }
    auto src_code = Ref<>::steal(PyUnicode_FromString(src));
    if (src_code == nullptr) {
      THROW("Failed to convert code string into unicode object");
    }
    auto mod_name = Ref<>::steal(PyUnicode_FromString(JIT_TEST_MOD_NAME));
    if (mod_name == nullptr) {
      THROW("Failed to create unicode object for '{}'", JIT_TEST_MOD_NAME);
    }
    auto res = Ref<>::steal(PyObject_CallFunctionObjArgs(
        exec_static,
        src_code.get(),
        globals_.get(),
        globals_.get(),
        mod_name.get(),
        nullptr));
    if (res == nullptr) {
      THROW("Failed running compiler '{}:{}'", compiler_module, exec_fn);
    }
  }

  Ref<> compileAndGet(const char* src, const char* name) {
    runCode(src);
    return getGlobal(name);
  }

  Ref<> compileStaticAndGet(const char* src, const char* name) {
    runStaticCode(src);
    return getGlobal(name);
  }

  Ref<> compileStockAndGet(const char* src, const char* name) {
    runStockCode(src);
    return getGlobal(name);
  }

  Ref<> getGlobal(const char* name) {
    PyObject* obj = PyDict_GetItemString(globals_, name);
    if (obj == nullptr) {
      THROW("Failed to load global '{}' after compilation", name);
    }
    return Ref<>::create(obj);
  }

  ::testing::AssertionResult isIntEquals(BorrowedRef<> obj, long expected) {
    EXPECT_NE(obj, nullptr) << "object is null";
    EXPECT_TRUE(PyLong_CheckExact(obj)) << "object is not an exact int";
    int overflow;
    long result = PyLong_AsLongAndOverflow(obj, &overflow);
    EXPECT_EQ(overflow, 0) << "conversion to long overflowed";
    if (result == expected) {
      return ::testing::AssertionSuccess();
    }
    return ::testing::AssertionFailure()
        << "expected " << expected << " but found " << result;
  }

  Ref<> MakeGlobals() {
    auto module = Ref<>::steal(PyModule_New(JIT_TEST_MOD_NAME));
    if (module == nullptr) {
      return module;
    }
    auto globals = Ref<>::create(PyModule_GetDict(module));

    if (AddModuleWithBuiltins(module, globals)) {
      return Ref<>(nullptr);
    }
    return globals;
  }

  Ref<> MakeGlobalsStrict() {
    auto globals = Ref<>::steal(PyDict_New());
    if (globals == nullptr) {
      return globals;
    }
    if (PyDict_SetItemString(
            globals,
            "__name__",
            Ref<>::steal(PyUnicode_FromString(JIT_TEST_MOD_NAME)))) {
      return Ref<>(nullptr);
    }
    auto args = Ref<>::steal(PyTuple_New(1));
    if (args == nullptr) {
      return args;
    }
    if (PyTuple_SetItem(args, 0, globals.get())) {
      return Ref<>(nullptr);
    }
    Py_INCREF(globals.get());
    auto kwargs = Ref<>::steal(PyDict_New());
    if (kwargs == nullptr) {
      return kwargs;
    }
    auto module = Ref<>::steal(
        Ci_StrictModule_New(&Ci_StrictModule_Type, args.get(), kwargs.get()));
    if (module == nullptr) {
      return module;
    }
    auto dict = Ref<>::steal(PyDict_New());
    if (dict == nullptr) {
      return dict;
    }
    if (AddModuleWithBuiltins(module, globals)) {
      return Ref<>(nullptr);
    }
    return globals;
  }

  bool AddModuleWithBuiltins(BorrowedRef<> module, BorrowedRef<> globals) {
    // Look up the builtins module to mimic real code, rather than using its
    // dict.
    auto modules = CI_INTERP_IMPORT_FIELD(PyInterpreterState_Get(), modules);
    auto builtins = PyDict_GetItemString(modules, "builtins");
    if (PyDict_SetItemString(globals, "__builtins__", builtins) != 0 ||
        PyDict_SetItemString(modules, JIT_TEST_MOD_NAME, module) != 0) {
      return true;
    }
    return false;
  }

  std::unique_ptr<jit::hir::Function> buildHIR(
      BorrowedRef<PyFunctionObject> func);

  // Out param is a limitation of googletest.
  // See https://fburl.com/z3fzhl9p for more details.
  void CompileToHIR(
      const char* src,
      const char* func_name,
      std::unique_ptr<jit::hir::Function>& irfunc) {
    Ref<PyFunctionObject> func(compileAndGet(src, func_name));
    ASSERT_NE(func.get(), nullptr) << "failed creating function";

    irfunc = buildHIR(func);
  }

  void CompileToHIRStatic(
      const char* src,
      const char* func_name,
      std::unique_ptr<jit::hir::Function>& irfunc) {
    Ref<PyFunctionObject> func(compileStaticAndGet(src, func_name));
    ASSERT_NE(func.get(), nullptr) << "failed creating function";

    irfunc = buildHIR(func);
  }

  bool isStaticCompiler() const {
    return flags_ & kStaticCompiler;
  }

  bool isCinderCompiler() const {
    return flags_ & kCinderCompiler;
  }

  bool isJit() const {
    return flags_ & kJit;
  }

 private:
  Ref<> globals_;
  std::optional<jit::hir::IsolatedPreloaders> isolated_preloaders_;
  Flags flags_;
};

constexpr RuntimeTest::Flags operator|(
    RuntimeTest::Flags a,
    RuntimeTest::Flags b) {
  return static_cast<RuntimeTest::Flags>(
      static_cast<int>(a) | static_cast<int>(b));
}

class HIRTest : public RuntimeTest {
 public:
  HIRTest(
      Flags flags,
      bool src_is_hir,
      const std::string& src,
      const std::string& expected_hir)
      : RuntimeTest{flags},
        src_{src},
        expected_hir_{expected_hir},
        src_is_hir_{src_is_hir} {}

  void setPasses(std::vector<std::unique_ptr<jit::hir::Pass>> passes) {
    passes_ = std::move(passes);
  }

  void TestBody() override;

 private:
  std::vector<std::unique_ptr<jit::hir::Pass>> passes_;
  std::string src_;
  std::string expected_hir_;
  bool src_is_hir_;
};
