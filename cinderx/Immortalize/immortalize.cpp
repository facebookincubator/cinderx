// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Immortalize/immortalize.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove

#define FROM_GC(g) ((PyObject*)(((PyGC_Head*)g) + 1))
#define GEN_HEAD(state, n) (&(state)->generations[n].head)
using GCState = struct _gc_runtime_state;

struct _gc_runtime_state* get_gc_state() {
  return &PyInterpreterState_Get()->gc;
}

bool can_immortalize(PyObject* obj) {
  if (obj == nullptr || _Py_IsImmortal(obj)) {
    return false;
  }

  // Python 3.12 will assert that strings that are immortalized are also
  // interned in debug builds.  This is purely a debug check, it's fine to do in
  // optimized builds.
  if constexpr (PY_VERSION_HEX >= 0x030C0000 && kPyDebug) {
    return !PyUnicode_Check(obj);
  }

  return true;
}

bool immortalize(PyObject* obj) {
  if (!can_immortalize(obj)) {
    return false;
  }

  IMMORTALIZE(obj);

  if (PyCode_Check(obj)) {
    BorrowedRef<PyCodeObject> code{obj};
    codeExtra(code);
#if PY_VERSION_HEX < 0x030B0000
    // In 3.11 these changed to have the bytes embedded in the code object and
    // the names in a unified tuple
    IMMORTALIZE(PyCode_GetCode(code));
    IMMORTALIZE(PyCode_GetVarnames(code));
    IMMORTALIZE(PyCode_GetFreevars(code));
    IMMORTALIZE(PyCode_GetCellvars(code));
#else
    IMMORTALIZE(code->co_localspluskinds);
    IMMORTALIZE(code->co_localsplusnames);
#endif
    IMMORTALIZE(code->co_consts);
    IMMORTALIZE(code->co_names);
    IMMORTALIZE(code->co_linetable);

    // These are strings and we need to check if this is safe.
    immortalize(code->co_filename);
    immortalize(code->co_name);
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

#if PY_VERSION_HEX >= 0x030C0000
PyObject* immortalize_heap(PyObject* mod) {
  /* Remove any dead objects to avoid immortalizing them */
  PyGC_Collect();

  /* Move all instances into the permanent generation */
  Cix_gc_freeze_impl(mod);

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
#else
PyObject* immortalize_heap(PyObject* /* mod */) {
  // for 3.10.cinder, we fall back to the implementation that ships in the gc
  // module NOTE: this isn't a documented API, so I'm mostly adding it for
  // parity, but it shouldn't actually be used anywhere
  Ref<> gcmodule = Ref<>::steal(PyImport_ImportModule("gc"));
  if (!gcmodule) {
    return nullptr;
  }
  Ref<> immortalize =
      Ref<>::steal(PyObject_GetAttrString(gcmodule, "immortalize_heap"));
  if (!immortalize) {
    return nullptr;
  }
  return Ref<>::steal(PyObject_CallFunctionObjArgs(immortalize, nullptr));
#endif
}
