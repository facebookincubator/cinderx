// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#ifdef CINDERX_RUNTIME_TESTS_STATIC_CINDERX
#include "cinderx/_cinderx-lib.h"
#endif

#include "cinderx/Common/util.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/hir/clean_cfg.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/dead_code_elimination.h"
#include "cinderx/Jit/hir/dynamic_comparison_elimination.h"
#include "cinderx/Jit/hir/guard_removal.h"
#include "cinderx/Jit/hir/inliner.h"
#include "cinderx/Jit/hir/insert_update_prev_instr.h"
#include "cinderx/Jit/hir/load_method_elimination.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/refcount_insertion.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/Jit/hir/sink_primitive_box.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/testutil.h"

#ifdef CINDERX_RUNTIME_TESTS_USE_BUCK_RESOURCES
#include "tools/cxx/Resources.h"
#endif

#include <fmt/format.h>

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include <cstdlib>
#include <cstring>
#include <string>

#if defined(CINDERX_RUNTIME_TESTS_USE_BUCK_RESOURCES) && defined(_WIN32)
#include <filesystem>
#include <stdexcept>
#endif

namespace {

using namespace cinderx;
using namespace cinderx::jit;
using namespace cinderx::jit::hir;

class AllPasses : public Pass {
 public:
  AllPasses() : Pass("@AllPasses") {}

  void run(Function& irfunc) override {
    Compiler::runPasses(irfunc, PassConfig::kAll);
  }

  static std::unique_ptr<AllPasses> factory() {
    return std::make_unique<AllPasses>();
  }

  AllPasses(const AllPasses&) = delete;
  AllPasses& operator=(const AllPasses&) = delete;
};

using PassFactory = std::function<std::unique_ptr<Pass>()>;

class TestPassRegistry {
 public:
  TestPassRegistry() {
    addPass(RefcountInsertion::factory);
    addPass(CopyPropagation::factory);
    addPass(CleanCFG::factory);
    addPass(DynamicComparisonElimination::factory);
    addPass(PhiElimination::factory);
    addPass(InlineFunctionCalls::factory);
    addPass(Simplify::factory);
    addPass(SinkPrimitiveBox::factory);
    addPass(DeadCodeElimination::factory);
    addPass(GuardTypeRemoval::factory);
    addPass(BeginInlinedFunctionElimination::factory);
    addPass(LoadMethodElimination::factory);
    addPass(InsertUpdatePrevInstr::factory);

    addPass(AllPasses::factory);
  }

  std::unique_ptr<Pass> makePass(const std::string& name) {
    auto it = factories_.find(name);
    return it != factories_.end() ? it->second() : nullptr;
  }

  void addPass(const PassFactory& factory) {
    auto temp = factory();
    factories_.emplace(temp->name(), factory);
  }

  TestPassRegistry(const TestPassRegistry&) = delete;
  TestPassRegistry& operator=(const TestPassRegistry&) = delete;

 private:
  std::unordered_map<std::string, PassFactory> factories_;
};

class SkipFixture : public ::testing::Test {
 public:
  void TestBody() override {
    SKIP("Skipping from SkipFixture");
  }
};

void remap_txt_path(std::string& path) {
#ifdef CINDERX_RUNTIME_TESTS_USE_BUCK_RESOURCES
  boost::filesystem::path hir_tests_path =
      build::getResourcePath("cinderx/RuntimeTests/hir_tests");
  path = (hir_tests_path / path).string();
#else
  path = "RuntimeTests/hir_tests/" + path;
#endif
}

void register_test(
    std::string path,
    RuntimeTest::Flags extra_flags = RuntimeTest::Flags{}) {
#if defined(CINDERX_RUNTIME_TESTS_FEATURE_FREE)
  // Minimal builds do not support the full set of features assumed by Static
  // Python goldens.
  if (extra_flags & RuntimeTest::kStaticCompiler) {
    return;
  }
#endif

  remap_txt_path(path);
  auto suite = ReadHIRTestSuite(path.c_str());
  if (suite == nullptr) {
    std::exit(1);
  }
  auto pass_names = suite->pass_names;
  bool has_passes = !pass_names.empty();
  if (has_passes) {
    TestPassRegistry registry;
    for (auto& pass_name : pass_names) {
      auto pass = registry.makePass(pass_name);
      if (pass == nullptr) {
        std::cerr << "ERROR [" << path << "] Unknown pass name " << pass_name
                  << '\n';
        std::exit(1);
      }
    }
  }
  for (auto& test_case : suite->test_cases) {
    ::testing::RegisterTest(
        suite->name.c_str(),
        test_case.name.c_str(),
        nullptr,
        nullptr,
        __FILE__,
        __LINE__,
        [=]() -> ::testing::Test* {
          if (test_case.is_skip) {
            return new SkipFixture{};
          }
          auto test = new HIRTest(
              RuntimeTest::kJit | extra_flags,
              test_case.src_is_hir,
              test_case.src,
              test_case.expected);
          if (has_passes) {
            TestPassRegistry registry;
            std::vector<std::unique_ptr<Pass>> passes;
            for (auto& pass_name : pass_names) {
              passes.push_back(registry.makePass(pass_name));
            }
            test->setPasses(std::move(passes));
          }
          return test;
        });
  }
}

#define _QUOTE_HELPER(x) #x
#define _QUOTE(x) _QUOTE_HELPER(x)

#ifdef BAKED_IN_PYTHONPATH
#define _BAKED_IN_PYTHONPATH _QUOTE(BAKED_IN_PYTHONPATH)
#endif

#ifdef CINDERX_RUNTIME_TESTS_PYTHONPATH_PACKAGE
#define _CINDERX_RUNTIME_TESTS_PYTHONPATH_PACKAGE \
  _QUOTE(CINDERX_RUNTIME_TESTS_PYTHONPATH_PACKAGE)
#endif

} // namespace

#ifdef CINDERX_RUNTIME_TESTS_STATIC_CINDERX
PyMODINIT_FUNC PyInit__cinderx() {
  return _cinderx_lib_init();
}
#endif

#if defined(CINDERX_RUNTIME_TESTS_USE_BUCK_RESOURCES) && defined(_WIN32)
// Directory holding the interpreter this binary is linked against, found by
// asking the loader which module a Python symbol came from.
std::filesystem::path pythonInstallRootFromLoadedDll() {
  HMODULE python_dll = nullptr;
  if (!GetModuleHandleExA(
          GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCSTR>(&Py_Initialize),
          &python_dll)) {
    throw std::runtime_error{
        "Could not find the module exporting Py_Initialize"};
  }
  char dll_path[MAX_PATH];
  DWORD len = GetModuleFileNameA(python_dll, dll_path, MAX_PATH);
  if (len == 0 || len == MAX_PATH) {
    throw std::runtime_error{"Could not read the path of the Python DLL"};
  }
  return std::filesystem::path{dll_path}.parent_path();
}
#endif

void registerCinderX() {
#if defined(CINDERX_RUNTIME_TESTS_USE_BUCK_RESOURCES) && defined(_WIN32)
  // Windows links CinderX against the interpreter deployed on the host rather
  // than one Buck builds, so the matching stdlib is the one sitting beside the
  // loaded python DLL.  Locate it from the DLL rather than shipping a copy as
  // a resource: third-party's Windows distribution is an MSBuild genrule that
  // wants network access, far too heavy to pull into a unit test.
  std::filesystem::path python_install = pythonInstallRootFromLoadedDll();
  // Windows spells the stdlib layout Lib\ and DLLs\, and separates PATH-style
  // lists with ';'.  The cinderx package is ours rather than the deployed
  // interpreter's, so it comes from a resource as it does elsewhere.
  boost::filesystem::path cinderx_lib =
      build::getResourcePath("cinderx/RuntimeTests/cinderx_pythonlib");
  setEnvVar(
      "PYTHONPATH",
      (python_install / "Lib").string() + ";" +
          (python_install / "DLLs").string() + ";" + cinderx_lib.string());
#elif defined(CINDERX_RUNTIME_TESTS_USE_BUCK_RESOURCES)
  try {
    boost::filesystem::path python_install =
        build::getResourcePath("cinderx/RuntimeTests/python_install");
    std::string python_version = fmt::format(
        "python{}.{}{}",
        PY_MAJOR_VERSION,
        PY_MINOR_VERSION,
        kFreeThreadedBuild ? "t" : "");
    boost::filesystem::path lib_path = python_install / "lib" / python_version;
    boost::filesystem::path lib_dynload_path = lib_path / "lib-dynload";
    std::string python_install_str =
        lib_path.string() + ":" + lib_dynload_path.string();
    setEnvVar("PYTHONPATH", python_install_str);
  } catch (const std::exception&) {
    std::cerr << "Error: Failed to access bundled Python installation in buck "
                 "build, re-running usually fixes the issue\n";
    throw;
  }
#endif

#ifdef CINDERX_RUNTIME_TESTS_STATIC_CINDERX
  if (PyImport_AppendInittab("_cinderx", PyInit__cinderx) != 0) {
    PyErr_Print();
    throw std::runtime_error{"Could not add cinderx to inittab"};
  }
#endif
}

// In the prefork-model build CinderX intentionally immortalizes JIT-compiled
// objects, so they are never freed and LeakSanitizer reports them as leaks at
// exit, failing the test binary even though every gtest passes. Turn leak
// checking off for that build only; non-prefork builds keep leak detection.
// kPreforkModel is a constexpr, so this folds to a constant return as required
// by __lsan_is_turned_off().
extern "C" __attribute__((used)) int __lsan_is_turned_off() {
  return int{kPreforkModel};
}

int main(int argc, char* argv[]) {
#ifdef CINDERX_RUNTIME_TESTS_PYTHONPATH_PACKAGE
  // OSS path: point PYTHONPATH at the in-tree cinderx package so
  // RuntimeTest::SetUp() can explicitly import cinderx after Py_Initialize().
  setEnvVar("PYTHONPATH", _CINDERX_RUNTIME_TESTS_PYTHONPATH_PACKAGE);
#elif defined(BAKED_IN_PYTHONPATH)
  setEnvVar("PYTHONPATH", _BAKED_IN_PYTHONPATH);
#endif

  registerCinderX();

  ::testing::InitGoogleTest(&argc, argv);

  // Needed for update_hir_expected.py to know which expected output to update.
  std::cout << "Python Version: " << runtimeTestPythonVersion() << '\n';

  register_test("clean_cfg_test.txt");
  register_test("dynamic_comparison_elimination_test.txt");
  register_test("hir_builder_static_test.txt", RuntimeTest::kStaticCompiler);
  register_test("guard_type_removal_test.txt");
  register_test("inliner_test.txt");
  register_test("inliner_elimination_test.txt");
  register_test("inliner_static_test.txt", RuntimeTest::kStaticCompiler);
  register_test(
      "inliner_elimination_static_test.txt", RuntimeTest::kStaticCompiler);
  register_test("phi_elimination_test.txt");
  register_test("refcount_insertion_test.txt");
  register_test(
      "refcount_insertion_static_test.txt", RuntimeTest::kStaticCompiler);
  register_test("super_access_static_test.txt", RuntimeTest::kStaticCompiler);
  register_test("super_access_test.txt");
  register_test("simplify_test.txt");
  register_test("simplify_uses_guard_types.txt");
  register_test("simplify_static_test.txt", RuntimeTest::kStaticCompiler);
  register_test("sink_primitive_box_test.txt");
  register_test("dead_code_elimination_test.txt");
  register_test(
      "dead_code_elimination_and_simplify_test.txt",
      RuntimeTest::kStaticCompiler);
  register_test("load_method_elimination_test.txt");
  register_test("leaf_function_test.txt");
#if !defined(CINDERX_RUNTIME_TESTS_FEATURE_FREE)
  // These goldens depend on lightweight frames and symbolized call targets.
  register_test("all_passes_test.txt");
#endif
  register_test("all_passes_static_test.txt", RuntimeTest::kStaticCompiler);
  if constexpr (kOS == OS::kLinux) {
    // The goldens name the library to dlopen, and "libc.so.6" only exists on
    // Linux.
    register_test("native_calls_test.txt", RuntimeTest::kStaticCompiler);
  }
  register_test("static_array_item_test.txt", RuntimeTest::kStaticCompiler);

  cinderx::setPythonProgramName(argv[0]);

  // Prevent any test failures due to transient pointer values.
  setUseStablePointers(true);

#ifndef _WIN32
  // Particularly with ASAN, we might need a really large stack size.  Windows
  // fixes the stack reservation at link time instead, so there is nothing to
  // raise here.
  struct rlimit rl;
  rl.rlim_cur = RLIM_INFINITY;
  rl.rlim_max = RLIM_INFINITY;
  setrlimit(RLIMIT_STACK, &rl);
#endif

  return RUN_ALL_TESTS();
}
