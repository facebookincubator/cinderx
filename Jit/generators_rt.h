// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <Python.h>

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_frame.h"

#include "cinderx/Common/py-portability.h"
#include "cpython/genobject.h"

#endif

#ifdef __cplusplus

#include "cinderx/module_state.h" // @donotremove
namespace jit {

#if PY_VERSION_HEX < 0x030C0000

template <typename PyObjectT>
int JitGen_CheckAny(PyObjectT*) {
  return 0;
}

#else

struct GenDataFooter;
extern PyType_Spec JitGen_Spec;
extern PyType_Spec JitCoro_Spec;
extern PyType_Spec JitAnextAwaitable_Spec;

template <typename PyObjectT>
int JitGen_CheckAny(PyObjectT* op) {
  return Py_IS_TYPE(
             reinterpret_cast<PyObject*>(op),
             cinderx::getModuleState()->genType()) ||
      Py_IS_TYPE(
             reinterpret_cast<PyObject*>(op),
             cinderx::getModuleState()->coroType());
}

struct JitGenObject : PyGenObject {
  JitGenObject() = delete;

  template <typename PyGenObjectT>
  static JitGenObject* cast(PyGenObjectT* gen) {
    return JitGen_CheckAny(gen) ? reinterpret_cast<JitGenObject*>(gen)
                                : nullptr;
  }

  GenDataFooter** genDataFooterPtr() {
    // TODO(T209501671): This has way too much going on. If we made PyGenObject
    // use PyObject_VAR_HEAD like it probably should this would get simpler. If
    // we expanded the allocation to include the GenDataFooter it'd get simpler
    // still.
    auto gen_frame = reinterpret_cast<_PyInterpreterFrame*>(gi_iframe);
    int python_frame_data_bytes =
        _PyFrame_NumSlotsForCodeObject(_PyFrame_GetCode(gen_frame)) *
        cinderx::getModuleState()->genType()->tp_itemsize;
    // A *pointer* to JIT data comes after all the other data in the default
    // generator object.
    return reinterpret_cast<jit::GenDataFooter**>(
        reinterpret_cast<uintptr_t>(this) +
        cinderx::getModuleState()->genType()->tp_basicsize +
        python_frame_data_bytes);
  }

  GenDataFooter* genDataFooter() {
    return *genDataFooterPtr();
  }

  PyObject* yieldFrom();
};

// Converts a JitGenObject into a regular PyGenObject. This assumes deopting
// the associated frame will be done elsewhere.
void deopt_jit_gen_object_only(JitGenObject* gen);

// Fully deopt a generator so it'll be ready for use in the interpreter. Note
// this cannot be done on a currently executing JIT generator and will return
// false in this case. The caller should issue an appropriate error.
bool deopt_jit_gen(PyObject* op);
inline bool deopt_jit_gen(PyGenObject* gen) {
  return deopt_jit_gen(reinterpret_cast<PyObject*>(gen));
}

void init_jit_genobject_type();

PyObject* JitGen_AnextAwaitable_New(
    cinderx::ModuleState* moduleState,
    PyObject* awaitable,
    PyObject* defaultValue);

#endif // PY_VERSION_HEX >= 0x030C0000

} // namespace jit
#endif // __cplusplus

#if PY_VERSION_HEX >= 0x030C0000

#ifdef __cplusplus
extern "C" {
#endif

int JitGen_CheckExact(PyObject* o);
int JitCoro_CheckExact(PyObject* o);
PyObject* JitCoro_GetAwaitableIter(PyObject* o);
PyObject* JitGen_yf(PyGenObject* gen);

#ifdef __cplusplus
}
#endif

#endif // PY_VERSION_HEX >= 0x030C0000
