// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Immortalize/immortalize.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove

#include <vector>

namespace cinderx {

namespace {

#define FROM_GC(g) ((PyObject*)(((PyGC_Head*)g) + 1))
#define GEN_HEAD(state, n) (&(state)->generations[n].head)

struct _gc_runtime_state* get_gc_state() {
  return &PyInterpreterState_Get()->gc;
}

// Covers dict subclasses too: GC traversal skips their string keys.
void immortalize_dict_entries(PyObject* obj) {
  PyObject* key;
  PyObject* value;
  Py_ssize_t pos = 0;
  // PyDict_Next() can resolve lazy imports when values are requested. When
  // lazy imports are enabled, heap immortalization must preserve them during
  // prepare-for-fork.
#ifdef ENABLE_LAZY_IMPORTS
  while (_PyDict_NextKeepLazy(obj, &pos, &key, &value)) {
#else
  while (PyDict_Next(obj, &pos, &key, &value)) {
#endif
    immortalize(key);
    immortalize(value);
  }
}

// The heap walk never reaches items of untracked tuples. Nested tuples are
// followed iteratively so deep nesting can't exhaust the stack. Tuples that
// are already immortal are skipped: their items were covered when they were
// immortalized. Only exact tuples: some tuple subclasses, such as Static
// Python's vtable, don't store items in the tuple layout.
void immortalize_tuple_items(PyObject* root) {
  std::vector<PyObject*> pending{root};
  while (!pending.empty()) {
    PyObject* tuple = pending.back();
    pending.pop_back();
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(tuple); i++) {
      PyObject* item = PyTuple_GET_ITEM(tuple, i);
      if (item == nullptr || !PyTuple_CheckExact(item)) {
        immortalize(item);
      } else if (can_immortalize(item)) {
        IMMORTALIZE(item);
        pending.push_back(item);
      }
    }
  }
}

} // namespace

bool can_immortalize(PyObject* obj) {
  if (obj == nullptr || _Py_IsImmortal(obj)) {
    return false;
  }

  // Python 3.12 will assert that strings that are immortalized are also
  // interned in debug builds.  This is purely a debug check, it's fine to do in
  // optimized builds.
  if constexpr (kPyDebug) {
    return !PyUnicode_Check(obj);
  }

  return true;
}

bool immortalize(PyObject* obj) {
  if (!can_immortalize(obj)) {
    return false;
  }

  IMMORTALIZE(obj);

  if (PyTuple_CheckExact(obj)) {
    immortalize_tuple_items(obj);
  }

  if (PyDict_Check(obj)) {
    immortalize_dict_entries(obj);
  }

  if (PyCode_Check(obj)) {
    BorrowedRef<PyCodeObject> code{obj};
    codeExtra(code);
    IMMORTALIZE(code->co_localspluskinds);
    IMMORTALIZE(code->co_linetable);
    IMMORTALIZE(code->co_exceptiontable);

    // Code objects are not GC-traversed in 3.12.
    immortalize(code->co_localsplusnames);
    immortalize(code->co_consts);
    immortalize(code->co_names);

    // These are strings and we need to check if this is safe.
    immortalize(code->co_filename);
    immortalize(code->co_name);
    immortalize(code->co_qualname);
  }

  /* Cache the hash value of unicode object to reduce Copy-on-writes */
  if (PyUnicode_CheckExact(obj)) {
    PyObject_Hash(obj);
  }

  if (PyType_Check(obj)) {
    PyUnstable_Type_AssignVersionTag(reinterpret_cast<PyTypeObject*>(obj));
  }

  return true;
}

PyObject* immortalize_heap([[maybe_unused]] PyObject* mod) {
  if constexpr (kFreeThreadedBuild) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "Immortalizing the heap is not yet supported in FT Python");
    return nullptr;
  }

  // TODO(T251571267): Low priority for now.
  /* Remove any dead objects to avoid immortalizing them */
  PyGC_Collect();

  /* Move all instances into the permanent generation */
  Ref<> gc_mod = Ref<>::steal(PyImport_ImportModule("gc"));
  if (!gc_mod) {
    return nullptr;
  }
  Ref<> freeze_result =
      Ref<>::steal(PyObject_CallMethod(gc_mod, "freeze", nullptr));
  if (!freeze_result) {
    return nullptr;
  }

  /* Immortalize all instances in the permanent generation */
  struct _gc_runtime_state* gcstate = get_gc_state();
  PyGC_Head* list = &gcstate->permanent_generation.head;
  for (PyGC_Head* gc = _PyGCHead_NEXT(list); gc != list;
       gc = _PyGCHead_NEXT(gc)) {
    immortalize(FROM_GC(gc));

    auto immortalize_visitor = [](PyObject* obj, void*) {
      immortalize(obj);
      return 0;
    };
    Py_TYPE(FROM_GC(gc))
        ->tp_traverse(FROM_GC(gc), immortalize_visitor, nullptr);
  }

  Py_RETURN_NONE;
}

} // namespace cinderx
