// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/generators_core.h"

#include "cinderx/module_state.h"

#if PY_VERSION_HEX >= 0x030C0000

#include "internal/pycore_frame.h"

#include "cinderx/Common/py-portability.h"

namespace jit {

bool jitgen_is_coroutine(PyObject* o) {
  if (Py_TYPE(o) != cinderx::getModuleState()->genType() &&
      !PyGen_CheckExact(o)) {
    return false;
  }

  // Do not use PyGen_GetCode() as this asserts on PyGen_Check().
  BorrowedRef<PyGenObject> base_gen{o};
  auto gen_frame = generatorFrame(base_gen);
  BorrowedRef<PyCodeObject> code = _PyFrame_GetCode(gen_frame);
  return code->co_flags & CO_ITERABLE_COROUTINE;
}

} // namespace jit

extern "C" {
int JitGen_CheckExact(PyObject* o) {
  return Py_TYPE(o) == cinderx::getModuleState()->genType();
}

int JitCoro_CheckExact(PyObject* o) {
  return Py_TYPE(o) == cinderx::getModuleState()->coroType();
}

// This is a slightly modified version of _PyCoro_GetAwaitableIter. It
// could be a lot shorter and still correct if it just checked whether the input
// is a JIT coro and deferred to the original implementation if not. However, if
// we do this the error message produced when an __await__() function
// produces a JIT coroutine is very slightly different from what
// test_coroutines.CoroutineTest.test_await_12 expects. So we pull in the rest
// and tweak the following check too just to make the test pass.
PyObject* JitCoro_GetAwaitableIter(PyObject* o) {
  unaryfunc getter = nullptr;
  PyTypeObject* ot;

  if (JitCoro_CheckExact(o) || PyCoro_CheckExact(o) ||
      jit::jitgen_is_coroutine(o)) {
    /* 'o' is a coroutine. */
    return Py_NewRef(o);
  }

  ot = Py_TYPE(o);
  if (ot->tp_as_async != nullptr) {
    getter = ot->tp_as_async->am_await;
  }
  if (getter != nullptr) {
    PyObject* res = (*getter)(o);
    if (res != nullptr) {
      if (JitCoro_CheckExact(res) || PyCoro_CheckExact(res) ||
          jit::jitgen_is_coroutine(res)) {
        /* __await__ must return an *iterator*, not
           a coroutine or another awaitable (see PEP 492) */
        if constexpr (PY_VERSION_HEX >= 0x030F0000) {
          PyErr_Format(
              PyExc_TypeError,
              "%T.__await__() must return an iterator, "
              "not coroutine",
              o);
        } else {
          PyErr_SetString(PyExc_TypeError, "__await__() returned a coroutine");
        }
        Py_CLEAR(res);
      } else if (!PyIter_Check(res)) {
        if constexpr (PY_VERSION_HEX >= 0x030F0000) {
          PyErr_Format(
              PyExc_TypeError,
              "%T.__await__() must return an iterator, "
              "not %T",
              o,
              res);
        } else {
          PyErr_Format(
              PyExc_TypeError,
              "__await__() returned non-iterator "
              "of type '%.100s'",
              Py_TYPE(res)->tp_name);
        }
        Py_CLEAR(res);
      }
    }
    return res;
  }

  PyErr_Format(
      PyExc_TypeError,
#if PY_VERSION_HEX >= 0x030E0000
      "'%.100s' object can't be awaited",
#else
      "object %.100s can't be used in 'await' expression",
#endif
      ot->tp_name);
  return nullptr;
}
}

#endif
