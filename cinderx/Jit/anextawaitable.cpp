// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/anextawaitable.h"

#include "internal/pycore_genobject.h"
#include "internal/pycore_object.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Jit/generators_rt.h"
#include "cinderx/module_state.h"

namespace jit {

namespace {

struct ANextAwaitableObject {
  PyObject_HEAD
  PyObject* wrapped;
  PyObject* default_value;
};

Ref<> anextawaitable_getiter(ANextAwaitableObject* obj) {
  JIT_DCHECK(obj->wrapped != nullptr, "anextawaitable has no wrapped object");
  Ref<> awaitable = Ref<>::steal(JitCoro_GetAwaitableIter(obj->wrapped));
  if (awaitable == nullptr) {
    return nullptr;
  }
  if (Py_TYPE(awaitable)->tp_iternext == nullptr) {
    // JitCoro_GetAwaitableIter returns a Coroutine, a Generator,
    // or an iterator. Of these, only coroutines lack tp_iternext.
    JIT_DCHECK(
        JitCoro_CheckExact(awaitable) || PyCoro_CheckExact(awaitable),
        "awaitable without tp_iternext should be a coroutine");
    unaryfunc getter = Py_TYPE(awaitable)->tp_as_async->am_await;
    Ref<> new_awaitable = Ref<>::steal(getter(awaitable));
    if (new_awaitable == nullptr) {
      return nullptr;
    }
    awaitable = std::move(new_awaitable);
    if (!PyIter_Check(awaitable)) {
      PyErr_Format(
          PyExc_TypeError,
          "%T.__await__() must return an iterable, not %T",
          obj,
          awaitable.get());
      return nullptr;
    }
  }
  return awaitable;
}

PyObject* anextawaitable_proxy(
    ANextAwaitableObject* obj,
    const char* meth,
    PyObject* arg) {
  Ref<> awaitable = anextawaitable_getiter(obj);
  if (awaitable == nullptr) {
    return nullptr;
  }
  // When specified, 'arg' may be a tuple (if coming from a METH_VARARGS
  // method) or a single object (if coming from a METH_O method).
  Ref<> ret = Ref<>::steal(
      arg == nullptr ? PyObject_CallMethod(awaitable, meth, nullptr)
                     : PyObject_CallMethod(awaitable, meth, "O", arg));
  if (ret != nullptr) {
    return ret.release();
  }
  if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
    // ANextAwaitableObject is only used by anext() when a default value is
    // provided. So when we have a StopAsyncIteration exception we replace it
    // with a StopIteration(default), as if it was the return value of
    // __anext__() coroutine.
    PyErr_Clear();
    _PyGen_SetStopIterationValue(obj->default_value);
  }
  return nullptr;
}

void anextawaitable_dealloc(ANextAwaitableObject* obj) {
  PyTypeObject* type = Py_TYPE(obj);
  PyObject_GC_UnTrack(obj);
  Py_XDECREF(obj->wrapped);
  Py_XDECREF(obj->default_value);
  PyObject_GC_Del(obj);
  // Heap types increment their type, so we need to decrement it here:
  Py_DECREF(type);
}

int anextawaitable_traverse(
    ANextAwaitableObject* obj,
    visitproc visit,
    void* arg) {
  Py_VISIT(obj->wrapped);
  Py_VISIT(obj->default_value);
  return 0;
}

PyObject* anextawaitable_iternext(ANextAwaitableObject* obj) {
  Ref<> awaitable = anextawaitable_getiter(obj);
  if (awaitable == nullptr) {
    return nullptr;
  }
  Ref<> result = Ref<>::steal((*Py_TYPE(awaitable)->tp_iternext)(awaitable));
  if (result != nullptr) {
    return result.release();
  }
  if (PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
    PyErr_Clear();
    _PyGen_SetStopIterationValue(obj->default_value);
  }
  return nullptr;
}

PyObject* anextawaitable_send(ANextAwaitableObject* obj, PyObject* arg) {
  return anextawaitable_proxy(obj, "send", arg);
}

PyObject* anextawaitable_throw(ANextAwaitableObject* obj, PyObject* arg) {
  return anextawaitable_proxy(obj, "throw", arg);
}

PyObject* anextawaitable_close(
    ANextAwaitableObject* obj,
    [[maybe_unused]] PyObject* arg) {
  return anextawaitable_proxy(obj, "close", nullptr);
}

PyMethodDef anextawaitable_methods[] = {
    {"send", reinterpret_cast<PyCFunction>(anextawaitable_send), METH_O, ""},
    {"throw",
     reinterpret_cast<PyCFunction>(anextawaitable_throw),
     METH_VARARGS,
     ""},
#if PY_VERSION_HEX >= 0x030E0000
    {"close",
     reinterpret_cast<PyCFunction>(anextawaitable_close),
     METH_NOARGS,
     ""},
#else
    {"close",
     reinterpret_cast<PyCFunction>(anextawaitable_close),
     METH_VARARGS,
     ""},
#endif
    {} // Sentinel
};

} // namespace

PyType_Slot anext_awaitable_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(anextawaitable_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(anextawaitable_traverse)},
    {Py_tp_methods, anextawaitable_methods},
    {Py_tp_iternext, reinterpret_cast<void*>(anextawaitable_iternext)},
    {Py_tp_iter, reinterpret_cast<void*>(PyObject_SelfIter)},
    {Py_am_await, reinterpret_cast<void*>(PyObject_SelfIter)},
    {0, nullptr},
};

PyType_Spec JitAnextAwaitable_Spec = {
    .name = "builtins.anext_awaitable",
    .basicsize = sizeof(ANextAwaitableObject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = anext_awaitable_slots,
};

// This is the same as CPython's anextawaitable except it also recognizes
// JIT coroutines. Most of the code is CPython's implementation but converted
// to C++. This code is small, rarely changes, and we use it across multiple
// versions so it's just easier to maintain our own version.
PyObject* JitGen_AnextAwaitable_New(
    cinderx::ModuleState* moduleState,
    PyObject* awaitable,
    PyObject* defaultValue) {
  ANextAwaitableObject* anext =
      PyObject_GC_New(ANextAwaitableObject, moduleState->anext_awaitable_type);
  if (anext == nullptr) {
    return nullptr;
  }
  anext->wrapped = Py_NewRef(awaitable);
  anext->default_value = Py_NewRef(defaultValue);
  PyObject_GC_Track(anext);
  return reinterpret_cast<PyObject*>(anext);
}

} // namespace jit
