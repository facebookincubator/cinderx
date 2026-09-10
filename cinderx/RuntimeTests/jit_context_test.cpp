// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/threaded_compile.h"
#include "cinderx/Jit/type_deopt_patchers.h"
#include "cinderx/RuntimeTests/fixtures.h"

#include <array>
#include <memory>

namespace cinderx {

using namespace cinderx::jit;

class JITContextTest : public RuntimeTest {
 public:
  void SetUp() override {
    RuntimeTest::SetUp();
    jit_ctx_ = std::make_unique<CompilerContext<Compiler>>();
    ASSERT_NE(jit_ctx_, nullptr) << "Failed creating jit context";
  }

  void TearDown() override {
    jit_ctx_.reset();
    RuntimeTest::TearDown();
  }

  std::unique_ptr<CompilerContext<Compiler>> jit_ctx_;
};

TEST_F(JITContextTest, UnwatchableBuiltins) {
  // This is a C++ test rather than in test_cinderjit so we can guarantee a
  // fresh runtime state with a watchable builtins dict when the test begins.
  const char* py_src = R"(
import builtins

def del_foo():
    global foo
    del foo

def func():
    foo
    builtins.__dict__[42] = 42
    del_foo()

foo = "hello"
)";

  Ref<PyFunctionObject> func(compileAndGet(py_src, "func"));
  std::unique_ptr<hir::Preloader> preloader(
      hir::Preloader::make(func, makeFrameReifier(func->func_code)));

  // compilePreloaderImpl() expects its caller to have already claimed the
  // compile under the GIL; the production entry points do that in
  // admitCompile(), so a direct call has to register the key itself.
  ASSERT_TRUE(jit_ctx_->addActiveCompile(
      CompilationKey{
          preloader->code(), preloader->builtins(), preloader->globals()}));

  auto [comp_result, unclaimed] = compilePreloaderImpl(
      jit_ctx_.get(), *preloader, Ref<PyFunctionObject>::create(func));
  ASSERT_EQ(comp_result, Result::OK);

  auto empty_tuple = Ref<>::steal(PyTuple_New(0));
  auto result = Ref<>::steal(PyObject_Call(func, empty_tuple, nullptr));
  ASSERT_EQ(result, Py_None);
}

// A watch deferred during threaded compilation must be validated before it is
// installed: the type may have changed after the compile checked its
// assumptions but before the watch was installed, in which case the watch
// would never fire for that change and the code must deopt eagerly instead.
TEST_F(JITContextTest, PendingTypeWatchPatchedWhenStale) {
  runCode("class Foo:\n    x = 1\n");
  Ref<> foo_obj = getGlobal("Foo");
  ASSERT_TRUE(PyType_Check(foo_obj.get()));
  BorrowedRef<PyTypeObject> foo{foo_obj.get()};
  Ref<> name_obj{Ref<>::steal(PyUnicode_InternFromString("x"))};
  ASSERT_NE(name_obj, nullptr);
  BorrowedRef<PyUnicodeObject> name{name_obj.get()};
  Ref<> target{Ref<>::steal(PyObject_GetAttr(foo_obj.get(), name_obj.get()))};
  ASSERT_NE(target, nullptr);

  TypeAttrDeoptPatcher patcher{foo, name, target};
  ASSERT_TRUE(patcher.assumptionsStillValid());

  std::array<uint8_t, 8> scratch{};
  patcher.linkJump(
      reinterpret_cast<uintptr_t>(scratch.data()),
      reinterpret_cast<uintptr_t>(scratch.data()));
  ASSERT_TRUE(patcher.isLinked());

  {
    ThreadedCompileContext compile;
    jit_ctx_->watchType(
        foo, &patcher, [&patcher] { return patcher.assumptionsStillValid(); });
    // Simulate the race: the type changes after the compile's checks but
    // before the watch is installed, so no notification fires. The compile
    // context stays active with the GIL held throughout, matching
    // finalizeMultiThreadedCompile() on a background worker.
    runCode("Foo.x = 2\n");
    ASSERT_FALSE(patcher.assumptionsStillValid());
    jit_ctx_->watchPendingTypes();
  }
  EXPECT_TRUE(patcher.isPatched());
}

TEST_F(JITContextTest, PendingTypeWatchInstalledWhenValid) {
  runCode("class Bar:\n    x = 1\n");
  Ref<> bar_obj = getGlobal("Bar");
  ASSERT_TRUE(PyType_Check(bar_obj.get()));
  BorrowedRef<PyTypeObject> bar{bar_obj.get()};
  Ref<> name_obj{Ref<>::steal(PyUnicode_InternFromString("x"))};
  ASSERT_NE(name_obj, nullptr);
  BorrowedRef<PyUnicodeObject> name{name_obj.get()};
  Ref<> target{Ref<>::steal(PyObject_GetAttr(bar_obj.get(), name_obj.get()))};
  ASSERT_NE(target, nullptr);

  TypeAttrDeoptPatcher patcher{bar, name, target};

  std::array<uint8_t, 8> scratch{};
  patcher.linkJump(
      reinterpret_cast<uintptr_t>(scratch.data()),
      reinterpret_cast<uintptr_t>(scratch.data()));

  {
    ThreadedCompileContext compile;
    jit_ctx_->watchType(
        bar, &patcher, [&patcher] { return patcher.assumptionsStillValid(); });
    jit_ctx_->watchPendingTypes();
  }
  EXPECT_FALSE(patcher.isPatched());
  // Detach so a later notification for the still-installed global watch
  // cannot reach the destroyed patcher.
  jit_ctx_->unwatch(&patcher);
}

// The generic TypeDeoptPatcher cannot re-derive its assumptions, so its
// creation site attaches a validator comparing the descriptor slots. A change
// to tp_descr_get between the compile's checks and the watch installation
// must eagerly deopt instead of installing a stale watch.
TEST_F(JITContextTest, PendingGenericWatchPatchedWhenSlotsChange) {
  runCode("class Qux:\n    pass\n");
  Ref<> qux_obj = getGlobal("Qux");
  ASSERT_TRUE(PyType_Check(qux_obj.get()));
  BorrowedRef<PyTypeObject> qux{qux_obj.get()};

  descrgetfunc descr_get = qux->tp_descr_get;
  descrsetfunc descr_set = qux->tp_descr_set;

  TypeDeoptPatcher patcher{qux};
  // Mirror simplifyLoadAttrGenericDescriptor: the validator lives in the
  // compilation env's side table, keyed by the patcher.
  hir::Environment env;
  env.setWatchValidator(&patcher, [qux, descr_get, descr_set] {
    return qux->tp_descr_get == descr_get && qux->tp_descr_set == descr_set;
  });

  std::array<uint8_t, 8> scratch{};
  patcher.linkJump(
      reinterpret_cast<uintptr_t>(scratch.data()),
      reinterpret_cast<uintptr_t>(scratch.data()));
  ASSERT_TRUE(patcher.isLinked());

  {
    ThreadedCompileContext compile;
    // Mirror linkDeoptPatchers: look the validator up in the env side table.
    jit_ctx_->watchType(qux, &patcher, env.watchValidator(&patcher));
    // Simulate the race: the slot changes without any notification firing.
    // PyProperty_Type's slot is a real, distinct descrgetfunc. The compile
    // context stays active with the GIL held, matching finalization on a
    // background worker.
    descrgetfunc saved_get = qux->tp_descr_get;
    qux->tp_descr_get = PyProperty_Type.tp_descr_get;
    ASSERT_NE(qux->tp_descr_get, descr_get);
    jit_ctx_->watchPendingTypes();
    qux->tp_descr_get = saved_get;
  }
  EXPECT_TRUE(patcher.isPatched());
}

} // namespace cinderx
