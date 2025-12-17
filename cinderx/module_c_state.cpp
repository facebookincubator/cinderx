// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/module_c_state.h"

#include "cinderx/Common/log.h"
#include "cinderx/module_state.h"

extern "C" {

#if PY_VERSION_HEX < 0x030F0000
vectorcallfunc Ci_PyFunction_Vectorcall;
#endif

int Ci_Watchers_WatchDict(PyObject* dict) {
  return cinderx::getModuleState()->watcherState().watchDict(dict);
}

int Ci_Watchers_UnwatchDict(PyObject* dict) {
  return cinderx::getModuleState()->watcherState().unwatchDict(dict);
}

int Ci_Watchers_WatchType(PyTypeObject* type) {
  return cinderx::getModuleState()->watcherState().watchType(type);
}

int Ci_Watchers_UnwatchType(PyTypeObject* type) {
  return cinderx::getModuleState()->watcherState().unwatchType(type);
}

PyObject**
Ci_GetGlobalCache(PyObject* builtins, PyObject* globals, PyObject* key) {
  JIT_CHECK(
      PyDict_CheckExact(builtins),
      "Builtins should be a dict, but is actually a {}",
      Py_TYPE(builtins)->tp_name);
  JIT_CHECK(
      PyDict_CheckExact(globals),
      "Globals should be a dict, but is actually a {}",
      Py_TYPE(globals)->tp_name);
  JIT_CHECK(
      PyUnicode_CheckExact(key),
      "Dictionary key should be a string, but is actually a {}",
      Py_TYPE(key)->tp_name);

  return cinderx::getModuleState()->cacheManager()->getGlobalCache(
      builtins, globals, key);
}

PyObject** Ci_GetDictCache(PyObject* dict, PyObject* key) {
  return Ci_GetGlobalCache(dict, dict, key);
}

void Ci_free_jit_list_gen(PyGenObject* obj) {
  cinderx::getModuleState()->jitGenFreeList()->free(
      reinterpret_cast<PyObject*>(obj));
}

} // extern "C"
