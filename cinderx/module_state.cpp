// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/module_state.h"

#include "cinderx/Common/log.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "pycore_opcode_utils.h"
#endif

namespace cinderx {

namespace {

ModuleState* s_cinderx_state;

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
  return 0;
}

bool ModuleState::initBuiltinMembers() {
#if PY_VERSION_HEX >= 0x030C0000
  PyTypeObject* types[] = {
      &PyBool_Type,
      &PyBytes_Type,
      &PyByteArray_Type,
      &PyComplex_Type,
      &PyCode_Type,
      &PyDict_Type,
      &PyFloat_Type,
      &PyFrozenSet_Type,
      &PyList_Type,
      &PyLong_Type,
      Py_TYPE(Py_None),
      &PyProperty_Type,
      &PySet_Type,
      &PyTuple_Type,
      &PyUnicode_Type,
  };

  for (auto type : types) {
    PyObject* mro = type->tp_mro;
    if (mro == nullptr) {
      continue;
    }

    Ref<> type_members = Ref<>::steal(PyDict_New());
    if (type_members == nullptr) {
      return false;
    }
    for (Py_ssize_t i = 0; i < Py_SIZE(mro); i++) {
      PyTypeObject* base =
          reinterpret_cast<PyTypeObject*>(PyTuple_GetItem(mro, i));
      Py_ssize_t cur_mem = 0;
      PyObject *key, *value;
      Ref<> tp_dict = Ref<>::steal(PyType_GetDict(base));
      while (PyDict_Next(tp_dict, &cur_mem, &key, &value)) {
        if (PyDict_Contains(type_members, key)) {
          continue;
        }
        if (PyDict_SetItem(type_members, key, value) < 0) {
          return false;
        }
      }
    }

    builtin_members.emplace(type, std::move(type_members));
  }
#endif
  return true;
}

ModuleState* getModuleState() {
  return s_cinderx_state;
}

ModuleState* getModuleState(BorrowedRef<> mod) {
  return reinterpret_cast<ModuleState*>(PyModule_GetState(mod));
}

void setModuleState(BorrowedRef<> mod) {
  auto state = reinterpret_cast<cinderx::ModuleState*>(PyModule_GetState(mod));
  s_cinderx_state = state;
  state->cinderx_module = mod;

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
