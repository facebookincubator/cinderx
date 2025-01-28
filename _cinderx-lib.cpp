// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/_cinderx-lib.h"

#include <Python.h>

// clang-format off
// This has to be high up here to avoid <atomic> vs <stdatomic.h> include
// issues.
#include "cinderx/Common/log.h"
// clang-format on

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#include "internal/pycore_shadow_frame.h"
#endif

#include "cinder/hooks.h"
#include "internal/pycore_pystate.h"

#include "cinderx/CachedProperties/cached_properties.h"
#include "cinderx/Common/dict.h"
#include "cinderx/Common/func.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/watchers.h"
#include "cinderx/Immortalize/immortalize.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/pyjit_result.h"
#include "cinderx/Jit/runtime.h"
#include "cinderx/ParallelGC/parallel_gc.h"
#include "cinderx/Shadowcode/shadowcode.h"
#include "cinderx/StaticPython/_static.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/descrobject_vectorcall.h"
#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/methodobject_vectorcall.h"
#include "cinderx/StaticPython/objectkey.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/vtable_builder.h"
#include "cinderx/Upgrade/upgrade_stubs.h" // @donotremove
#include "cinderx/UpstreamBorrow/borrowed.h"

#include <dlfcn.h>

namespace {

// Function objects registered for pre-fork perf-trampoline compilation.
std::unordered_set<BorrowedRef<PyFunctionObject>> perf_trampoline_worklist;

/*
 * Misc. Python-facing utility functions.
 */

PyObject* clear_caches(PyObject*, PyObject*) {
  jit::_PyJIT_GetGlobalCacheManager()->clear();
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
  PyObject* sys = PyImport_ImportModule("sys");
  if (sys == nullptr) {
    Py_RETURN_NONE;
  }

  PyObject* modules = PyObject_GetAttrString(sys, "modules");
  Py_DECREF(sys);
  if (modules == nullptr) {
    Py_RETURN_NONE;
  }
  Ci_Watchers_WatchDict(modules);
  Py_DECREF(modules);
  Py_RETURN_NONE;
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

  if (Cinder_EnableParallelGC(min_gen, num_threads) < 0) {
    return nullptr;
  }
  Py_RETURN_NONE;
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
  Cinder_DisableParallelGC();
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    cinder_immortalize_heap_doc,
    "immortalize_heap($module, /)\n"
    "--\n"
    "\n"
    "Immortalize all instances accessible through the GC roots.");
PyObject* cinder_immortalize_heap(PyObject* mod, PyObject* unused) {
  return immortalize_heap(mod, unused);
}

PyDoc_STRVAR(
    cinder_is_immortal_doc,
    "is_immortal($module, obj, /)\n"
    "--\n"
    "\n"
    "Return True if the object is immortal, else return False.");
PyObject* cinder_is_immortal(PyObject* /* mod */, PyObject* obj) {
  return is_immortal(obj);
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
  return Cinder_GetParallelGCSettings();
}

PyObject* compile_perf_trampoline_pre_fork(PyObject*, PyObject*) {
  if (!jit::perf::isPreforkCompilationEnabled()) {
    Py_RETURN_NONE;
  }

  PyUnstable_PerfTrampoline_SetPersistAfterFork(1);

  for (BorrowedRef<PyFunctionObject> func : perf_trampoline_worklist) {
    BorrowedRef<PyCodeObject> code = func->func_code;
    if (PyUnstable_PerfTrampoline_CompileCode(code) == -1) {
      JIT_LOG(
          "Failed to compile perf trampoline for function {}",
          jit::funcFullname(func));
    }
  }
  perf_trampoline_worklist.clear();

  Py_RETURN_NONE;
}

PyObject* is_compile_perf_trampoline_pre_fork_enabled(PyObject*, PyObject*) {
  if (jit::perf::isPreforkCompilationEnabled()) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

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
#if PY_VERSION_HEX < 0x030C0000
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
#else
  // As we don't have shadow frames and qualnames are upstream we can probably
  // do this all using the normal C-API now.
  UPGRADE_ASSERT(FRAME_HANDLING_CHANGED);
  return nullptr;
#endif
}

PyObject* get_entire_call_stack_as_qualnames_with_lineno(PyObject*, PyObject*) {
  return collect_stack(0);
}

PyObject* get_entire_call_stack_as_qualnames_with_lineno_and_frame(
    PyObject*,
    PyObject*) {
  return collect_stack(1);
}

// Schedule a function to be JIT-compiled.  If that fails, then also try
// compiling a perf trampoline for the Python function.
void scheduleCompile(BorrowedRef<PyFunctionObject> func) {
  bool scheduled = jit::scheduleJitCompile(func);
  if (!scheduled && jit::perf::isPreforkCompilationEnabled()) {
    perf_trampoline_worklist.emplace(func);
  }
}

/*
 * (De)initialization functions
 */

// Visit a Python function on CinderX module initialization.
int function_visitor(BorrowedRef<PyFunctionObject> func) {
  // Ensure the code object can track how often it is called.
  BorrowedRef<PyCodeObject> code = func->func_code;
  JIT_CHECK(
      !USE_CODE_EXTRA || initCodeExtra(code) != nullptr,
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
  initCodeExtraIndex();

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
  return tstate->cframe->current_frame->f_code->co_flags;
#endif
}

int cinderx_code_watcher(PyCodeEvent event, PyCodeObject* co) {
  switch (event) {
    case PY_CODE_EVENT_CREATE:
      initCodeExtra(co);
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

  jit::GlobalCacheManager* globalCaches = jit::_PyJIT_GetGlobalCacheManager();

  switch (event) {
    case PyDict_EVENT_ADDED:
    case PyDict_EVENT_MODIFIED:
    case PyDict_EVENT_DELETED: {
      _PyClassLoader_NotifyDictChange(dict, event, key_obj, new_value);

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
      globalCaches->notifyDictClear(dict);
      break;
    case PyDict_EVENT_CLONED:
      globalCaches->notifyDictUnwatch(dict);
      break;
    case PyDict_EVENT_DEALLOCATED:
      _PyClassLoader_NotifyDictChange(dict, event, key_obj, new_value);
      globalCaches->notifyDictUnwatch(dict);
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
    case PyFunction_EVENT_DESTROY:
      if (jit::perf::isPreforkCompilationEnabled()) {
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

int cinder_init() {
  if (init_upstream_borrow() < 0) {
    return -1;
  }
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
  Ci_hook_EvalFrame = Ci_EvalFrame;
  Ci_hook_PyJIT_GetFrame = _PyJIT_GetFrame;
  Ci_hook_PyJIT_GetBuiltins = _PyJIT_GetBuiltins;
  Ci_hook_PyJIT_GetGlobals = _PyJIT_GetGlobals;
  Ci_hook_PyJIT_GetCurrentCodeFlags = get_current_code_flags;
  Ci_hook_ShadowFrame_GetCode_JIT = Ci_ShadowFrame_GetCode_JIT;
  Ci_hook_ShadowFrame_HasGen_JIT = Ci_ShadowFrame_HasGen_JIT;
  Ci_hook_ShadowFrame_GetModuleName_JIT = Ci_ShadowFrame_GetModuleName_JIT;
  Ci_hook_ShadowFrame_WalkAndPopulate = Ci_ShadowFrame_WalkAndPopulate;
#else
  Ci_hook_EvalFrame = Ci_EvalFrame;
#endif

#if PY_VERSION_HEX < 0x030C0000
  JIT_CHECK(
      __strobe_CodeRuntime_py_code == jit::CodeRuntime::kPyCodeOffset,
      "Invalid PyCodeOffset for Strobelight");
  JIT_CHECK(
      __strobe_RuntimeFrameState_py_code ==
          jit::RuntimeFrameState::codeOffset(),
      "Invalid codeOffset for Strobelight");
#endif

  init_already_existing_types();

  WatcherState watcher_state;
  watcher_state.code_watcher = cinderx_code_watcher;
  watcher_state.dict_watcher = cinderx_dict_watcher;
  watcher_state.func_watcher = cinderx_func_watcher;
  watcher_state.type_watcher = cinderx_type_watcher;
  if (Ci_Watchers_Init(&watcher_state)) {
    return -1;
  }

  int jit_init_ret = _PyJIT_Initialize();
  if (jit_init_ret) {
    // Exit here rather than in _PyJIT_Initialize so the tests for printing
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
#endif

  // Create _static module
  return _Ci_CreateStaticModule();
}

// Attempts to shutdown CinderX. This is very much a best-effort with the
// primary goals being to ensure Python shuts down without crashing, and
// tests which do some kind of re-initialization continue to work. A secondary
// goal is to one day make it possible to arbitrarily load/relaod CinderX at
// runtime. However, for now the only thing we truly support is loading
// CinderX once ASAP on start-up, and then never unloading it until complete
// process shutdown.
int cinder_fini() {
  _PyClassLoader_ClearCache();

  PyThreadState* tstate = PyThreadState_Get();
  bool code_running =
#if PY_VERSION_HEX < 0x030C0000
      tstate->shadow_frame != nullptr;
#else
      tstate->cframe != &tstate->root_cframe;
#endif
  if (code_running) {
    // If any Python code is running we can't tell if JIT code is in use. Even
    // if every frame in the callstack is interpreter-owned, some of them could
    // be the result of deopt and JIT code may still be on the native stack.
    JIT_DABORT("Python code still running on CinderX unload");
    JIT_LOG("Python code is executing, cannot cleanly shutdown CinderX.");
    return -1;
  }

  if (_PyJIT_Finalize()) {
    return -1;
  }

  finiCodeExtraIndex();

#if PY_VERSION_HEX < 0x030C0000
  if (Ci_cinderx_initialized && Ci_hook__PyShadow_FreeAll()) {
    return -1;
  }

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

  Ci_hook_EvalFrame = nullptr;
  Ci_hook_PyJIT_GetFrame = nullptr;
  Ci_hook_PyJIT_GetBuiltins = nullptr;
  Ci_hook_PyJIT_GetGlobals = nullptr;
  Ci_hook_PyJIT_GetCurrentCodeFlags = nullptr;

  Ci_cinderx_initialized = 0;
#else
  Ci_hook_EvalFrame = nullptr;
#endif

  return 0;
}

bool g_was_initialized = false;

PyObject* init(PyObject* /*self*/, PyObject* /*obj*/) {
  if (g_was_initialized) {
    Py_RETURN_FALSE;
  }
  if (cinder_init()) {
    if (!PyErr_Occurred()) {
      PyErr_SetString(PyExc_RuntimeError, "Failed to initialize CinderX");
    }
    return nullptr;
  }
  g_was_initialized = true;
  Py_RETURN_TRUE;
}

void module_free(void*) {
  if (g_was_initialized) {
    g_was_initialized = false;
    JIT_CHECK(cinder_fini() == 0, "Failed to finalize CinderX");
  }
}

PyMethodDef _cinderx_methods[] = {
    {"init",
     init,
     METH_NOARGS,
     PyDoc_STR(
         "This must be called early. Preferably before any user code is run.")},
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
    {"_compile_perf_trampoline_pre_fork",
     compile_perf_trampoline_pre_fork,
     METH_NOARGS,
     PyDoc_STR("Compile perf-trampoline entries before forking.")},
    {"_is_compile_perf_trampoline_pre_fork_enabled",
     is_compile_perf_trampoline_pre_fork_enabled,
     METH_NOARGS,
     PyDoc_STR("Return whether compile perf-trampoline entries before fork is "
               "enabled or not.")},
    {"_get_entire_call_stack_as_qualnames_with_lineno",
     get_entire_call_stack_as_qualnames_with_lineno,
     METH_NOARGS,
     PyDoc_STR(
         "Return the current stack as a list of tuples (qualname, lineno).")},
    {"_get_entire_call_stack_as_qualnames_with_lineno_and_frame",
     get_entire_call_stack_as_qualnames_with_lineno_and_frame,
     METH_NOARGS,
     PyDoc_STR("Return the current stack as a list of tuples (qualname, "
               "lineno, PyFrame | None).")},
    {"immortalize_heap",
     cinder_immortalize_heap,
     METH_NOARGS,
     cinder_immortalize_heap_doc},
    {"is_immortal", cinder_is_immortal, METH_O, cinder_is_immortal_doc},
    {nullptr, nullptr, 0, nullptr}};

struct PyModuleDef _cinderx_module = {
    PyModuleDef_HEAD_INIT,
    "_cinderx",
    PyDoc_STR("The internal CinderX extension module."),
    /*m_size=*/-1, // Doesn't support sub-interpreters
    _cinderx_methods,
    /*m_slots=*/nullptr,
    /*m_traverse=*/nullptr,
    /*m_clear=*/nullptr,
    /*m_free=*/module_free,
};

} // namespace

PyObject* _cinderx_lib_init() {
  int dlopenflags =
      CI_INTERP_IMPORT_FIELD(PyInterpreterState_Get(), dlopenflags);
  if ((dlopenflags & RTLD_GLOBAL) == 0) {
    PyErr_SetString(
        PyExc_ImportError,
        "Do not import _cinderx directly. Use cinderx instead.");
    return nullptr;
  }

  CiExc_StaticTypeError =
      PyErr_NewException("cinderx.StaticTypeError", PyExc_TypeError, nullptr);
  if (CiExc_StaticTypeError == nullptr) {
    return nullptr;
  }

  // Deliberate single-phase initialization.
  PyObject* m = PyModule_Create(&_cinderx_module);
  if (m == nullptr) {
    return nullptr;
  }

  if (PyType_Ready(&PyCachedProperty_Type) < 0) {
    return nullptr;
  }
  if (PyType_Ready(&PyCachedPropertyWithDescr_Type) < 0) {
    return nullptr;
  }
  if (PyType_Ready(&Ci_StrictModule_Type) < 0) {
    return nullptr;
  }
  if (PyType_Ready(&PyAsyncCachedProperty_Type) < 0) {
    return nullptr;
  }
  if (PyType_Ready(&PyAsyncCachedPropertyWithDescr_Type) < 0) {
    return nullptr;
  }
  if (PyType_Ready(&PyAsyncCachedClassProperty_Type) < 0) {
    return nullptr;
  }
  if (PyType_Ready(&_Ci_ObjectKeyType) < 0) {
    return nullptr;
  }

  PyObject* cached_classproperty =
      PyType_FromSpec(&_PyCachedClassProperty_TypeSpec);
  if (cached_classproperty == nullptr) {
    return nullptr;
  }
  if (PyObject_SetAttrString(m, "cached_classproperty", cached_classproperty) <
      0) {
    Py_DECREF(cached_classproperty);
    return nullptr;
  }
  Py_DECREF(cached_classproperty);

#define ADDITEM(NAME, OBJECT)                                   \
  if (PyObject_SetAttrString(m, NAME, (PyObject*)OBJECT) < 0) { \
    return nullptr;                                             \
  }

  ADDITEM("StaticTypeError", CiExc_StaticTypeError);
  ADDITEM("StrictModule", &Ci_StrictModule_Type);
  ADDITEM("cached_property", &PyCachedProperty_Type);
  ADDITEM("async_cached_property", &PyAsyncCachedProperty_Type);
  ADDITEM("async_cached_classproperty", &PyAsyncCachedClassProperty_Type);

#undef ADDITEM

  return m;
}
