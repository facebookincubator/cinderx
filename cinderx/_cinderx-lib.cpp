// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/_cinderx-lib.h"

#include "internal/pycore_pystate.h"

#include "cinderx/CachedProperties/cached_properties.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/util.h"
#include "cinderx/Common/watchers.h"
#include "cinderx/Immortalize/immortalize.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/global_cache.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/runtime.h"
#include "cinderx/Jit/symbolizer.h"
#include "cinderx/python_runtime.h"
// NOLINTNEXTLINE(facebook-unused-include-check)
#include "cinderx/Shadowcode/shadowcode.h"
#include "cinderx/StaticPython/_static.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/descrobject_vectorcall.h"
#include "cinderx/StaticPython/methodobject_vectorcall.h"
#include "cinderx/StaticPython/objectkey.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/async_lazy_value.h"
#include "cinderx/module_state.h"

#ifdef ENABLE_PARALLEL_GC
#include "cinderx/ParallelGC/parallel_gc.h"
#endif

#ifdef ENABLE_XXCLASSLOADER
#include "cinderx/StaticPython/xxclassloader.h"
#endif

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#include "internal/pycore_shadow_frame.h"
#endif

#if PY_VERSION_HEX < 0x030D0000 && defined(ENABLE_EVAL_HOOK)
#include "cinder/hooks.h"
#endif

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_modsupport.h"
#endif

#include <dlfcn.h>

namespace {

/*
 * Misc. Python-facing utility functions.
 */

PyObject* clear_caches(PyObject* mod, PyObject*) {
  auto state = cinderx::getModuleState(mod);
  _PyCheckedDict_ClearCaches();
  _PyCheckedList_ClearCaches();
  _PyClassLoader_ClearValueCache();
  // We replace sys._clear_type_cache with our own function which
  // clears the caches, so we should call this too.
  if constexpr (PY_VERSION_HEX >= 0x030C0000) {
    auto sys_clear = state->sysClearCaches();
    if (sys_clear != nullptr) {
      Ref<> res =
          Ref<>::steal(PyObject_Vectorcall(sys_clear, nullptr, 0, nullptr));
      if (res == nullptr) {
        return nullptr;
      }
    }
  }
  Py_RETURN_NONE;
}

#if PY_VERSION_HEX < 0x030C0000
PyObject* clear_all_shadow_caches(PyObject*, PyObject*) {
  _PyShadow_FreeAll();
  Py_RETURN_NONE;
}
#endif

PyDoc_STRVAR(
    strict_module_patch_doc,
    "strict_module_patch(mod, name, value)\n\
Patch a field in a strict module\n\
Requires patching to be enabled");
PyObject* strict_module_patch(PyObject*, PyObject* args) {
  PyObject* mod;
  PyObject* name;
  PyObject* value;
  if (!PyArg_ParseTuple(args, "OUO", &mod, &name, &value)) {
    return nullptr;
  }
  if (Ci_do_strictmodule_patch(mod, name, value) < 0) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    strict_module_patch_delete_doc,
    "strict_module_patch_delete(mod, name)\n\
Delete a field in a strict module\n\
Requires patching to be enabled");
PyObject* strict_module_patch_delete(PyObject*, PyObject* args) {
  PyObject* mod;
  PyObject* name;
  if (!PyArg_ParseTuple(args, "OU", &mod, &name)) {
    return nullptr;
  }
  if (Ci_do_strictmodule_patch(mod, name, nullptr) < 0) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    strict_module_patch_enabled_doc,
    "strict_module_patch_enabled(mod)\n\
Gets whether patching is enabled on the strict module");
PyObject* strict_module_patch_enabled(PyObject*, PyObject* mod) {
  if (!Ci_StrictModule_Check(mod)) {
    PyErr_SetString(PyExc_TypeError, "expected strict module object");
    return nullptr;
  }
  if (Ci_StrictModule_GetDictSetter(mod) != nullptr) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyObject* clear_classloader_caches(PyObject*, PyObject*) {
  _PyClassLoader_ClearVtables();
  _PyClassLoader_ClearCache();
  _PyClassLoader_ClearGenericTypes();
  Py_RETURN_NONE;
}

PyObject* watch_sys_modules(PyObject*, PyObject*) {
  auto sys = Ref<>::steal(PyImport_ImportModule("sys"));
  if (sys == nullptr) {
    Py_RETURN_NONE;
  }

  auto modules = Ref<>::steal(PyObject_GetAttrString(sys, "modules"));
  if (modules == nullptr) {
    Py_RETURN_NONE;
  }
  if (Ci_Watchers_WatchDict(modules) < 0) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    has_parallel_gc_doc,
    "has_parallel_gc()\n\n"
    "Return True if the Parallel GC is available and supported.");

PyObject* has_parallel_gc(
    [[maybe_unused]] PyObject*,
    [[maybe_unused]] PyObject*) {
#ifdef ENABLE_PARALLEL_GC
  Py_RETURN_TRUE;
#else
  Py_RETURN_FALSE;
#endif
}

PyDoc_STRVAR(
    cinder_enable_parallel_gc_doc,
    "enable_parallel_gc(min_generation=2, num_threads=0)\n\
\n\
Enable parallel garbage collection for generations >= `min_generation`.\n\
\n\
Use `num_threads` threads to perform collection in parallel. When this value is\n\
0 the number of threads is half the number of processors.\n\
\n\
Calling this more than once has no effect. Call `cinder.disable_parallel_gc()`\n\
and then call this function to change the configuration.\n\
\n\
A ValueError is raised if the generation or number of threads is invalid.");

PyObject*
cinder_enable_parallel_gc(PyObject*, PyObject* args, PyObject* kwargs) {
  static char* argnames[] = {
      const_cast<char*>("min_generation"),
      const_cast<char*>("num_threads"),
      nullptr};

  int min_gen = 2;
  int num_threads = 0;

  if (!PyArg_ParseTupleAndKeywords(
          args, kwargs, "|ii", argnames, &min_gen, &num_threads)) {
    return nullptr;
  }

  if (min_gen < 0) {
    PyErr_SetString(PyExc_ValueError, "invalid generation");
    return nullptr;
  }

  if (num_threads < 0) {
    PyErr_SetString(PyExc_ValueError, "invalid num_threads");
    return nullptr;
  }

#ifdef ENABLE_PARALLEL_GC
  if (Cinder_EnableParallelGC(min_gen, num_threads) < 0) {
    return nullptr;
  }
  Py_RETURN_NONE;
#else
  PyErr_SetString(
      PyExc_RuntimeError, "Parallel GC is not supported in this build");
  return nullptr;
#endif
}

PyDoc_STRVAR(
    cinder_disable_parallel_gc_doc,
    "disable_parallel_gc()\n\
\n\
Disable parallel garbage collection.\n\
\n\
This only affects the next collection; calling this from a finalizer does not\n\
affect the current collection.");
PyObject* cinder_disable_parallel_gc(PyObject*, PyObject*) {
#ifdef ENABLE_PARALLEL_GC
  Cinder_DisableParallelGC();
#endif
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    cinder_get_parallel_gc_settings_doc,
    "get_parallel_gc_settings()\n\
\n\
Return the settings used by the parallel garbage collector or\n\
None if the parallel collector is not enabled.\n\
\n\
Returns a dictionary with the following keys when the parallel\n\
collector is enabled:\n\
\n\
    num_threads: Number of threads used.\n\
    min_generation: The minimum generation for which parallel gc is enabled.");
PyObject* cinder_get_parallel_gc_settings(PyObject*, PyObject*) {
#ifdef ENABLE_PARALLEL_GC
  return Cinder_GetParallelGCSettings();
#else
  Py_RETURN_NONE;
#endif
}

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_PARALLEL_GC)
PyDoc_STRVAR(
    cinder_get_threshold_doc,
    "get_threshold($module, /)\n"
    "--\n"
    "\n"
    "Return the current collection thresholds.");
PyObject* cinder_get_threshold(PyObject* mod, PyObject*) {
  PyInterpreterState* interp = PyInterpreterState_Get();
  struct _gc_runtime_state* gcstate = &interp->gc;
  // 3.14 always returns 0 for the oldest generation, but allows setting
  // it. Return the real value when parallel GC is enabled.
  return Py_BuildValue(
      "(iii)",
      gcstate->young.threshold,
      gcstate->old[0].threshold,
      Cinder_IsParallelGCEnabled() ? gcstate->old[1].threshold : 0);
}
#endif

PyDoc_STRVAR(
    cinder_immortalize_heap_doc,
    "immortalize_heap($module, /)\n"
    "--\n"
    "\n"
    "Immortalize all instances accessible through the GC roots.");
PyObject* cinder_immortalize_heap(PyObject* mod, PyObject* /* args */) {
  return immortalize_heap(mod);
}

PyDoc_STRVAR(
    cinder_is_immortal_doc,
    "is_immortal($module, obj, /)\n"
    "--\n"
    "\n"
    "Return True if the object is immortal, else return False.");
PyObject* cinder_is_immortal(PyObject* /* mod */, PyObject* obj) {
  return PyBool_FromLong(_Py_IsImmortal(obj));
}

PyObject* compile_perf_trampoline_pre_fork(PyObject* mod, PyObject*) {
#if ENABLE_PERF_TRAMPOLINE || PY_VERSION_HEX >= 0x030D0000
  if (!jit::perf::isPreforkCompilationEnabled()) {
    Py_RETURN_NONE;
  }

  PyUnstable_PerfTrampoline_SetPersistAfterFork(1);

  auto& perf_trampoline_worklist =
      cinderx::getModuleState(mod)->perfTrampolineWorklist();

  for (BorrowedRef<PyFunctionObject> func : perf_trampoline_worklist) {
    BorrowedRef<PyCodeObject> code = func->func_code;
    if (PyUnstable_PerfTrampoline_CompileCode(code) == -1) {
      JIT_LOG(
          "Failed to compile perf trampoline for function {}",
          jit::funcFullname(func));
    }
  }
  perf_trampoline_worklist.clear();
#endif

  Py_RETURN_NONE;
}

PyObject* is_compile_perf_trampoline_pre_fork_enabled(PyObject*, PyObject*) {
  if (jit::perf::isPreforkCompilationEnabled()) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

#if PY_VERSION_HEX >= 0x030C0000
PyDoc_STRVAR(
    cinder_delay_adaptive_doc,
    "delay_adaptive($module, delay, /)\n"
    "--\n"
    "\n"
    "Enables or disables delaying adaptive code until a function is hot.");
PyObject* cinder_delay_adaptive(PyObject* mod, PyObject* delay) {
#ifdef ENABLE_INTERPRETER_LOOP
  if (!PyBool_Check(delay)) {
    PyErr_SetString(PyExc_TypeError, "expected bool");
  }

  Ci_DelayAdaptiveCode = delay == Py_True;
#endif
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    cinder_set_adaptive_delay_doc,
    "set_adaptive_delay($module, delay, /)\n"
    "--\n"
    "\n"
    "Sets the adaptive delay");
PyObject* cinder_set_adaptive_delay(PyObject* mod, PyObject* delay) {
#ifdef ENABLE_INTERPRETER_LOOP
  if (!PyLong_Check(delay)) {
    PyErr_SetString(PyExc_TypeError, "expected long");
  }

  Ci_AdaptiveThreshold = PyLong_AsLong(delay);
#endif
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    cinder_get_adaptive_delay_doc,
    "get_adaptive_delay($module, /)\n"
    "--\n"
    "\n"
    "Gets the adaptive delay");
PyObject* cinder_get_adaptive_delay(PyObject* mod, PyObject*) {
#ifdef ENABLE_INTERPRETER_LOOP
  return PyLong_FromUnsignedLongLong(Ci_AdaptiveThreshold);
#else
  return PyLong_FromLong(-1);
#endif
}

#endif

// In 3.12+ we don't have a shadow-stack so there's no need for our own
// stack-walking functions.
#if PY_VERSION_HEX < 0x030C0000
typedef struct {
  PyObject* list;
  int hasError;
  int collectFrame;
} StackWalkState;

CiStackWalkDirective frame_data_collector(
    void* data,
    PyObject* fqname,
    PyCodeObject* code,
    int lineno,
    PyObject* pyframe) {
  PyObject* lineNoObj;
  int failed;

  StackWalkState* state = (StackWalkState*)data;
  if (fqname == nullptr) {
    fqname = ((PyCodeObject*)code)->co_qualname;
    if (!fqname || !PyUnicode_Check(fqname)) {
      fqname = ((PyCodeObject*)code)->co_name;
    }
  }
  PyObject* t = PyTuple_New(2 + state->collectFrame);
  if (t == nullptr) {
    goto fail;
  }
  lineNoObj = PyLong_FromLong(lineno);
  if (lineNoObj == nullptr) {
    Py_DECREF(t);
    goto fail;
  }
  PyTuple_SET_ITEM(t, 0, fqname);
  Py_INCREF(fqname);

  // steals ref
  PyTuple_SET_ITEM(t, 1, lineNoObj);

  if (state->collectFrame) {
    PyObject* o = pyframe;
    if (!o) {
      o = Py_None;
    }
    PyTuple_SET_ITEM(t, 2, o);
    Py_INCREF(o);
  }
  failed = PyList_Append(state->list, t);
  Py_DECREF(t);
  if (!failed) {
    return CI_SWD_CONTINUE_STACK_WALK;
  }
fail:
  state->hasError = 1;
  return CI_SWD_STOP_STACK_WALK;
}

PyObject* collect_stack(int collectFrame) {
  PyObject* stack = PyList_New(0);
  if (stack == nullptr) {
    return nullptr;
  }
  StackWalkState state = {
      .list = stack, .hasError = 0, .collectFrame = collectFrame};
  Ci_WalkAsyncStack(PyThreadState_GET(), frame_data_collector, &state);
  if (state.hasError || (PyList_Reverse(stack) != 0)) {
    Py_CLEAR(stack);
  }
  return stack;
}

PyObject* get_entire_call_stack_as_qualnames_with_lineno(PyObject*, PyObject*) {
  return collect_stack(0);
}

PyObject* get_entire_call_stack_as_qualnames_with_lineno_and_frame(
    PyObject*,
    PyObject*) {
  return collect_stack(1);
}
#endif

void ensurePyFunctionVectorcall(BorrowedRef<PyFunctionObject> func) {
#if PY_VERSION_HEX < 0x030F0000
  if (!Ci_PyFunction_Vectorcall) {
    // capture the original vectorcall function on the first function
    // creation
    Ci_PyFunction_Vectorcall = func->vectorcall;
  }
#endif
}

// Schedule a function to be JIT-compiled.  If that fails, then also try
// compiling a perf trampoline for the Python function.
void scheduleCompile(BorrowedRef<PyFunctionObject> func) {
  ensurePyFunctionVectorcall(func);

  bool scheduled =
      jit::shouldScheduleCompile(func) && jit::scheduleJitCompile(func);
  if (!scheduled && jit::perf::isPreforkCompilationEnabled()) {
    auto& perf_trampoline_worklist =
        cinderx::getModuleState()->perfTrampolineWorklist();
    perf_trampoline_worklist.emplace(func);
  }
}

extern "C" PyObject* PyAnextAwaitable_New(PyObject*, PyObject*);
// Replacement for builtins.anext which is aware of JIT generators
#if PY_VERSION_HEX >= 0x030C0000
static PyObject*
builtin_anext(PyObject* module, PyObject* const* args, Py_ssize_t nargs) {
  if (!_PyArg_CheckPositional("anext", nargs, 1, 2)) {
    return nullptr;
  }

  cinderx::ModuleState* state = cinderx::getModuleState(module);

  BorrowedRef<> aiterator;
  BorrowedRef<> default_value = nullptr;

  aiterator = args[0];
  if (nargs == 2) {
    default_value = args[1];
  }

  BorrowedRef<PyTypeObject> t;
  BorrowedRef<> awaitable;

  t = Py_TYPE(aiterator);
  if (t->tp_as_async == nullptr || t->tp_as_async->am_anext == nullptr) {
    PyErr_Format(
        PyExc_TypeError,
        "'%.200s' object is not an async iterator",
        t->tp_name);
    return nullptr;
  }

  awaitable = (*t->tp_as_async->am_anext)(aiterator);
  if (awaitable == nullptr) {
    return nullptr;
  }
  if (default_value == nullptr) {
    return awaitable;
  }

  BorrowedRef<> new_awaitable =
      jit::JitGen_AnextAwaitable_New(state, awaitable, default_value);
  Py_DECREF(awaitable);
  return new_awaitable;
}
#endif

/*
 * (De)initialization functions
 */

// Visit a Python function on CinderX module initialization.
int function_visitor(BorrowedRef<PyFunctionObject> func) {
  // Ensure the code object can track how often it is called.
  BorrowedRef<PyCodeObject> code = func->func_code;
  JIT_CHECK(
      !USE_CODE_EXTRA || codeExtra(code) != nullptr,
      "Failed to initialize extra data for {}",
      jit::funcFullname(func));

  // Schedule the function to be compiled if desired.
  scheduleCompile(func);

  return 1;
}

// Visit a Python object on CinderX module initialization.
int object_visitor(PyObject* obj, [[maybe_unused]] void* arg) {
  if (PyFunction_Check(obj)) {
    return function_visitor(reinterpret_cast<PyFunctionObject*>(obj));
  }

  return 1;
}

// Visit every Python object on CinderX module initialization.
void init_existing_objects() {
  PyUnstable_GC_VisitObjects(object_visitor, nullptr);
}

std::unique_ptr<PyGetSetDef[]> s_func_getset;
std::unique_ptr<PyGetSetDef[]> s_class_method_getset;
std::unique_ptr<PyGetSetDef[]> s_method_getset;

// Count the number of elements in a PyGetSetDef array.
size_t getsetLen(PyGetSetDef* getset) {
  size_t len = 0;
  for (PyGetSetDef* def = getset; def->name != nullptr; ++def) {
    ++len;
  }
  return len;
}

// Override the getset array for a type with a new one that contains an extra
// typed signature getter.
void getsetOverride(
    PyTypeObject* type,
    std::unique_ptr<PyGetSetDef[]>& targetArray,
    getter typeSigGetter) {
  constexpr std::string_view kGetterName{"__typed_signature__"};

  PyGetSetDef* original = type->tp_getset;
  size_t len = getsetLen(original);

  // Might be re-initializing CinderX, when that happens the typed signature
  // getters are already installed.
  if (original == targetArray.get()) {
    PyGetSetDef* member = &original[len - 1];
    JIT_CHECK(
        member->name == kGetterName && member->get == typeSigGetter,
        "PyTypeObject should already have typed signature getter");
    return;
  }

  // Need two extra spots, one for the new getter and another that acts as a
  // null terminator.
  size_t newLen = len + 2;

  // Allocate a new array, keeping the original argument array around because it
  // still needs to be read from.
  auto newArray = std::make_unique<PyGetSetDef[]>(newLen);
  memset(newArray.get(), 0, newLen * sizeof(PyGetSetDef));
  memcpy(newArray.get(), original, len * sizeof(PyGetSetDef));

  // Tack on the signature getter.
  PyGetSetDef* def = &newArray[len];
  def->name = kGetterName.data();
  def->get = typeSigGetter;

  // Override the type's getset array and assign it to global scope.
  targetArray = std::move(newArray);
  type->tp_getset = targetArray.get();

  // Assign a descr for the new getter.  Will abort on failure as there's no way
  // to recover right now.
  auto descr = Ref<>::steal(PyDescr_NewGetSet(type, def));
  JIT_CHECK(
      descr != nullptr, "Failed to create descr for typed signature getter");
  BorrowedRef<> dict = _PyType_GetDict(type);
  JIT_CHECK(
      PyDict_SetDefault(dict, PyDescr_NAME(descr.get()), descr.get()) !=
          nullptr,
      "Failed to assign typed signature descr on type");

  PyType_Modified(type);
}

void init_already_existing_types() {
  // Update getset functions for callable types to include typed signature
  // getters.
  //
  // NB: This persists after cinderx is unloaded.  Ideally we would put the
  // original arrays back.
  if constexpr (PY_VERSION_HEX < 0x030E0000) {
    getsetOverride(
        &PyCFunction_Type,
        s_func_getset,
        reinterpret_cast<getter>(Ci_meth_get__typed_signature__));
    getsetOverride(
        &PyClassMethodDescr_Type,
        s_class_method_getset,
        reinterpret_cast<getter>(Ci_method_get_typed_signature));
    getsetOverride(
        &PyMethodDescr_Type,
        s_method_getset,
        reinterpret_cast<getter>(Ci_method_get_typed_signature));
  }
}

#if PY_VERSION_HEX < 0x030C0000
void shadowcode_code_sizeof(struct _PyShadowCode* shadow, Py_ssize_t* res) {
  *res += sizeof(_PyShadowCode);
  *res += sizeof(PyObject*) * shadow->l1_cache.size;
  *res += sizeof(PyObject*) * shadow->cast_cache.size;
  *res += sizeof(PyObject**) * shadow->globals_size;
  *res +=
      sizeof(_PyShadow_InstanceAttrEntry**) * shadow->polymorphic_caches_size;
  *res += sizeof(_FieldCache) * shadow->field_cache_size;
  *res += sizeof(_Py_CODEUNIT) * shadow->len;
}
#endif

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
int get_current_code_flags(PyThreadState* tstate) {
#if PY_VERSION_HEX < 0x030C0000
  PyCodeObject* cur_code = nullptr;
  Ci_WalkStack(
      tstate,
      [](void* ptr, PyCodeObject* code, int) {
        PyCodeObject** topmost_code = (PyCodeObject**)ptr;
        *topmost_code = code;
        return CI_SWD_STOP_STACK_WALK;
      },
      &cur_code);
  if (!cur_code) {
    return -1;
  }
  return cur_code->co_flags;
#else
  return _PyFrame_GetCode(currentFrame(tstate))->co_flags;
#endif
}

int cinderx_code_watcher(PyCodeEvent event, PyCodeObject* co) {
  switch (event) {
    case PY_CODE_EVENT_CREATE:
      break;
    case PY_CODE_EVENT_DESTROY:
#if PY_VERSION_HEX < 0x030C0000
      _PyShadow_ClearCache((PyObject*)co);
#endif
      jit::codeDestroyed(co);
      break;
    default:
      break;
  }

  return 0;
}

int cinderx_dict_watcher(
    PyDict_WatchEvent event,
    PyObject* dict_obj,
    PyObject* key_obj,
    PyObject* new_value) {
  JIT_DCHECK(PyDict_Check(dict_obj), "Expecting dict from dict watcher");
  BorrowedRef<PyDictObject> dict{dict_obj};

  auto state = cinderx::getModuleState();
  jit::IGlobalCacheManager* globalCaches =
      state != nullptr ? state->cacheManager() : nullptr;

  switch (event) {
    case PyDict_EVENT_ADDED:
    case PyDict_EVENT_MODIFIED:
    case PyDict_EVENT_DELETED: {
      _PyClassLoader_NotifyDictChange(dict, event, key_obj, new_value);

      if (globalCaches == nullptr) {
        return 0;
      }
      if (key_obj == nullptr || !PyUnicode_CheckExact(key_obj)) {
        globalCaches->notifyDictUnwatch(dict);
        break;
      }
      // key is overwhemingly likely to be interned, since in normal code it
      // comes from co_names. If it's not, we at least know that an interned
      // string with its value exists (because we're watching it), so this
      // should just be a quick lookup.
      if (!PyUnicode_CHECK_INTERNED(key_obj)) {
        Py_INCREF(key_obj);
        PyUnicode_InternInPlace(&key_obj);
        Py_DECREF(key_obj);
      }
      BorrowedRef<PyUnicodeObject> key{key_obj};
      globalCaches->notifyDictUpdate(dict, key, new_value);
      break;
    }
    case PyDict_EVENT_CLEARED:
      if (globalCaches != nullptr) {
        globalCaches->notifyDictClear(dict);
      }
      break;
    case PyDict_EVENT_CLONED:
      if (globalCaches != nullptr) {
        globalCaches->notifyDictUnwatch(dict);
      }
      break;
    case PyDict_EVENT_DEALLOCATED:
      _PyClassLoader_NotifyDictChange(dict, event, key_obj, new_value);
      if (globalCaches != nullptr) {
        globalCaches->notifyDictUnwatch(dict);
      }
      break;
  }

  return 0;
}

int cinderx_func_watcher(
    PyFunction_WatchEvent event,
    PyFunctionObject* func,
    PyObject* new_value) {
  switch (event) {
    case PyFunction_EVENT_CREATE:
      // Using Ci_PyFunction_Vectorcall before calling scheduleCompile, so we
      // need to ensure that the global variable is defined.
      ensurePyFunctionVectorcall(func);
      // Update the new function's vectorcall to have it run with Static Python
      // if it needs to.
      func->vectorcall = getInterpretedVectorcall(func);
      scheduleCompile(func);
      break;
    case PyFunction_EVENT_MODIFY_CODE:
      jit::funcModified(func);
      // having deopted the func, we want to immediately consider recompiling.
      // func_set_code will assign this again later, but we do it early so
      // scheduleCompile() can consider the new code object now.
      Py_INCREF(new_value);
      Py_XSETREF(func->func_code, new_value);
      scheduleCompile(func);
      break;
    case PyFunction_EVENT_MODIFY_DEFAULTS:
      break;
    case PyFunction_EVENT_MODIFY_KWDEFAULTS:
      break;
#ifdef ENABLE_FUNC_EVENT_MODIFY_QUALNAME
    case PyFunction_EVENT_MODIFY_QUALNAME:
      // allow reconsideration of whether this function should be compiled
      if (!isJitCompiled(func)) {
        // func_set_qualname will assign this again, but we need to assign it
        // now so that CiSetJITEntryOnPyFunctionObject can consider the new
        // qualname.
        Py_INCREF(new_value);
        Py_XSETREF(func->func_qualname, new_value);
        scheduleCompile(func);
      }
      break;
#endif
    case PyFunction_EVENT_DESTROY:
      if (jit::perf::isPreforkCompilationEnabled()) {
        auto& perf_trampoline_worklist =
            cinderx::getModuleState()->perfTrampolineWorklist();
        perf_trampoline_worklist.erase(func);
      }
      jit::funcDestroyed(func);
      break;
  }

  return 0;
}

int cinderx_type_watcher(PyTypeObject* type) {
#if PY_VERSION_HEX < 0x030C0000
  _PyShadow_TypeModified(type);
#endif
  jit::typeModified(type);

  return 0;
}

#if PY_VERSION_HEX >= 0x030C0000
bool enable_patching = 0;
#endif

static PyObject* cinderx_freeze_type(PyObject*, PyObject* o) {
  if (!PyType_Check(o)) {
    PyErr_Format(
        PyExc_TypeError,
        "freeze_type requires a type, got %s",
        Py_TYPE(o)->tp_name);
    return nullptr;
  }

#if PY_VERSION_HEX < 0x030C0000
  PyInterpreterState* interp = _PyInterpreterState_GET();
  assert(interp != nullptr);
  if (!interp->config.enable_patching) {
    ((PyTypeObject*)o)->tp_flags |= Ci_Py_TPFLAGS_FROZEN;
  }
#else
  if (!enable_patching) {
    ((PyTypeObject*)o)->tp_flags |= Py_TPFLAGS_IMMUTABLETYPE;
  }
#endif
  Py_INCREF(o);
  return o;
}

PyDoc_STRVAR(
    freeze_type_doc,
    "freeze_type(t)\n\
\n\
Marks a type as being frozen and disallows any future mutations to it.");

// Install hooks into the 3.10.cinder runtime.  Does nothing for newer runtimes.
void initCinderHooks() {
#if PY_VERSION_HEX < 0x030C0000
  // The casts here are safe because BorrowedRef<T> has the same representation
  // as T*.  It's a little ugly, but it goes away post-3.10.
  Ci_hook_type_destroyed =
      reinterpret_cast<Ci_TypeCallback>(jit::typeDestroyed);
  Ci_hook_type_name_modified =
      reinterpret_cast<Ci_TypeCallback>(jit::typeNameModified);

  Ci_hook_JIT_GetFrame = _PyJIT_GetFrame;
  Ci_hook_PyCMethod_New = Ci_PyCMethod_New_METH_TYPED;
  Ci_hook_PyDescr_NewMethod = Ci_PyDescr_NewMethod_METH_TYPED;
  Ci_hook_WalkStack = Ci_WalkStack;
  Ci_hook_code_sizeof_shadowcode = shadowcode_code_sizeof;
  Ci_hook_PyJIT_GenVisitRefs = _PyJIT_GenVisitRefs;
  Ci_hook_PyJIT_GenDealloc = _PyJIT_GenDealloc;
  Ci_hook_PyJIT_GenSend = _PyJIT_GenSend;
  Ci_hook_PyJIT_GenYieldFromValue = _PyJIT_GenYieldFromValue;
  Ci_hook_PyJIT_GenMaterializeFrame = _PyJIT_GenMaterializeFrame;
  Ci_hook__PyShadow_FreeAll = _PyShadow_FreeAll;
  Ci_hook_MaybeStrictModule_Dict = Ci_MaybeStrictModule_Dict;
  Ci_hook_PyJIT_GetFrame = _PyJIT_GetFrame;
  Ci_hook_PyJIT_GetBuiltins = _PyJIT_GetBuiltins;
  Ci_hook_PyJIT_GetGlobals = _PyJIT_GetGlobals;
  Ci_hook_PyJIT_GetCurrentCodeFlags = get_current_code_flags;
  Ci_hook_ShadowFrame_GetCode_JIT = Ci_ShadowFrame_GetCode_JIT;
  Ci_hook_ShadowFrame_HasGen_JIT = Ci_ShadowFrame_HasGen_JIT;
  Ci_hook_ShadowFrame_GetModuleName_JIT = Ci_ShadowFrame_GetModuleName_JIT;
  Ci_hook_ShadowFrame_WalkAndPopulate = Ci_ShadowFrame_WalkAndPopulate;
#endif
}

int initFrameEvalFunc() {
#ifdef ENABLE_INTERPRETER_LOOP
#ifdef ENABLE_EVAL_HOOK
  Ci_hook_EvalFrame = Ci_EvalFrame;
#elif defined(ENABLE_PEP523_HOOK)
  // Let borrowed.h know the eval frame pointer
  Ci_EvalFrameFunc = Ci_EvalFrame;

  auto interp = _PyInterpreterState_GET();
  auto current_eval_frame = _PyInterpreterState_GetEvalFrameFunc(interp);
  if (current_eval_frame != _PyEval_EvalFrameDefault) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "CinderX tried to set a frame evaluator function but something else "
        "has done it first, this is not supported");
    return -1;
  }

  _PyInterpreterState_SetEvalFrameFunc(interp, Ci_EvalFrame);
#endif
#endif

  return 0;
}

void finiFrameEvalFunc() {
#ifdef ENABLE_INTERPRETER_LOOP
#ifdef ENABLE_EVAL_HOOK
  Ci_hook_EvalFrame = nullptr;
#elif defined(ENABLE_PEP523_HOOK)
  _PyInterpreterState_SetEvalFrameFunc(_PyInterpreterState_GET(), nullptr);
#endif
#endif
}

// Check if Python code is still being executed.
bool isCodeRunning() {
  PyThreadState* tstate = PyThreadState_Get();
#if PY_VERSION_HEX < 0x030C0000
  return tstate->shadow_frame != nullptr;
#elif PY_VERSION_HEX < 0x030D0000
  return tstate->cframe != &tstate->root_cframe;
#elif PY_VERSION_HEX < 0x030F0000
  return tstate->current_frame != nullptr;
#else
  return tstate->current_frame != tstate->base_frame;
#endif
}

int module_traverse(PyObject* mod, visitproc visit, void* arg) {
  cinderx::ModuleState* state = cinderx::getModuleState(mod);
  return state->traverse(visit, arg);
}

int module_clear(PyObject* mod) {
  cinderx::ModuleState* state = cinderx::getModuleState(mod);
  return state->clear();
}

void module_free(void* raw_mod) {
  auto mod = reinterpret_cast<PyObject*>(raw_mod);
  auto state = cinderx::getModuleState(mod);

  _PyClassLoader_ClearCache();
  _PyClassLoader_ClearValueCache();

  // If any Python code is running we can't tell if JIT code is in use. Even if
  // every frame in the callstack is interpreter-owned, some of them could be
  // the result of deopt and JIT code may still be on the native stack.
  JIT_CHECK(!isCodeRunning(), "Python code still running on CinderX unload");

  finiFrameEvalFunc();

  jit::finalize();

  finiCodeExtraIndex();

#if PY_VERSION_HEX < 0x030C0000
  JIT_CHECK(
      !Ci_cinderx_initialized || !Ci_hook__PyShadow_FreeAll(),
      "Failed to free shadowcode data");

  Ci_hook_type_destroyed = nullptr;
  Ci_hook_type_name_modified = nullptr;
  Ci_hook_JIT_GetFrame = nullptr;
  Ci_hook_PyDescr_NewMethod = nullptr;
  Ci_hook_WalkStack = nullptr;
  Ci_hook_code_sizeof_shadowcode = nullptr;
  Ci_hook_PyJIT_GenVisitRefs = nullptr;
  Ci_hook_PyJIT_GenDealloc = nullptr;
  Ci_hook_PyJIT_GenSend = nullptr;
  Ci_hook_PyJIT_GenYieldFromValue = nullptr;
  Ci_hook_PyJIT_GenMaterializeFrame = nullptr;
  Ci_hook__PyShadow_FreeAll = nullptr;
  Ci_hook_MaybeStrictModule_Dict = nullptr;
  Ci_hook_ShadowFrame_GetCode_JIT = nullptr;
  Ci_hook_ShadowFrame_HasGen_JIT = nullptr;
  Ci_hook_ShadowFrame_GetModuleName_JIT = nullptr;
  Ci_hook_ShadowFrame_WalkAndPopulate = nullptr;

  /* These hooks are not safe to unset, since there may be SP generic types that
   * outlive finalization of the cinder module, and if we don't have the hooks
   * in place for their cleanup, we will have leaks. But these hooks also have
   * no effect for any type other than an SP generic type, so they are generally
   * harmless to leave in place, even if the runtime is shutdown and
   * reinitialized. */

  Ci_hook_PyJIT_GetFrame = nullptr;
  Ci_hook_PyJIT_GetBuiltins = nullptr;
  Ci_hook_PyJIT_GetGlobals = nullptr;
  Ci_hook_PyJIT_GetCurrentCodeFlags = nullptr;

  Ci_cinderx_initialized = 0;
#endif

#if PY_VERSION_HEX >= 0x030C0000
  // This must be done at the point the module is free'd as the free-list uses
  // data backed by the module state. The free-list will use the module refcount
  // to keep the module alive while such uses are outstanding.
  jit::shutdown_jit_genobject_type();
#endif

  // Running the module state's destructor will access the global singleton, so
  // reset the singleton afterwards.
  state->cinderx::ModuleState::~ModuleState();
  cinderx::removeModuleState();
}

// Called when the interpreter is shutting down, allows us to do some aggressive
// cleanup. Currently this includes clearing out all strict modules which the
// interpreter won't do because it only supports clearing normal module objects.
static PyObject* clear_strict_modules(PyObject*, PyObject*) {
  BorrowedRef<> modules = PyImport_GetModuleDict();
  Ref<> clearing;
  if (PyDict_CheckExact(modules)) {
    Py_ssize_t pos = 0;
    PyObject *key, *value;
    while (PyDict_Next(modules, &pos, &key, &value)) {
      if (Ci_StrictModule_Check(value)) {
        if (clearing == nullptr) {
          clearing = Ref<>::steal(PyList_New(0));
        }
        if (PyList_Append(clearing, value) < 0) {
          break;
        }
      }
    }
  }

  if (clearing != nullptr) {
    for (Py_ssize_t i = 0; i < PyList_GET_SIZE(clearing.get()); i++) {
      BorrowedRef<> mod = PyList_GET_ITEM(clearing.get(), i);
      BorrowedRef<> dict = Ci_StrictModule_GetDict(mod);
      PyDict_Clear(dict);
    }
  }
  Py_RETURN_NONE;
}

PyMethodDef _cinderx_methods[] = {
    {"clear_caches",
     clear_caches,
     METH_NOARGS,
     PyDoc_STR(
         "Clears caches associated with the JIT.  This may have a "
         "negative effect on performance of existing JIT compiled code.")},
#if PY_VERSION_HEX < 0x030C0000
    {"clear_all_shadow_caches",
     clear_all_shadow_caches,
     METH_NOARGS,
     PyDoc_STR("")},
#endif
    {"freeze_type", cinderx_freeze_type, METH_O, freeze_type_doc},
    {"strict_module_patch",
     strict_module_patch,
     METH_VARARGS,
     strict_module_patch_doc},
    {"strict_module_patch_delete",
     strict_module_patch_delete,
     METH_VARARGS,
     strict_module_patch_delete_doc},
    {"strict_module_patch_enabled",
     strict_module_patch_enabled,
     METH_O,
     strict_module_patch_enabled_doc},
    {"clear_classloader_caches",
     clear_classloader_caches,
     METH_NOARGS,
     PyDoc_STR(
         "Clears classloader caches and vtables on all accessible types. "
         "Will hurt perf; for test isolation where modules and types with "
         "identical names are dynamically created and destroyed.")},
    {"watch_sys_modules",
     watch_sys_modules,
     METH_NOARGS,
     PyDoc_STR(
         "Watch the sys.modules dict to allow invalidating Static Python's "
         "internal caches.")},
    {"has_parallel_gc", has_parallel_gc, METH_NOARGS, has_parallel_gc_doc},
    {"enable_parallel_gc",
     (PyCFunction)cinder_enable_parallel_gc,
     METH_VARARGS | METH_KEYWORDS,
     cinder_enable_parallel_gc_doc},
    {"disable_parallel_gc",
     cinder_disable_parallel_gc,
     METH_NOARGS,
     cinder_disable_parallel_gc_doc},
    {"get_parallel_gc_settings",
     cinder_get_parallel_gc_settings,
     METH_NOARGS,
     cinder_get_parallel_gc_settings_doc},
    {"_clear_strict_modules",
     clear_strict_modules,
     METH_NOARGS,
     "Clears all strict modules for shutdown"},
    {"_compile_perf_trampoline_pre_fork",
     compile_perf_trampoline_pre_fork,
     METH_NOARGS,
     PyDoc_STR("Compile perf-trampoline entries before forking.")},
    {"_is_compile_perf_trampoline_pre_fork_enabled",
     is_compile_perf_trampoline_pre_fork_enabled,
     METH_NOARGS,
     PyDoc_STR(
         "Return whether compile perf-trampoline entries before fork is "
         "enabled or not.")},
#if PY_VERSION_HEX < 0x030C0000
    {"_get_entire_call_stack_as_qualnames_with_lineno",
     get_entire_call_stack_as_qualnames_with_lineno,
     METH_NOARGS,
     PyDoc_STR(
         "Return the current stack as a list of tuples (qualname, lineno).")},
    {"_get_entire_call_stack_as_qualnames_with_lineno_and_frame",
     get_entire_call_stack_as_qualnames_with_lineno_and_frame,
     METH_NOARGS,
     PyDoc_STR(
         "Return the current stack as a list of tuples (qualname, "
         "lineno, PyFrame | None).")},
#endif
    {"immortalize_heap",
     cinder_immortalize_heap,
     METH_NOARGS,
     cinder_immortalize_heap_doc},
    {"is_immortal", cinder_is_immortal, METH_O, cinder_is_immortal_doc},
#if PY_VERSION_HEX >= 0x030C0000
    {"anext",
     reinterpret_cast<PyCFunction>(builtin_anext),
     METH_FASTCALL,
     "anext($module, aiterator, default=<unrepresentable>, /)\n"
     "--\n"
     "\n"
     "Return the next item from the async iterator.\n"
     "\n"
     "If default is given and the async iterator is exhausted,\n"
     "it is returned instead of raising StopAsyncIteration."},
    {"delay_adaptive",
     cinder_delay_adaptive,
     METH_O,
     cinder_delay_adaptive_doc},
    {"set_adaptive_delay",
     cinder_set_adaptive_delay,
     METH_O,
     cinder_set_adaptive_delay_doc},
    {"get_adaptive_delay",
     cinder_get_adaptive_delay,
     METH_NOARGS,
     cinder_get_adaptive_delay_doc},
#endif
#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_PARALLEL_GC)
    {"get_threshold",
     cinder_get_threshold,
     METH_NOARGS,
     cinder_get_threshold_doc},
#endif
    {nullptr, nullptr, 0, nullptr}};

int _cinderx_exec_impl(PyObject* m) {
  cinderx::initStaticObjects();

  // The state will be destroyed in module_free(), which gets called even if
  // this function exits early with an error.
  void* state_mem = PyModule_GetState(m);
  auto state = new (state_mem) cinderx::ModuleState();
  cinderx::setModuleState(m);

  auto cache_manager = new (std::nothrow) jit::GlobalCacheManager();
  if (cache_manager == nullptr) {
    return -1;
  }

  state->setCacheManager(cache_manager);

  // Code allocator is initialized in jit::initialize(), because it needs to
  // read -X options from the CLI and environment variables to figure out which
  // implementation to use.

  Ref<> builtins_mod = Ref<>::steal(PyImport_ImportModule("builtins"));
  if (builtins_mod == nullptr) {
    return -1;
  }
  Ref<> next = Ref<>::steal(PyObject_GetAttrString(builtins_mod, "next"));
  if (next == nullptr) {
    return -1;
  }
  state->setBuiltinNext(next);

#if PY_VERSION_HEX >= 0x030C0000

  auto async_lazy_value = new (std::nothrow) cinderx::AsyncLazyValueState();
  if (async_lazy_value == nullptr) {
    return -1;
  } else if (!async_lazy_value->init()) {
    delete async_lazy_value;
    return -1;
  }

  state->setAsyncLazyValueState(async_lazy_value);

  PyTypeObject* gen_type = (PyTypeObject*)PyType_FromSpec(&jit::JitGen_Spec);
  if (gen_type == nullptr) {
    return -1;
  }
  state->setGenType(gen_type);
  Py_DECREF(gen_type);

  PyTypeObject* coro_type = (PyTypeObject*)PyType_FromSpec(&jit::JitCoro_Spec);
  if (coro_type == nullptr) {
    return -1;
  }
  state->setCoroType(coro_type);
  Py_DECREF(coro_type);

#if defined(ENABLE_LIGHTWEIGHT_FRAMES) && PY_VERSION_HEX < 0x030E0000
  Ref<PyTypeObject> frame_reifier_type = Ref<PyTypeObject>::steal(
      (PyTypeObject*)PyType_FromSpec(&jit::JitFrameReifier_Spec));
  if (frame_reifier_type == nullptr) {
    return -1;
  }
  PyObject* reifier = _PyObject_New(frame_reifier_type);
  if (reifier == nullptr) {
    return -1;
  }

  ((jit::JitFrameReifier*)reifier)->vectorcall =
      (vectorcallfunc)jit::jitFrameReifierVectorcall;
  state->setFrameReifier(reifier);

  // Mark as immortal so we don't have to refcount this.
  immortalize(reifier);
#endif

  // PyType_FromSpec wants us to provide a module name, but we really don't
  // want one, we go a long way to make these look just like CPython's types.
  gen_type->tp_name = "generator";
  coro_type->tp_name = "coroutine";
  Ref<> builtins = Ref<>::steal(PyUnicode_FromString("builtins"));
  if (builtins == nullptr ||
      PyDict_SetItemString(gen_type->tp_dict, "__module__", builtins) < 0 ||
      PyDict_SetItemString(coro_type->tp_dict, "__module__", builtins) < 0) {
    return -1;
  }

  PyTypeObject* anext_awaitable_type =
      (PyTypeObject*)PyType_FromSpec(&jit::JitAnextAwaitable_Spec);
  if (anext_awaitable_type == nullptr) {
    return -1;
  }
  state->setAnextAwaitableType(anext_awaitable_type);

  auto anext_func = Ref<>::steal(PyObject_GetAttrString(m, "anext"));
  if (anext_func == nullptr ||
      PyObject_SetAttrString(builtins_mod, "anext", anext_func) < 0) {
    return -1;
  }

#else
  state->setCoroType(&PyCoro_Type);
  state->setGenType(&PyGen_Type);

#endif

#if PY_VERSION_HEX >= 0x030E0000 && defined(ENABLE_PARALLEL_GC)
  Ref<> gc_mod = Ref<>::steal(PyImport_ImportModule("gc"));
  if (gc_mod == nullptr) {
    return -1;
  }

  auto get_threshold_func =
      Ref<>::steal(PyObject_GetAttrString(m, "get_threshold"));
  if (get_threshold_func == nullptr ||
      PyObject_SetAttrString(gc_mod, "get_threshold", get_threshold_func) < 0) {
    return -1;
  }
#endif

  auto runtime = new (std::nothrow) jit::Runtime();
  if (runtime == nullptr) {
    return -1;
  }
  state->setRuntime(runtime);

  auto symbolizer = new (std::nothrow) jit::Symbolizer();
  if (symbolizer == nullptr) {
    return -1;
  }
  state->setSymbolizer(symbolizer);

  if constexpr (PY_VERSION_HEX >= 0x030C0000) {
    if (!state->initBuiltinMembers()) {
      return -1;
    }
  }

  auto& watcher_state = state->watcherState();
  watcher_state.setCodeWatcher(cinderx_code_watcher);
  watcher_state.setDictWatcher(cinderx_dict_watcher);
  watcher_state.setFuncWatcher(cinderx_func_watcher);
  watcher_state.setTypeWatcher(cinderx_type_watcher);

  CiExc_StaticTypeError =
      PyErr_NewException("cinderx.StaticTypeError", PyExc_TypeError, nullptr);
  if (CiExc_StaticTypeError == nullptr) {
    return -1;
  }

  if (PyType_Ready(&PyCachedProperty_Type) < 0) {
    return -1;
  }
  if (PyType_Ready(&PyCachedPropertyWithDescr_Type) < 0) {
    return -1;
  }
  if (PyType_Ready(&Ci_StrictModule_Type) < 0) {
    return -1;
  }
  if (PyType_Ready(&PyAsyncCachedProperty_Type) < 0) {
    return -1;
  }
  if (PyType_Ready(&PyAsyncCachedPropertyWithDescr_Type) < 0) {
    return -1;
  }
  if (PyType_Ready(&PyAsyncCachedClassProperty_Type) < 0) {
    return -1;
  }
  if (PyType_Ready(&_Ci_ObjectKeyType) < 0) {
    return -1;
  }

  PyObject* cached_classproperty =
      PyType_FromSpec(&_PyCachedClassProperty_TypeSpec);
  if (cached_classproperty == nullptr) {
    return -1;
  }
  if (PyObject_SetAttrString(m, "cached_classproperty", cached_classproperty) <
      0) {
    Py_DECREF(cached_classproperty);
    return -1;
  }
  Py_DECREF(cached_classproperty);

#define ADDITEM(NAME, OBJECT)                                   \
  if (PyObject_SetAttrString(m, NAME, (PyObject*)OBJECT) < 0) { \
    return -1;                                                  \
  }

  ADDITEM("StaticTypeError", CiExc_StaticTypeError);
  ADDITEM("StrictModule", &Ci_StrictModule_Type);
  ADDITEM("cached_property", &PyCachedProperty_Type);
  ADDITEM("cached_property_with_descr", &PyCachedPropertyWithDescr_Type);
  ADDITEM("async_cached_property", &PyAsyncCachedProperty_Type);
  ADDITEM("async_cached_classproperty", &PyAsyncCachedClassProperty_Type);
#if PY_VERSION_HEX >= 0x030C0000
  ADDITEM("AsyncLazyValue", async_lazy_value->asyncLazyValueType());
  ADDITEM("AwaitableValue", async_lazy_value->awaitableValueType());
#endif

#undef ADDITEM

  // We don't want a low level callback which is called pretty late after
  // sys.modules has been cleared. Instead we want to be called before that
  // happens so we can clear out strict modules first, so we register
  // directly with the atexit library.
  auto atexit = Ref<>::steal(PyImport_ImportModule("atexit"));
  auto register_func = Ref<>::steal(PyObject_GetAttrString(atexit, "register"));
  auto clear_strict_modules_func =
      Ref<>::steal(PyObject_GetAttrString(m, "_clear_strict_modules"));
  if (clear_strict_modules_func == nullptr) {
    return -1;
  }
  auto res = Ref<>::steal(
      PyObject_CallOneArg(register_func, clear_strict_modules_func));
  if (res == nullptr) {
    return -1;
  }

  if constexpr (PY_VERSION_HEX >= 0x030C0000) {
    // Get the existing cache clear function so we can forward to it.
    const char* clear_name;
    if constexpr (PY_VERSION_HEX >= 0x030E0000) {
      clear_name = "_clear_internal_caches";
    } else {
      clear_name = "_clear_type_cache";
    }
    BorrowedRef<> clear_type_cache = PySys_GetObject(clear_name);
    state->setSysClearCaches(clear_type_cache);

    // Replace sys._clear_type_cache with our clearing function
    Ref<> clear_caches =
        Ref<>::steal(PyObject_GetAttrString(m, "clear_caches"));
    if (clear_caches == nullptr ||
        PySys_SetObject(clear_name, clear_caches) < 0) {
      return -1;
    }
  }

  if (init_upstream_borrow() < 0) {
    return -1;
  }

  // Initialize the code object extra data index early, before we hook into the
  // interpreter and try to use it.
  initCodeExtraIndex();

  initCinderHooks();

  if (initFrameEvalFunc() < 0) {
    return -1;
  }

  init_already_existing_types();

  if (watcher_state.init() < 0) {
    return -1;
  }

  int jit_init_ret = jit::initialize();
  if (jit_init_ret) {
    // Exit here rather than in jit::initialize() so the tests for printing
    // argument help works.
    if (jit_init_ret == -2) {
      exit(1);
    }
    return -1;
  }

  init_existing_objects();

#if PY_VERSION_HEX < 0x030C0000
  Ci_cinderx_initialized = 1;
#endif

#if PY_VERSION_HEX >= 0x030C0000
  char* patching = getenv("PYTHONENABLEPATCHING");
  enable_patching = patching != nullptr && strcmp(patching, "1") == 0;

#ifdef ENABLE_INTERPRETER_LOOP
  Ci_InitOpcodes();
#endif

#endif

#ifdef ENABLE_XXCLASSLOADER
  if (_Ci_CreateXXClassLoaderModule() < 0) {
    return -1;
  }
#endif

  if (_Ci_CreateStaticModule() < 0) {
    return -1;
  }

  return 0;
}

int _cinderx_exec(PyObject* m) {
  int result = _cinderx_exec_impl(m);
  // Initialization can fail and leave things partially initialized. The main
  // item we want to restore immediately is the interpreter loop function,
  // otherwise Ci_EvalFrame will still try to access CinderX data.
  //
  // Everything else will be handled by module_free() when there's an error.
  if (result < 0) {
    finiFrameEvalFunc();
  }
  return result;
}

PyModuleDef_Slot _cinderx_slots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(_cinderx_exec)},
#if PY_VERSION_HEX >= 0x030C0000
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
#endif
    {0, nullptr}};

PyModuleDef _cinderx_module = {
    PyModuleDef_HEAD_INIT,
    "_cinderx",
    PyDoc_STR("The internal CinderX extension module."),
    sizeof(cinderx::ModuleState),
    _cinderx_methods,
    _cinderx_slots,
    module_traverse,
    module_clear,
    module_free,
};

} // namespace

PyObject* _cinderx_lib_init() {
  return PyModuleDef_Init(&_cinderx_module);
}
