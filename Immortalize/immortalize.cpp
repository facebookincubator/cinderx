#include "cinderx/Immortalize/immortalize.h"

#include "internal/pycore_gc.h"

#include "cinderx/Common/code.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove

#define FROM_GC(g) ((PyObject*)(((PyGC_Head*)g) + 1))
#define GEN_HEAD(state, n) (&(state)->generations[n].head)
typedef struct _gc_runtime_state GCState;

struct _gc_runtime_state* get_gc_state() {
  return &PyInterpreterState_Get()->gc;
}

namespace {

int immortalize_object(PyObject* obj, PyObject* /* args */) {
  if (_Py_IsImmortal(obj)) {
    return 0;
  }

  IMMORTALIZE(obj);

  if (PyCode_Check(obj)) {
    PyCodeObject* code = reinterpret_cast<PyCodeObject*>(obj);
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
    IMMORTALIZE(code->co_filename);
    IMMORTALIZE(code->co_name);
    IMMORTALIZE(code->co_linetable);
  }

  /* Cache the hash value of unicode object to reduce Copy-on-writes */
  if (PyUnicode_CheckExact(obj)) {
    PyObject_Hash(obj);
  }

  if (PyType_Check(obj)) {
    PyUnstable_Type_AssignVersionTag(reinterpret_cast<PyTypeObject*>(obj));
  }
  return 0;
}

} // namespace

#if PY_VERSION_HEX > 0x030C0000
PyObject* immortalize_heap(PyObject* mod, PyObject* /* args */) {
  fprintf(stderr, "Recursive heap walk for immortalization\n");

  /* Remove any dead objects to avoid immortalizing them */
  PyGC_Collect();

  /* Move all instances into the permanent generation */
  Cix_gc_freeze_impl(mod);

  /* Immortalize all instances in the permanent generation */
  struct _gc_runtime_state* gcstate = get_gc_state();
  PyGC_Head* list = &gcstate->permanent_generation.head;
  for (PyGC_Head* gc = _PyGCHead_NEXT(list); gc != list;
       gc = _PyGCHead_NEXT(gc)) {
    immortalize_object(FROM_GC(gc), nullptr);
    Py_TYPE(FROM_GC(gc))
        ->tp_traverse(
            FROM_GC(gc),
            reinterpret_cast<visitproc>(immortalize_object),
            nullptr);
  }

  Py_RETURN_NONE;
#else
PyObject* immortalize_heap(PyObject* /* mod */, PyObject* /* args */) {
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

PyObject* is_immortal(PyObject* obj) {
  return PyBool_FromLong(_Py_IsImmortal(obj));
}
