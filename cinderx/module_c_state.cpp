// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/module_c_state.h"

#include "cinderx/Common/log.h"
#include "cinderx/module_state.h"

extern "C" {

#if PY_VERSION_HEX < 0x030F0000
vectorcallfunc Ci_PyFunction_Vectorcall;
#endif

PyObject* Ci_GetStaticTypeError(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->static_type_error.getObj() : nullptr;
}

PyObject* Ci_GetGenericInstCache(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->genericinst_cache.getObj() : nullptr;
}

void Ci_SetGenericInstCache(PyDictObject* cache) {
  cinderx::getModuleState()->genericinst_cache =
      Ref<PyDictObject>::create(cache);
}

void Ci_ClearGenericInstCache(void) {
  if (auto state = cinderx::getModuleState(); state != nullptr) {
    state->genericinst_cache.reset();
  }
}

PyObject* Ci_GetClassLoaderCache(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->classloader_cache.getObj() : nullptr;
}

void Ci_SetClassLoaderCache(PyDictObject* cache) {
  cinderx::getModuleState()->classloader_cache =
      Ref<PyDictObject>::create(cache);
}

void Ci_ClearClassLoaderCache(void) {
  if (auto state = cinderx::getModuleState(); state != nullptr) {
    state->classloader_cache.reset();
  }
}

PyObject* Ci_GetClassLoaderCacheModuleToKeys(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->classloader_cache_module_to_keys.getObj()
                          : nullptr;
}

void Ci_SetClassLoaderCacheModuleToKeys(PyDictObject* cache) {
  cinderx::getModuleState()->classloader_cache_module_to_keys =
      Ref<PyDictObject>::create(cache);
}

void Ci_ClearClassLoaderCacheModuleToKeys(void) {
  if (auto state = cinderx::getModuleState(); state != nullptr) {
    state->classloader_cache_module_to_keys.reset();
  }
}

int Ci_Watchers_WatchDict(PyObject* dict) {
  return cinderx::getModuleState()->watcher_state.watchDict(dict);
}

int Ci_Watchers_UnwatchDict(PyObject* dict) {
  return cinderx::getModuleState()->watcher_state.unwatchDict(dict);
}

int Ci_Watchers_WatchType(PyTypeObject* type) {
  return cinderx::getModuleState()->watcher_state.watchType(type);
}

int Ci_Watchers_UnwatchType(PyTypeObject* type) {
  return cinderx::getModuleState()->watcher_state.unwatchType(type);
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

  return cinderx::getModuleState()->cache_manager->getGlobalCache(
      builtins, globals, key);
}

PyObject** Ci_GetDictCache(PyObject* dict, PyObject* key) {
  return Ci_GetGlobalCache(dict, dict, key);
}

void Ci_free_jit_list_gen(PyGenObject* obj) {
  cinderx::getModuleState()->jit_gen_free_list->free(
      reinterpret_cast<PyObject*>(obj));
}

} // extern "C"
