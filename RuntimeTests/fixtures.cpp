// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif

#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/parser.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/hir/ssa.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/runtime.h"
#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove

std::unique_ptr<jit::hir::Function> RuntimeTest::buildHIR(
    BorrowedRef<PyFunctionObject> func) {
  auto funcs = jit::preloadFuncAndDeps(func);
  JIT_CHECK(!funcs.empty(), "Failed to preload function");
  auto preloader = jit::hir::preloaderManager().find(funcs.back());
  JIT_CHECK(
      preloader->code() == func->func_code,
      "Expecting the last function to compile to be the first one preloaded");
  return jit::hir::buildHIR(*preloader);
}

void HIRTest::TestBody() {
  using namespace jit::hir;

  std::string test_name = "<unknown>";
  const testing::TestInfo* info =
      testing::UnitTest::GetInstance()->current_test_info();
  if (info != nullptr) {
    test_name = fmt::format("{}:{}", info->test_suite_name(), info->name());
  }

  std::unique_ptr<Function> irfunc;
  if (src_is_hir_) {
    irfunc = HIRParser{}.ParseHIR(src_.c_str());
    ASSERT_FALSE(passes_.empty())
        << "HIR tests don't make sense without a pass to test";
    ASSERT_NE(irfunc, nullptr);
    ASSERT_TRUE(checkFunc(*irfunc, std::cout));
    reflowTypes(*irfunc);
  } else if (isStaticCompiler()) {
    ASSERT_NO_FATAL_FAILURE(CompileToHIRStatic(src_.c_str(), "test", irfunc));
  } else {
    ASSERT_NO_FATAL_FAILURE(CompileToHIR(src_.c_str(), "test", irfunc));
  }

  if (jit::g_dump_hir) {
    JIT_LOG("Initial HIR for {}:\n{}", test_name, *irfunc);
  }

  if (!passes_.empty()) {
    if (!src_is_hir_ &&
        !(passes_.size() == 1 &&
          std::string(passes_.at(0)->name()) == "@AllPasses")) {
      SSAify{}.Run(*irfunc);
      // Perform some straightforward cleanup on Python inputs to make the
      // output more reasonable. This implies that tests for the passes used
      // here are most useful as HIR-only tests.
      Simplify{}.Run(*irfunc);
      PhiElimination{}.Run(*irfunc);
    }
    for (auto& pass : passes_) {
      pass->Run(*irfunc);
    }
    ASSERT_TRUE(checkFunc(*irfunc, std::cout));
  }
  HIRPrinter printer;
  auto hir = printer.ToString(*irfunc.get());
  EXPECT_EQ(hir, expected_hir_);
}

void HIRJSONTest::TestBody() {
  using namespace jit::hir;

  std::unique_ptr<Function> irfunc;
  irfunc = HIRParser{}.ParseHIR(src_.c_str());
  ASSERT_NE(irfunc, nullptr);
  ASSERT_TRUE(checkFunc(*irfunc, std::cout));
  reflowTypes(*irfunc);

  nlohmann::json expected_json_obj;
  try {
    expected_json_obj = nlohmann::json::parse(expected_json_);
  } catch (nlohmann::json::exception&) {
    ASSERT_TRUE(false) << "Could not parse JSON input";
  }

  JSONPrinter printer;
  nlohmann::json result = printer.Print(irfunc->cfg);
  EXPECT_EQ(result, expected_json_obj);
}
