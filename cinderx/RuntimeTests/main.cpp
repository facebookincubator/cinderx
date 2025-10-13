// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#ifdef BUCK_BUILD
#include "cinderx/_cinderx-lib.h"
#endif

#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/hir/builtin_load_method_elimination.h"
#include "cinderx/Jit/hir/clean_cfg.h"
#include "cinderx/Jit/hir/copy_propagation.h"
#include "cinderx/Jit/hir/dead_code_elimination.h"
#include "cinderx/Jit/hir/dynamic_comparison_elimination.h"
#include "cinderx/Jit/hir/guard_removal.h"
#include "cinderx/Jit/hir/inliner.h"
#include "cinderx/Jit/hir/insert_update_prev_instr.h"
#include "cinderx/Jit/hir/phi_elimination.h"
#include "cinderx/Jit/hir/refcount_insertion.h"
#include "cinderx/Jit/hir/simplify.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/testutil.h"

#ifdef BUCK_BUILD
#include "tools/cxx/Resources.h"
#endif

#include <fmt/format.h>
#include <sys/resource.h>

#include <cstdlib>
#include <cstring>

namespace {

using jit::Compiler;
using jit::PassConfig;
using jit::hir::Function;
using jit::hir::Pass;

class AllPasses : public Pass {
 public:
  AllPasses() : Pass("@AllPasses") {}

  void Run(Function& irfunc) override {
    Compiler::runPasses(irfunc, PassConfig::kAll);
  }

  static std::unique_ptr<AllPasses> Factory() {
    return std::make_unique<AllPasses>();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(AllPasses);
};

using PassFactory = std::function<std::unique_ptr<Pass>()>;

class TestPassRegistry {
 public:
  TestPassRegistry() {
    addPass(jit::hir::RefcountInsertion::Factory);
    addPass(jit::hir::CopyPropagation::Factory);
    addPass(jit::hir::CleanCFG::Factory);
    addPass(jit::hir::DynamicComparisonElimination::Factory);
    addPass(jit::hir::PhiElimination::Factory);
    addPass(jit::hir::InlineFunctionCalls::Factory);
    addPass(jit::hir::Simplify::Factory);
    addPass(jit::hir::DeadCodeElimination::Factory);
    addPass(jit::hir::GuardTypeRemoval::Factory);
    addPass(jit::hir::BeginInlinedFunctionElimination::Factory);
    addPass(jit::hir::BuiltinLoadMethodElimination::Factory);
    if constexpr (PY_VERSION_HEX >= 0x030C0000) {
      addPass(jit::hir::InsertUpdatePrevInstr::Factory);
    }

    addPass(AllPasses::Factory);
  }

  std::unique_ptr<Pass> makePass(const std::string& name) {
    auto it = factories_.find(name);
    return it != factories_.end() ? it->second() : nullptr;
  }

  void addPass(const PassFactory& factory) {
    auto temp = factory();
    factories_.emplace(temp->name(), factory);
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestPassRegistry);

  std::unordered_map<std::string, PassFactory> factories_;
};

class SkipFixture : public ::testing::Test {
 public:
  void TestBody() override {
    GTEST_SKIP();
  }
};

void remap_txt_path(std::string& path) {
#ifdef BUCK_BUILD
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

#ifdef BAKED_IN_PYTHONPATH
#define _QUOTE(x) #x
#define QUOTE(x) _QUOTE(x)
#define _BAKED_IN_PYTHONPATH QUOTE(BAKED_IN_PYTHONPATH)
#endif

} // namespace

#ifdef BUCK_BUILD
PyMODINIT_FUNC PyInit__cinderx() {
  return _cinderx_lib_init();
}
#endif

void registerCinderX() {
#ifdef BUCK_BUILD
  try {
    boost::filesystem::path python_install =
        build::getResourcePath("cinderx/RuntimeTests/python_install");
    std::string python_version =
        fmt::format("python{}.{}", PY_MAJOR_VERSION, PY_MINOR_VERSION);
    boost::filesystem::path lib_path = python_install / "lib" / python_version;
    boost::filesystem::path lib_dynload_path = lib_path / "lib-dynload";
    std::string python_install_str =
        lib_path.string() + ":" + lib_dynload_path.string();
    setenv("PYTHONPATH", python_install_str.c_str(), 1);
  } catch (const std::exception&) {
    std::cerr << "Error: Failed to access bundled Python installation in buck "
                 "build, re-running usually fixes the issue\n";
    throw;
  }

  if (PyImport_AppendInittab("_cinderx", PyInit__cinderx) != 0) {
    PyErr_Print();
    throw std::runtime_error{"Could not add cinderx to inittab"};
  }
#endif
}

int main(int argc, char* argv[]) {
#ifdef BAKED_IN_PYTHONPATH
  setenv("PYTHONPATH", _BAKED_IN_PYTHONPATH, 1);
#endif

  registerCinderX();

  ::testing::InitGoogleTest(&argc, argv);

  // Needed for update_hir_expected.py to know which expected output to update.
  std::cout << "Python Version: " << PY_MAJOR_VERSION << "." << PY_MINOR_VERSION
            << '\n';

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
  register_test("dead_code_elimination_test.txt");
  register_test(
      "dead_code_elimination_and_simplify_test.txt",
      RuntimeTest::kStaticCompiler);
  register_test("builtin_load_method_elimination_test.txt");
  register_test("all_passes_test.txt");
  register_test("all_passes_static_test.txt", RuntimeTest::kStaticCompiler);
  register_test("native_calls_test.txt", RuntimeTest::kStaticCompiler);
  register_test("static_array_item_test.txt", RuntimeTest::kStaticCompiler);

  wchar_t* argv0 = Py_DecodeLocale(argv[0], nullptr);
  if (argv0 == nullptr) {
    std::cerr << "Py_DecodeLocale() failed to allocate\n";
    std::abort();
  }
  Py_SetProgramName(argv0);

  // Prevent any test failures due to transient pointer values.
  jit::setUseStablePointers(true);

  // Particularly with ASAN, we might need a really large stack size.
  struct rlimit rl;
  rl.rlim_cur = RLIM_INFINITY;
  rl.rlim_max = RLIM_INFINITY;
  setrlimit(RLIMIT_STACK, &rl);

  int result = RUN_ALL_TESTS();

  PyMem_RawFree(argv0);
  return result;
}
