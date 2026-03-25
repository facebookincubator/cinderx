// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/module_c_state.h"

#include "cinderx/Common/log.h"
#include "cinderx/Jit/config.h"
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

PyObject* Ci_GetValueCache(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->value_cache.getObj() : nullptr;
}

void Ci_SetValueCache(PyListObject* cache) {
  cinderx::getModuleState()->value_cache = Ref<PyListObject>::create(cache);
}

void Ci_ClearValueCache(void) {
  if (auto state = cinderx::getModuleState(); state != nullptr) {
    state->value_cache.reset();
  }
}

PyObject* Ci_GetValueIndices(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->value_indices.getObj() : nullptr;
}

void Ci_SetValueIndices(PyDictObject* indices) {
  cinderx::getModuleState()->value_indices = Ref<PyDictObject>::create(indices);
}

void Ci_ClearValueIndices(void) {
  if (auto state = cinderx::getModuleState(); state != nullptr) {
    state->value_indices.reset();
  }
}

int32_t Ci_GetTypeIndexOffset(void) {
  return cinderx::getModuleState()->type_index_offset;
}

void Ci_AddTypeIndexOffset(int32_t offset) {
  cinderx::getModuleState()->type_index_offset += offset;
}

PyObject* Ci_GetDlopenCache(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->dlopen_cache.getObj() : nullptr;
}

void Ci_SetDlopenCache(PyDictObject* cache) {
  cinderx::getModuleState()->dlopen_cache = Ref<PyDictObject>::create(cache);
}

PyObject* Ci_GetDlsymCache(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->dlsym_cache.getObj() : nullptr;
}

void Ci_SetDlsymCache(PyDictObject* cache) {
  cinderx::getModuleState()->dlsym_cache = Ref<PyDictObject>::create(cache);
}

PyObject* Ci_GetInvokeNativeHelper(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->invoke_native_helper.getObj() : nullptr;
}

void Ci_SetInvokeNativeHelper(PyFunctionObject* helper) {
  cinderx::getModuleState()->invoke_native_helper =
      Ref<PyFunctionObject>::create(helper);
}

PyObject* Ci_GetReturnNone(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->return_none.getObj() : nullptr;
}

void Ci_SetReturnNone(PyFunctionObject* obj) {
  cinderx::getModuleState()->return_none = Ref<PyFunctionObject>::create(obj);
}

PyObject* Ci_GetWeakrefCallback(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->weakref_callback.getObj() : nullptr;
}

void Ci_SetWeakrefCallback(PyCFunctionObject* cb) {
  cinderx::getModuleState()->weakref_callback =
      Ref<PyCFunctionObject>::create(cb);
}

bool Ci_GetSpAuditHookInstalled(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->sp_audit_hook_installed : false;
}

void Ci_SetSpAuditHookInstalled(bool installed) {
  cinderx::getModuleState()->sp_audit_hook_installed = installed;
}

PyObject* Ci_GetIndexErr(void) {
  auto state = cinderx::getModuleState();
  return state != nullptr ? state->indexerr.getObj() : nullptr;
}

void Ci_SetIndexErr(PyUnicodeObject* err) {
  cinderx::getModuleState()->indexerr = Ref<PyUnicodeObject>::create(err);
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

bool Ci_GetDelayAdaptiveCode(void) {
  return jit::getConfig().delay_adaptive_code;
}

void Ci_SetDelayAdaptiveCode(bool delay) {
  jit::getMutableConfig().delay_adaptive_code = delay;
}

uint64_t Ci_GetAdaptiveThreshold(void) {
  return jit::getConfig().adaptive_threshold;
}

void Ci_SetAdaptiveThreshold(uint64_t threshold) {
  jit::getMutableConfig().adaptive_threshold = threshold;
}

} // extern "C"
