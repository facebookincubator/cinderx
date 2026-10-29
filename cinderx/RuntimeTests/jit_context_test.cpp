// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/bytecode.h"
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
#include <vector>

namespace cinderx {

using namespace cinderx::jit;

using JITModuleTest = RuntimeTest;

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

std::vector<const hir::PreloadedImport*> importRecords(
    const hir::Preloader& preloader) {
  std::vector<const hir::PreloadedImport*> records;
  for (const auto& instr : BytecodeInstructionBlock{preloader.code()}) {
    if (instr.opcode() != IMPORT_NAME && instr.opcode() != EAGER_IMPORT_NAME) {
      continue;
    }
    if (const auto* info = preloader.preloadedImport(instr.baseOffset())) {
      EXPECT_EQ(info->opcode, instr.opcode());
      records.push_back(info);
    }
  }
  return records;
}

TEST_F(JITContextTest, PreloaderCanonicalImportsPopulateSiteRecords) {
  const char* src = R"(
import types

def func():
    import types
    from types import SimpleNamespace
    return types, SimpleNamespace
)";
  Ref<PyFunctionObject> func{compileAndGet(src, "func")};
  std::unique_ptr<hir::Preloader> preloader{hir::Preloader::make(func)};
  ASSERT_NE(preloader, nullptr);

  std::vector<const hir::PreloadedImport*> records = importRecords(*preloader);
  ASSERT_EQ(records.size(), 2);

  const hir::PreloadedImport* import_name = records[0];
  const hir::PreloadedImport* import_from = records[1];
  ASSERT_TRUE(import_name->from_members.empty());
  ASSERT_EQ(import_from->from_members.size(), 1);
  EXPECT_EQ(
      PyUnicode_CompareWithASCIIString(
          import_from->from_members[0].name, "SimpleNamespace"),
      0);

  for (const hir::PreloadedImport* info : records) {
    EXPECT_EQ(
        ftAtomicLoadPtrAcquire(*info->full_module.cache),
        info->full_module.expected);
    EXPECT_EQ(
        ftAtomicLoadPtrAcquire(*info->returned_module.cache),
        info->returned_module.expected);
    EXPECT_EQ(
        ftAtomicLoadPtrAcquire(*info->import_func.cache),
        info->import_func.expected);
    EXPECT_TRUE(PyDict_CheckExact(info->modules));
    EXPECT_TRUE(PyDict_CheckExact(info->source_module_dict));
  }
  EXPECT_EQ(
      ftAtomicLoadPtrAcquire(*import_from->from_members[0].value.cache),
      import_from->from_members[0].value.expected);
}

TEST_F(JITContextTest, PreloaderUnsafeImportFormsDoNotPopulateRecords) {
  const char* src = R"(
def replacement_import(*args, **kwargs):
    return None

def template():
    import types

custom = type(template)(
    template.__code__,
    {"__builtins__": {"__import__": replacement_import}},
)

def relative():
    from . import missing
)";
  runCode(src);

  for (const char* name : {"custom", "relative"}) {
    Ref<PyFunctionObject> func{getGlobal(name)};
    std::unique_ptr<hir::Preloader> preloader{hir::Preloader::make(func)};
    ASSERT_NE(preloader, nullptr);
    EXPECT_TRUE(importRecords(*preloader).empty()) << name;
  }

#if PY_VERSION_HEX >= 0x030F0000
  runCode(R"(
import dis

def lazy_template():
    import types

code = bytearray(lazy_template.__code__.co_code)
for instr in dis.get_instructions(lazy_template):
    if instr.opname == "IMPORT_NAME":
        code[instr.offset + 1] = (instr.arg & ~3) | 1
lazy = type(lazy_template)(
    lazy_template.__code__.replace(co_code=bytes(code)),
    globals(),
)
)");
  Ref<PyFunctionObject> lazy_func{getGlobal("lazy")};
  std::unique_ptr<hir::Preloader> lazy_preloader{
      hir::Preloader::make(lazy_func)};
  ASSERT_NE(lazy_preloader, nullptr);
  EXPECT_TRUE(importRecords(*lazy_preloader).empty());
#endif
}

TEST_F(JITContextTest, PreloaderDeclinesCinderXModuleImports) {
  const char* src = R"(
def import_module():
    import _cinderx

def import_member():
    from _cinderx import _next_or_sentinel
)";
  runCode(src);

  for (const char* name : {"import_module", "import_member"}) {
    Ref<PyFunctionObject> func{getGlobal(name)};
    std::unique_ptr<hir::Preloader> preloader{hir::Preloader::make(func)};
    ASSERT_NE(preloader, nullptr);
    EXPECT_TRUE(importRecords(*preloader).empty()) << name;
  }
}

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

TEST_F(JITContextTest, ReentrantCompileDuringCleanup) {
  runCode(R"(
import weakref
import cinderx.jit

def to_compile(x):
    return x + 1

callback_ran = False

@cinderx.jit.jit_suppress
def on_delete(ref):
    global callback_ran
    assert cinderx.jit.force_compile(to_compile)
    callback_ran = True

class Target:
    pass

target = Target()
ref = weakref.ref(target, on_delete)
assert not cinderx.jit.is_jit_compiled(to_compile)
)");

  PyThreadState* attached = PyThreadState_Get();
  {
    // Worker cleanup runs attached while its compile context is still active.
    ThreadedCompileContext compile;
    runCode("del target\n");
    EXPECT_EQ(PyThreadState_GetUnchecked(), attached);
    EXPECT_EQ(getGlobal("callback_ran"), Py_True);
    getContext()->finalizePendingCompiles();
    runCode("assert cinderx.jit.is_jit_compiled(to_compile)\n");
  }
  EXPECT_EQ(PyThreadState_GetUnchecked(), attached);
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
    // finalizePendingCompiles() on a background worker.
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
