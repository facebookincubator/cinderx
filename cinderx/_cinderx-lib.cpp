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
#include "cinderx/Jit/anextawaitable.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/config.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/global_cache.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/Jit/symbolizer.h"
#include "cinderx/StaticPython/_static.h"
#include "cinderx/StaticPython/checked_dict.h"
#include "cinderx/StaticPython/checked_list.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/StaticPython/objectkey.h"
#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/async_lazy_value.h"
#include "cinderx/module_state.h"
#include "cinderx/python_runtime.h"

#ifdef ENABLE_PARALLEL_GC
#include "cinderx/ParallelGC/parallel_gc.h"
#endif

#ifdef ENABLE_XXCLASSLOADER
#include "cinderx/StaticPython/xxclassloader.h"
#endif

#if PY_VERSION_HEX < 0x030D0000 && defined(ENABLE_EVAL_HOOK)
#include "cinder/hooks.h"
#endif

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_modsupport.h"
#endif

#ifndef WIN32
#include <dlfcn.h>
#endif

namespace {

/*
 * Misc. Python-facing utility functions.
 */

PyObject* clear_caches(PyObject* mod, PyObject*) {
  auto state = cinderx::getModuleState(mod);
  _PyCheckedDict_ClearCaches();
  _PyCheckedList_ClearCaches();
  _PyClassLoader_ClearValueCache();
  if (auto* ctx = jit::getContext()) {
    ctx->clearDeoptStats();
  }
  // We replace sys._clear_type_cache with our own function which clears the
  // caches, so we should call this too.
  BorrowedRef<> sys_clear = state->sys_clear_caches;
  if (sys_clear != nullptr) {
    Ref<> res =
        Ref<>::steal(PyObject_Vectorcall(sys_clear, nullptr, 0, nullptr));
    if (res == nullptr) {
      return nullptr;
    }
  }
  Py_RETURN_NONE;
}

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
    return nullptr;
  }

  auto modules = Ref<>::steal(PyObject_GetAttrString(sys, "modules"));
  if (modules == nullptr) {
    return nullptr;
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
#if !defined(WIN32) && (ENABLE_PERF_TRAMPOLINE || PY_VERSION_HEX >= 0x030D0000)
  if (!jit::perf::isPreforkCompilationEnabled()) {
    Py_RETURN_NONE;
  }

  PyUnstable_PerfTrampoline_SetPersistAfterFork(1);

  auto& perf_trampoline_worklist =
      cinderx::getModuleState(mod)->perf_trampoline_worklist;

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
#ifndef WIN32
  if (jit::perf::isPreforkCompilationEnabled()) {
    Py_RETURN_TRUE;
  }
#endif
  Py_RETURN_FALSE;
}

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
    return nullptr;
  }

  jit::getMutableConfig().delay_adaptive_code = (delay == Py_True);
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
    return nullptr;
  }

  jit::getMutableConfig().adaptive_threshold = PyLong_AsLong(delay);
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
  return PyLong_FromUnsignedLongLong(jit::getConfig().adaptive_threshold);
#else
  return PyLong_FromLong(-1);
#endif
}

// Capture the default vectorcall entrypoint for functions.
int ensurePyFunctionVectorcall() {
#if PY_VERSION_HEX < 0x030F0000
  // Picking a function that has a high chance of being implemented in Python,
  // and is extremely likely to be loaded already.
  const char* mod_name = "site";
  const char* func_name = "addsitedir";

  auto mod = Ref<>::steal(PyImport_ImportModule(mod_name));
  if (mod == nullptr) {
    return -1;
  }
  auto obj = Ref<>::steal(PyObject_GetAttrString(mod, func_name));
  if (obj == nullptr) {
    return -1;
  }
  if (!PyFunction_Check(obj)) {
    PyErr_Format(
        PyExc_RuntimeError,
        "Tried to load a Python function (%s.%s()) but got a '%.200s'",
        mod_name,
        func_name,
        Py_TYPE(obj)->tp_name);
    return -1;
  }
  BorrowedRef<PyFunctionObject> func{obj};
  Ci_PyFunction_Vectorcall = func->vectorcall;
#endif

  return 0;
}

// Schedule a function to be JIT-compiled.  If that fails, then also try
// compiling a perf trampoline for the Python function.
void scheduleCompile(BorrowedRef<PyFunctionObject> func) {
  bool scheduled = jit::scheduleJitCompile(func);
#ifndef WIN32
  if (!scheduled && jit::perf::isPreforkCompilationEnabled()) {
    auto& perf_trampoline_worklist =
        cinderx::getModuleState()->perf_trampoline_worklist;
    perf_trampoline_worklist.emplace(func);
  }
#endif
}

extern "C" PyObject* PyAnextAwaitable_New(PyObject*, PyObject*);

// Replacement for builtins.anext which is aware of JIT generators
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

/*
 * (De)initialization functions
 */

// Visit a Python function on CinderX module initialization.
int function_visitor(BorrowedRef<PyFunctionObject> func) {
  // Ensure the code object can track how often it is called.
  BorrowedRef<PyCodeObject> code = func->func_code;
  JIT_CHECK(
      codeExtra(code) != nullptr,
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

// Count the number of elements in a PyGetSetDef array.
size_t getsetLen(PyGetSetDef* getset) {
  size_t len = 0;
  for (PyGetSetDef* def = getset; def->name != nullptr; ++def) {
    ++len;
  }
  return len;
}

// NOLINTNEXTLINE(clang-diagnostic-unused-function)
int get_current_code_flags(PyThreadState* tstate) {
  return _PyFrame_GetCode(currentFrame(tstate))->co_flags;
}

int cinderx_code_watcher(PyCodeEvent event, PyCodeObject* co) {
  switch (event) {
    case PY_CODE_EVENT_CREATE:
      break;
    case PY_CODE_EVENT_DESTROY:
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
      state != nullptr ? state->cache_manager.get() : nullptr;

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
      // key is overwhelmingly likely to be interned, since in normal code it
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
#ifndef WIN32
      if (jit::perf::isPreforkCompilationEnabled()) {
        auto state = cinderx::getModuleState();
        if (state != nullptr) {
          auto& perf_trampoline_worklist = state->perf_trampoline_worklist;
          perf_trampoline_worklist.erase(func);
        }
      }
#endif
      jit::funcDestroyed(func);
      break;
  }

  return 0;
}

int cinderx_type_watcher(PyTypeObject* type) {
  jit::typeModified(type);

  return 0;
}

static PyObject* cinderx_freeze_type(PyObject*, PyObject* o) {
  if (!PyType_Check(o)) {
    PyErr_Format(
        PyExc_TypeError,
        "freeze_type requires a type, got %s",
        Py_TYPE(o)->tp_name);
    return nullptr;
  }

  if (!cinderx::getModuleState()->enable_patching) {
    ((PyTypeObject*)o)->tp_flags |= Py_TPFLAGS_IMMUTABLETYPE;
  }
  Py_INCREF(o);
  return o;
}

PyDoc_STRVAR(
    freeze_type_doc,
    "freeze_type(t)\n\
\n\
Marks a type as being frozen and disallows any future mutations to it.");

PyDoc_STRVAR(
    install_frame_evaluator_doc,
    "Install the CinderX frame evaluator.  This is needed for the JIT "
    "and Static Python to function.");
PyObject* install_frame_evaluator(PyObject* /* mod */, PyObject* /* args */) {
  if (Ci_InitFrameEvalFunc() < 0) {
    return nullptr;
  }
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    remove_frame_evaluator_doc,
    "Remove the CinderX frame evaluator.  This is DANGEROUS to do in the "
    "middle of Python execution, as code that has been compiled with the "
    "CinderX bytecode compiler (e.g. Static Python) will not work with the "
    "standard CPython frame evaluator.  This function should generally be "
    "avoided, and is exposed primarily for testing purposes.");
PyObject* remove_frame_evaluator(PyObject* /* mod */, PyObject* /* args */) {
  Ci_FiniFrameEvalFunc();
  Py_RETURN_NONE;
}

PyDoc_STRVAR(
    is_frame_evaluator_installed_doc,
    "Check whether the CinderX frame evaluator is currently installed.");
PyObject* is_frame_evaluator_installed(
    PyObject* /* mod */,
    PyObject* /* args */) {
#ifdef ENABLE_INTERPRETER_LOOP
#if defined(ENABLE_EVAL_HOOK)
  return PyBool_FromLong(Ci_hook_EvalFrame == Ci_EvalFrame);
#elif defined(ENABLE_PEP523_HOOK)
  auto interp = _PyInterpreterState_GET();
  auto current_eval_frame = _PyInterpreterState_GetEvalFrameFunc(interp);
  return PyBool_FromLong(current_eval_frame == Ci_EvalFrame);
#endif
#endif
  Py_RETURN_FALSE;
}

// Check if Python code is still being executed.
bool isCodeRunning() {
  PyThreadState* tstate = PyThreadState_Get();
#if PY_VERSION_HEX < 0x030D0000
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

  // If the module was never fully initialized (e.g. subinterpreter), just
  // destroy the state object and return. Skip all global cleanup since it
  // belongs to the main interpreter.
  if (!state->fully_initialized) {
    state->cinderx::ModuleState::~ModuleState();
    return;
  }

  _PyClassLoader_ClearCache();
  _PyClassLoader_ClearValueCache();

  // If any Python code is running we can't tell if JIT code is in use. Even if
  // every frame in the callstack is interpreter-owned, some of them could be
  // the result of deopt and JIT code may still be on the native stack.
  JIT_CHECK(!isCodeRunning(), "Python code still running on CinderX unload");

  Ci_FiniFrameEvalFunc();

  jit::finalize();

  finiCodeExtraIndex();

  // This must be done at the point the module is free'd as the free-list uses
  // data backed by the module state. The free-list will use the module refcount
  // to keep the module alive while such uses are outstanding.
  jit::shutdown_jit_genobject_type();

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
    {"install_frame_evaluator",
     install_frame_evaluator,
     METH_NOARGS,
     install_frame_evaluator_doc},
    {"remove_frame_evaluator",
     remove_frame_evaluator,
     METH_NOARGS,
     remove_frame_evaluator_doc},
    {"is_frame_evaluator_installed",
     is_frame_evaluator_installed,
     METH_NOARGS,
     is_frame_evaluator_installed_doc},
    {"clear_caches",
     clear_caches,
     METH_NOARGS,
     PyDoc_STR(
         "Clears caches associated with the JIT.  This may have a "
         "negative effect on performance of existing JIT compiled code.")},
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
  // The state will be destroyed in module_free(), which gets called even if
  // this function exits early with an error. Construct it first so that
  // module_free always has a valid object to work with.
  void* state_mem = PyModule_GetState(m);
  auto state = new (state_mem) cinderx::ModuleState();

  // CinderX does not support subinterpreters. Bail out early to avoid
  // corrupting the global module state that belongs to the main interpreter.
  // The state is left with fully_initialized == false so that module_free()
  // skips global cleanup.
  PyInterpreterState* interp = PyInterpreterState_Get();
  if (interp != PyInterpreterState_Main()) {
    PyErr_SetString(
        PyExc_ImportError,
        "The _cinderx extension does not support subinterpreters");
    return -1;
  }

  cinderx::initStaticObjects();

  // The JIT is going to need the Python function entrypoint during its
  // initialization.
  ensurePyFunctionVectorcall();

  cinderx::setModuleState(m);

  auto cache_manager = new (std::nothrow) jit::GlobalCacheManager();
  if (cache_manager == nullptr) {
    return -1;
  }

  state->cache_manager.reset(cache_manager);

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
  state->builtin_next = Ref<>::create(next);

  auto async_lazy_value = new (std::nothrow) cinderx::AsyncLazyValueState();
  if (async_lazy_value == nullptr) {
    return -1;
  } else if (!async_lazy_value->init()) {
    delete async_lazy_value;
    return -1;
  }

  state->async_lazy_value.reset(async_lazy_value);

  PyTypeObject* gen_type = (PyTypeObject*)PyType_FromSpec(&jit::JitGen_Spec);
  if (gen_type == nullptr) {
    return -1;
  }
  state->gen_type = Ref<PyTypeObject>::steal(gen_type);

  PyTypeObject* coro_type = (PyTypeObject*)PyType_FromSpec(&jit::JitCoro_Spec);
  if (coro_type == nullptr) {
    return -1;
  }
  state->coro_type = Ref<PyTypeObject>::steal(coro_type);

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
  state->frame_reifier = Ref<>::create(reifier);

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
  state->anext_awaitable_type = Ref<PyTypeObject>::steal(anext_awaitable_type);

  auto anext_func = Ref<>::steal(PyObject_GetAttrString(m, "anext"));
  if (anext_func == nullptr ||
      PyObject_SetAttrString(builtins_mod, "anext", anext_func) < 0) {
    return -1;
  }

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

  auto symbolizer = new (std::nothrow) jit::Symbolizer();
  if (symbolizer == nullptr) {
    return -1;
  }
  state->symbolizer.reset(symbolizer);

  if (!state->initBuiltinMembers()) {
    return -1;
  }

  auto& watcher_state = state->watcher_state;
  watcher_state.setCodeWatcher(cinderx_code_watcher);
  watcher_state.setDictWatcher(cinderx_dict_watcher);
  watcher_state.setFuncWatcher(cinderx_func_watcher);
  watcher_state.setTypeWatcher(cinderx_type_watcher);

  PyObject* static_type_error = PyErr_NewException(
      "cinderx.StaticTypeError", PyExc_TypeError, nullptr /* dict */);
  if (static_type_error == nullptr) {
    return -1;
  }
  JIT_CHECK(
      PyType_Check(static_type_error),
      "Created StaticTypeError but it isn't a type object");
  state->static_type_error = Ref<PyTypeObject>::steal(static_type_error);

  if (PyType_Ready(&PyCachedProperty_Type) < 0) {
    return -1;
  }
  PyCachedPropertyWithDescr_Type.tp_base = &PyCachedProperty_Type;
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
  if (PyType_Ready(&jit::_JitCoroWrapper_Type) < 0) {
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
  ADDITEM("AsyncLazyValue", async_lazy_value->asyncLazyValueType());
  ADDITEM("AwaitableValue", async_lazy_value->awaitableValueType());

#undef ADDITEM

  // We don't want a low level callback which is called pretty late after
  // sys.modules has been cleared. Instead we want to be called before that
  // happens so we can clear out strict modules first, so we register
  // directly with the atexit library.
  auto atexit = Ref<>::steal(PyImport_ImportModule("atexit"));
  if (atexit == nullptr) {
    return -1;
  }
  auto register_func = Ref<>::steal(PyObject_GetAttrString(atexit, "register"));
  if (register_func == nullptr) {
    return -1;
  }
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

  // Get the existing cache clear function so we can forward to it.
  const char* clear_name;
  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    clear_name = "_clear_internal_caches";
  } else {
    clear_name = "_clear_type_cache";
  }
  BorrowedRef<> clear_type_cache = PySys_GetObject(clear_name);
  state->sys_clear_caches = Ref<>::create(clear_type_cache);

  // Replace sys._clear_type_cache with our clearing function
  Ref<> clear_caches = Ref<>::steal(PyObject_GetAttrString(m, "clear_caches"));
  if (clear_caches == nullptr ||
      PySys_SetObject(clear_name, clear_caches) < 0) {
    return -1;
  }

  if (init_upstream_borrow() < 0) {
    return -1;
  }

  // Initialize the code object extra data index early, before we hook into the
  // interpreter and try to use it.
  initCodeExtraIndex();

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

  char* patching = getenv("PYTHONENABLEPATCHING");
  state->enable_patching = patching != nullptr && strcmp(patching, "1") == 0;

#ifdef ENABLE_INTERPRETER_LOOP
  Ci_InitOpcodes();
#endif

#ifdef ENABLE_XXCLASSLOADER
  if (_Ci_CreateXXClassLoaderModule() < 0) {
    return -1;
  }
#endif

  if (_Ci_CreateStaticModule() < 0) {
    return -1;
  }

  state->fully_initialized = true;
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
    Ci_FiniFrameEvalFunc();
  }
  return result;
}

PyModuleDef_Slot _cinderx_slots[] = {
    {Py_mod_exec, reinterpret_cast<void*>(_cinderx_exec)},
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_NOT_SUPPORTED},
#if PY_VERSION_HEX >= 0x030E0000
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
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
