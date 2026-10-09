// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/module_state.h"

#include "internal/pycore_object.h"

#include "cinderx/Common/fork_support.h"
#include "cinderx/Common/log.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "pycore_opcode_utils.h"
#endif
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
#include "internal/pycore_interpframe.h"
#endif

namespace cinderx {

ModuleState* s_cinderx_state;

namespace {

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
int noopReifier(_PyInterpreterFrame*, PyObject*) {
  return 0;
}

void initJITExecutableDealloc(ModuleState* state) {
  auto code = Ref<PyCodeObject>::steal(
      PyCode_NewEmpty("<cinderx>", "<jit_executable>", 0));
  JIT_CHECK(code != nullptr, "failed to create code object");
  JIT_THROW_IF(code == nullptr, "failed to create code object");
  auto executable =
      Ref<>::steal(PyUnstable_MakeJITExecutable(noopReifier, code, nullptr));
  JIT_THROW_IF(executable == nullptr, "failed to create JIT executable");
  state->jit_executable_dealloc = Py_TYPE(executable)->tp_dealloc;
  JIT_THROW_IF(
      !PyUnstable_JITExecutable_Check(executable),
      "CPython's internal headers replaced CinderX's "
      "PyUnstable_JITExecutable_Check");
}
#endif

} // namespace

int ModuleState::traverse(visitproc visit, void* arg) {
  Py_VISIT(static_type_error);
  Py_VISIT(genericinst_cache);
  Py_VISIT(classloader_cache);
  Py_VISIT(classloader_cache_module_to_keys);
  Py_VISIT(value_cache);
  Py_VISIT(value_indices);
  Py_VISIT(dlopen_cache);
  Py_VISIT(dlsym_cache);
  Py_VISIT(invoke_native_helper);
  Py_VISIT(return_none);
  Py_VISIT(weakref_callback);
  Py_VISIT(indexerr);
  Py_VISIT(builtin_next);
  Py_VISIT(object_getattribute);
  return 0;
}

int ModuleState::clear() {
  static_type_error.reset();
  genericinst_cache.reset();
  classloader_cache.reset();
  classloader_cache_module_to_keys.reset();
  value_cache.reset();
  value_indices.reset();
  dlopen_cache.reset();
  dlsym_cache.reset();
  invoke_native_helper.reset();
  return_none.reset();
  weakref_callback.reset();
  indexerr.reset();
  sys_clear_caches.reset();
  builtin_next.reset();
  object_getattribute.reset();
  return 0;
}

std::shared_ptr<HugePageArena> ModuleState::getSharedHugePageArena() {
  // A weak_ptr lets the arena be freed once the last SlabArena holding a strong
  // reference goes away, while still handing the same instance to any that are
  // created in the meantime.

  std::lock_guard<std::mutex> lock{mutex_};
  std::shared_ptr<HugePageArena> arena = huge_page_arena_.lock();
  if (arena == nullptr) {
    arena = std::make_shared<HugePageArena>();
    huge_page_arena_ = arena;
  }
  return arena;
}

void ModuleState::afterForkChild() {
  std::shared_ptr<HugePageArena> arena = huge_page_arena_.lock();
  if (arena != nullptr) {
    arena->afterForkChild();
  }

  for (auto& writer : perf_writers) {
    writer->afterForkChild();
  }
}

void ModuleState::atForkPrepare() {
  mutex_.lock();
  if (std::shared_ptr<HugePageArena> arena = huge_page_arena_.lock()) {
    arena->atForkPrepare();
  }
  if (symbolizer != nullptr) {
    symbolizer->atForkPrepare();
  }
}

void ModuleState::atForkParent() {
  if (symbolizer != nullptr) {
    symbolizer->atForkParent();
  }
  if (std::shared_ptr<HugePageArena> arena = huge_page_arena_.lock()) {
    arena->atForkParent();
  }
  mutex_.unlock();
}

void ModuleState::atForkChild() {
  if (std::shared_ptr<HugePageArena> arena = huge_page_arena_.lock()) {
    arena->atForkChild();
  }
  if (symbolizer != nullptr) {
    symbolizer->atForkChild();
  }
  resetMutexAfterFork(mutex_);
}

void ModuleState::joinCompileWorkers() {
  for (std::thread& worker : compile_worker_threads) {
    if (worker.joinable()) {
      worker.join();
    }
  }
  compile_worker_threads.clear();
}

ModuleState* getModuleState(BorrowedRef<> mod) {
  return reinterpret_cast<ModuleState*>(PyModule_GetState(mod));
}

void setModuleState(BorrowedRef<> mod) {
  auto state = reinterpret_cast<cinderx::ModuleState*>(PyModule_GetState(mod));
  s_cinderx_state = state;
  state->cinderx_module = mod;

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
  initJITExecutableDealloc(state);
#endif

#if PY_VERSION_HEX >= 0x030E0000
  Ci_common_consts[CONSTANT_ASSERTIONERROR] = PyExc_AssertionError;
  Ci_common_consts[CONSTANT_NOTIMPLEMENTEDERROR] = PyExc_NotImplementedError;
  Ci_common_consts[CONSTANT_BUILTIN_TUPLE] =
      reinterpret_cast<PyObject*>(&PyTuple_Type);
  PyObject* builtins = PyEval_GetBuiltins();
  Ci_common_consts[CONSTANT_BUILTIN_ALL] =
      PyDict_GetItemString(builtins, "all");
  JIT_CHECK(
      Ci_common_consts[CONSTANT_BUILTIN_ALL] != nullptr, "failed to get all");
  Ci_common_consts[CONSTANT_BUILTIN_ANY] =
      PyDict_GetItemString(builtins, "any");
  JIT_CHECK(
      Ci_common_consts[CONSTANT_BUILTIN_ANY] != nullptr, "failed to get any");
#if PY_VERSION_HEX >= 0x030F0000
  Ci_common_consts[CONSTANT_BUILTIN_LIST] =
      reinterpret_cast<PyObject*>(&PyList_Type);
  Ci_common_consts[CONSTANT_BUILTIN_SET] =
      reinterpret_cast<PyObject*>(&PySet_Type);
  Ci_common_consts[CONSTANT_NONE] = Py_GetConstantBorrowed(Py_CONSTANT_NONE);
  Ci_common_consts[CONSTANT_EMPTY_STR] =
      Py_GetConstantBorrowed(Py_CONSTANT_EMPTY_STR);
  Ci_common_consts[CONSTANT_TRUE] = Py_GetConstantBorrowed(Py_CONSTANT_TRUE);
  Ci_common_consts[CONSTANT_FALSE] = Py_GetConstantBorrowed(Py_CONSTANT_FALSE);
  Ci_common_consts[CONSTANT_MINUS_ONE] = PyLong_FromLong(-1);
#endif
#if PY_VERSION_HEX >= 0x03100000
  Ci_common_consts[CONSTANT_BUILTIN_FROZENSET] =
      reinterpret_cast<PyObject*>(&PyFrozenSet_Type);
  Ci_common_consts[CONSTANT_EMPTY_TUPLE] =
      Py_GetConstantBorrowed(Py_CONSTANT_EMPTY_TUPLE);
#endif
#endif
}

void removeModuleState() {
  s_cinderx_state = nullptr;
}

} // namespace cinderx

#if PY_VERSION_HEX >= 0x030E0000
PyObject* Ci_common_consts[NUM_COMMON_CONSTANTS];
#endif

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_LIGHTWEIGHT_FRAMES)
extern "C" destructor Ci_GetJITExecutableDealloc(void) {
  cinderx::ModuleState* state = cinderx::getModuleState();
  return state != nullptr ? state->jit_executable_dealloc : nullptr;
}
#endif
