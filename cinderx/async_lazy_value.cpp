// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/async_lazy_value.h"

#include "cinderx/Jit/generators_core.h"

// string.h and py-portability import pycore_ headers which bring in
// atomics breaking things.
// clang-format off
#include "cinderx/module_state.h"
#include "cinderx/Common/string.h"
#include "cinderx/Common/py-portability.h"
// clang-format on

#if PY_VERSION_HEX >= 0x030C0000

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_modsupport.h"
#include "internal/pycore_object.h"
#endif

extern "C" PyType_Spec _AsyncLazyValue_Spec;
extern "C" PyType_Spec _AsyncLazyValueCompute_Spec;
extern "C" PyType_Spec AwaitableValue_Spec;
extern "C" PyType_Spec _MethodTable_RefSpec;
extern "C" PyMethodDef _MethodTableRefCallback;

#ifdef ENABLE_GENERATOR_AWAITER
extern "C" Ci_AsyncMethodsWithExtra _async_lazy_value_compute_type_as_async;
#else
extern "C" PyAsyncMethods _async_lazy_value_compute_type_as_async;

#ifdef Ci_TPFLAGS_HAVE_AM_EXTRA
#undef Ci_TPFLAGS_HAVE_AM_EXTRA
#endif
#define Ci_TPFLAGS_HAVE_AM_EXTRA 0
#endif

extern "C" {

static cinderx::AsyncLazyValueState* get_state();

static void populate_method_table(
    PyMethodTableRef* tableref,
    PyTypeObject* type);

} // extern "C"

namespace cinderx {

Ref<PyMethodTableRef> init_table(PyTypeObject* t);

bool AsyncLazyValueState::init() {
  async_lazy_value_type_ = Ref<>::steal(PyType_FromSpec(&_AsyncLazyValue_Spec));
  if (async_lazy_value_type_ == nullptr) {
    return false;
  }

  async_lazy_value_compute_type_ =
      Ref<>::steal(PyType_FromSpec(&_AsyncLazyValueCompute_Spec));
  if (async_lazy_value_compute_type_ == nullptr) {
    return false;
  }
  async_lazy_value_compute_type_->tp_as_async =
      (PyAsyncMethods*)&_async_lazy_value_compute_type_as_async;

  awaitable_value_type_ = Ref<>::steal(PyType_FromSpec(&AwaitableValue_Spec));
  if (awaitable_value_type_ == nullptr) {
    return false;
  }

  _method_table_ref_type_ = Ref<PyTypeObject>::steal(PyType_FromSpecWithBases(
      &_MethodTable_RefSpec, (PyObject*)&_PyWeakref_RefType));
  if (_method_table_ref_type_ == nullptr) {
    return false;
  }

  methodref_callback_ =
      Ref<>::steal(PyCFunction_New(&_MethodTableRefCallback, nullptr));
  if (methodref_callback_ == nullptr) {
    return false;
  }

  return true;
}

BorrowedRef<> AsyncLazyValueState::getEventLoop() {
  if (get_event_loop_ != nullptr) {
    return get_event_loop_;
  }
  Ref<> asyncio = Ref<>::steal(PyImport_ImportModule("_asyncio"));
  if (asyncio == nullptr) {
    return nullptr;
  }
  get_event_loop_ =
      Ref<>::steal(PyObject_GetAttrString(asyncio, "get_event_loop"));
  return get_event_loop_;
}

BorrowedRef<PyTypeObject> AsyncLazyValueState::futureType() {
  if (future_type_ != nullptr) {
    return future_type_;
  }
  Ref<> asyncio = Ref<>::steal(PyImport_ImportModule("_asyncio"));
  if (asyncio == nullptr) {
    return nullptr;
  }
  future_type_ = Ref<>::steal(PyObject_GetAttrString(asyncio, "Future"));
  return future_type_;
}

BorrowedRef<> AsyncLazyValueState::asyncioFutureBlocking() {
  if (asyncio_future_blocking_ != nullptr) {
    return asyncio_future_blocking_;
  }
  auto future_type = futureType();
  if (future_type == nullptr) {
    return nullptr;
  }
  asyncio_future_blocking_ = Ref<>::steal(
      PyObject_GetAttrString(future_type, "_asyncio_future_blocking"));

  return asyncio_future_blocking_;
}

BorrowedRef<PyTypeObject> AsyncLazyValueState::cancalledError() {
  if (cancelled_error_ != nullptr) {
    return cancelled_error_;
  }
  Ref<> exceptions = Ref<>::steal(PyImport_ImportModule("asyncio.exceptions"));
  if (exceptions == nullptr) {
    return nullptr;
  }
  cancelled_error_ =
      Ref<>::steal(PyObject_GetAttrString(exceptions, "CancelledError"));
  return cancelled_error_;
}

// Lookups up a get/set with the specific name. Returns the function if it's
// found or Py_None if it isn't.
Ref<PyGetSetDescrObject> AsyncLazyValueState::lookupFutureGetSet(
    const char* name) {
  auto future_type = futureType();
  if (future_type == nullptr) {
    return Ref<>::create(Py_None);
  }
  auto func = Ref<PyGetSetDescrObject>::steal(
      PyObject_GetAttrString(future_type, name));
  if (func == nullptr || Py_TYPE(func) != &PyGetSetDescr_Type) {
    return Ref<>::create(Py_None);
  }

  return func;
}

BorrowedRef<PyGetSetDescrObject> AsyncLazyValueState::futureSourceTraceback() {
  if (future_source_traceback_ != nullptr) {
    return future_source_traceback_;
  }
  future_source_traceback_ = lookupFutureGetSet("source_traceback");
  return future_source_traceback_;
}

BorrowedRef<PyMethodTableRef> AsyncLazyValueState::futureTableRef() {
  if (future_table_ref_ != nullptr) {
    return future_table_ref_.get();
  }

  auto future_type = futureType();
  if (future_type == nullptr) {
    return nullptr;
  }

  future_table_ref_ = init_table(future_type);
  return future_table_ref_;
}

#define MethodTable_Check(obj) \
  (Py_TYPE(obj) == get_state()->methodTableRefType())

static inline BorrowedRef<PyMethodTableRef> get_method_table(
    PyTypeObject* type) {
  PyWeakReference** weakrefs =
      (PyWeakReference**)PyObject_GET_WEAKREFS_LISTPTR((PyObject*)type);
  if (weakrefs != nullptr) {
    PyWeakReference* p = *weakrefs;
    while (p != nullptr) {
      if (MethodTable_Check(p)) {
        return (PyMethodTableRef*)p;
      }
      p = p->wr_next;
    }
  }
  return nullptr;
}

static Ref<PyMethodTableRef> create_method_table(PyTypeObject* type) {
  PyObject* args = PyTuple_New(2);
  if (args == nullptr) {
    return nullptr;
  }
  PyTuple_SET_ITEM(args, 0, (PyObject*)type);
  Py_INCREF(type);
  PyTuple_SET_ITEM(args, 1, Py_NewRef(get_state()->methodRefCallback()));

  PyMethodTableRef* tableref = (PyMethodTableRef*)_PyWeakref_RefType.tp_new(
      get_state()->methodTableRefType(), args, nullptr);

  Py_DECREF(args);
  if (tableref == nullptr) {
    return nullptr;
  }
  populate_method_table(tableref, type);
  return Ref<PyMethodTableRef>::steal(tableref);
}

static Ref<PyMethodTableRef> get_or_create_method_table(PyTypeObject* type) {
  if (type == get_state()->futureType()) {
    return Ref<PyMethodTableRef>::create(get_state()->futureTableRef());
  }
  // if we saw this exact task type before - we have method table for it
  if (type == get_state()->lastUsedTaskType()) {
    return Ref<PyMethodTableRef>::create(
        get_state()->lastUsedTaskTypeTableRef());
  }
  BorrowedRef<PyMethodTableRef> tableref = get_method_table(type);
  if (tableref != nullptr) {
    return Ref<PyMethodTableRef>::create(tableref);
  }
  return create_method_table(type);
}

Ref<PyMethodTableRef> init_table(PyTypeObject* t) {
  PyMethodTableRef* table = get_method_table(t);
  if (table != nullptr) {
    return Ref<PyMethodTableRef>::create(table);
  }
  return create_method_table(t);
}
} // namespace cinderx

extern "C" {

static cinderx::AsyncLazyValueState* get_state() {
  return static_cast<cinderx::AsyncLazyValueState*>(
      cinderx::getModuleState()->asyncLazyValueState());
}

static PyObject* get_event_loop() {
  return PyObject_CallNoArgs(get_state()->getEventLoop());
}

typedef enum {
  ALV_NOT_STARTED,
  ALV_RUNNING,
  ALV_DONE,
} ASYNC_LAZY_VALUE_STATE;

typedef struct {
  PyObject_HEAD
  PyObject* alv_args;
  PyObject* alv_kwargs;
  PyObject* alv_result;
  PyObject* alv_futures;
  ASYNC_LAZY_VALUE_STATE alv_state;
  // parent AsyncLazyValue that triggered the execution of current
  // AsyncLazyValue
  PyObject* alv_parent;
} AsyncLazyValueObj;

typedef struct {
  PyObject_HEAD
  AsyncLazyValueObj* alvc_target;
  _PyErr_StackItem alvc_exc_state;
  PyObject* alvc_coroobj; // actual coroutine object computing the value of
                          // 'alvc_target'

  // This may be set if someone tries to set the awaiter before we've started
  // running the computation. This happens during non-eager execution because
  // we call Ci_PyAwaitable_SetAwaiter in both the JIT/interpreter before
  // starting the compute object. We'll check for this when we start the
  // computation and call Ci_PyAwaitable_SetAwaiter with the stored value on the
  // awaitable that is created.
  //
  // Stores a borrowed reference.
  PyObject* alvc_pending_awaiter;
} AsyncLazyValueComputeObj;

typedef struct {
  PyObject_HEAD
  PyObject* av_value;
} AwaitableValueObj;

typedef PyObject FutureObj;

static PyObject* _asyncio_Future_done_impl(FutureObj* self) {
  DEFINE_STATIC_STRING(done);
  return PyObject_CallMethodNoArgs(self, s_done);
}

static int future_set_result(FutureObj* fut, PyObject* res) {
  DEFINE_STATIC_STRING(set_result);
  PyObject* ret = PyObject_CallMethodOneArg(fut, s_set_result, res);
  Py_XDECREF(ret);
  if (ret == nullptr) {
    return -1;
  }
  return 0;
}

static int future_set_exception(FutureObj* fut, PyObject* exc) {
  DEFINE_STATIC_STRING(set_exception);
  PyObject* res = PyObject_CallMethodOneArg(fut, s_set_exception, exc);
  Py_XDECREF(res);
  if (res == nullptr) {
    return -1;
  }
  return 0;
}

static int future_cancel_impl(FutureObj* fut, PyObject* msg) {
  DEFINE_STATIC_STRING(cancel);
  PyObject* ret = PyObject_CallMethodOneArg(fut, s_cancel, msg);
  Py_XDECREF(ret);
  if (ret == nullptr) {
    return -1;
  }
  return 0;
}

#define _AsyncLazyValue_CheckExact(obj) \
  (Py_TYPE(obj) == (PyTypeObject*)get_state()->asyncLazyValueType())

#define _AsyncLazyValueCompute_CheckExact(obj) \
  (Py_TYPE(obj) == (PyTypeObject*)get_state()->asyncLazyValueComputeType())

static PyObject* FutureObj_get_traceback(PyObject* fut) {
  return get_state()->futureSourceTraceback()->d_getset->get(fut, nullptr);
}

static PyObject* FutureLike_get_traceback(PyObject* fut) {
  DEFINE_STATIC_STRING(_source_traceback);
  return PyObject_GetAttr(fut, s__source_traceback);
}

/************************ PyMethodTableRef ********************************/

// Auxiliary macros for cases when method tables are used to
// perform actions over awaitables stores in collections.
// In this case they typically all have the same  type so
// we can save bit of time by not pulling the same method table
// over and over again.
#define DECLARE_METHODTABLE(var)                                 \
  PyMethodTableRef *__prevtable_##var = nullptr, *var = nullptr; \
  PyTypeObject* __prevtype_##var = nullptr;

#define FETCH_METHOD_TABLE(var, type)                                \
  var = ((type) == __prevtype_##var)                                 \
      ? __prevtable_##var                                            \
      : (__prevtable_##var =                                         \
             get_or_create_method_table(__prevtype_##var = (type))); \
  assert((var) != nullptr)

static int
methodtableref_traverse(PyMethodTableRef* self, visitproc visit, void* arg) {
  return _PyWeakref_RefType.tp_traverse((PyObject*)self, visit, arg);
}

static int methodtableref_clear(PyMethodTableRef* self) {
  return _PyWeakref_RefType.tp_clear((PyObject*)self);
}

static void methodtable_dealloc(PyMethodTableRef* self) {
  methodtableref_clear(self);
  Py_DECREF(((PyObject*)self)->ob_type);
  _PyWeakref_RefType.tp_dealloc((PyObject*)self);
}

static PyObject* methodtable_callback_impl(
    PyObject* Py_UNUSED(self),
    PyMethodTableRef* tableref) {
  // decref methodtable to make sure it is collected
  Py_DECREF(tableref);
  Py_RETURN_NONE;
}

PyMethodDef _MethodTableRefCallback = {
    "methodtableref_callback",
    (PyCFunction)methodtable_callback_impl,
    METH_O,
    nullptr};

PyType_Slot methodtable_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(methodtable_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(methodtableref_traverse)},
    {Py_tp_clear, reinterpret_cast<void*>(methodtableref_clear)},
    {0, nullptr},
};

PyType_Spec _MethodTable_RefSpec = {
    .name = "cinderx.future_method_table",
    .basicsize = sizeof(PyMethodTableRef),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_IMMUTABLETYPE,
    .slots = methodtable_slots,
};

static PyObject*
lookup_attr(PyTypeObject* type, PyObject* name, PyTypeObject* attr_type) {
  PyObject* t = _PyType_Lookup(type, name); // t is borrowed
  if (t == nullptr || Py_TYPE(t) != attr_type) {
    return nullptr;
  }
  return t;
}

static void populate_method_table(
    PyMethodTableRef* tableref,
    PyTypeObject* type) {
  PyGetSetDescrObject* source_traceback = nullptr;
  DEFINE_STATIC_STRING(_asyncio_future_blocking);
  DEFINE_NAMED_STATIC_STRING(PyId__source_traceback, "_source_traceback");
  if (PyType_IsSubtype(type, get_state()->futureType())) {
    source_traceback = (PyGetSetDescrObject*)lookup_attr(
        type, PyId__source_traceback, &PyGetSetDescr_Type);
  }

  if (source_traceback == get_state()->futureSourceTraceback()) {
    tableref->source_traceback = FutureObj_get_traceback;
  } else {
    tableref->source_traceback = FutureLike_get_traceback;
  }
}

/******************** AsyncLazyValue ************************/

static inline int notify_futures(
    PyObject* futures,
    int (*cb)(FutureObj*, PyObject*),
    PyObject* arg) {
  if (futures == nullptr) {
    return 0;
  }
  Py_ssize_t len = PyList_GET_SIZE(futures);
  for (Py_ssize_t i = 0; i < len; ++i) {
    FutureObj* fut = (FutureObj*)PyList_GET_ITEM(futures, i);
    PyObject* res = _asyncio_Future_done_impl(fut);
    if (res == nullptr) {
      return -1;
    }
    int is_done = PyObject_IsTrue(res);
    Py_DECREF(res);
    if (is_done == -1) {
      return -1;
    }
    if (is_done) {
      continue;
    }
    int ok = cb(fut, arg);
    if (ok < 0) {
      return -1;
    }
  }
  return 0;
}

static int AsyncLazyValue_future_set_result(FutureObj* self, PyObject* res) {
  return future_set_result(self, res);
}

static int AsyncLazyValue_future_set_exception(FutureObj* self, PyObject* res) {
  return future_set_exception(self, res);
}

static int AsyncLazyValue_set_result(AsyncLazyValueObj* self, PyObject* res) {
  self->alv_result = res;
  Py_INCREF(res);
  self->alv_state = ALV_DONE;
  if (notify_futures(self->alv_futures, AsyncLazyValue_future_set_result, res) <
      0) {
    return -1;
  }
  Py_CLEAR(self->alv_args);
  Py_CLEAR(self->alv_kwargs);

  return 0;
}

static int AsyncLazyValue_set_error(AsyncLazyValueObj* self, PyObject* exc) {
  int ok;
  if (PyObject_IsInstance(exc, get_state()->cancalledError())) {
    ok = notify_futures(self->alv_futures, future_cancel_impl, exc);
  } else {
    ok = notify_futures(
        self->alv_futures, AsyncLazyValue_future_set_exception, exc);
  }

  Py_CLEAR(self->alv_futures);
  self->alv_state = ALV_NOT_STARTED;
  return ok;
}

static PyObject* AsyncLazyValue_new_computeobj(AsyncLazyValueObj* self) {
  AsyncLazyValueComputeObj* obj = PyObject_GC_New(
      AsyncLazyValueComputeObj,
      (PyTypeObject*)get_state()->asyncLazyValueComputeType());
  if (obj == nullptr) {
    return nullptr;
  }
  obj->alvc_target = self;
  Py_INCREF(self);

  obj->alvc_coroobj = nullptr;
  obj->alvc_exc_state.exc_value = nullptr;
  obj->alvc_pending_awaiter = nullptr;

  PyObject_GC_Track(obj);
  return (PyObject*)obj;
}

static int
AsyncLazyValue_init(AsyncLazyValueObj* self, PyObject* args, PyObject* kwargs) {
  if (PyTuple_GET_SIZE(args) == 0) {
    PyErr_SetString(PyExc_TypeError, "'coro' argument expected");
    return -1;
  }
  self->alv_args = args;
  Py_INCREF(args);
  self->alv_kwargs = kwargs;
  Py_XINCREF(kwargs);

  self->alv_futures = nullptr;
  self->alv_result = nullptr;
  self->alv_state = ALV_NOT_STARTED;
  return 0;
}

static PyObject* future_new(PyObject* loop) {
  DEFINE_STATIC_STRING(loop)
  PyObject* kwnames = PyTuple_New(1);
  if (kwnames == nullptr) {
    return nullptr;
  }
  PyTuple_SET_ITEM(kwnames, 0, s_loop);
  Py_INCREF(s_loop);

  FutureObj* fut =
      PyObject_Vectorcall(get_state()->futureType(), &loop, 0, kwnames);
  Py_DECREF(kwnames);
  return fut;
}

static PyObject* AsyncLazyValue_new_future(
    AsyncLazyValueObj* self,
    PyObject* loop) {
  FutureObj* fut;
  // if cinder is not present - stack walking is not available so create regular
  // future also create regular future if we have exactly AsyncLazyValue (not a
  // subtype that might add dedicated frame to identify running ALVs)
#if 0
  if (cinder_get_arg0_from_pyframe == nullptr ||
      _AsyncLazyValue_CheckExact(self)) {
#endif
  fut = future_new(loop);
  if (fut == nullptr) {
    return nullptr;
  }
#if 0
  } else {
    // for subclasses of _AsyncLazyValue_Type - create dedicated future type
    _ALVResultFutureObj* f = (_ALVResultFutureObj*)PyType_GenericNew(
        get_state()->alvResultFutureObj(), nullptr, nullptr);
    if (f == nullptr) {
      return nullptr;
    }
    f->alvrf_blocked_by_this = nullptr;
    f->alvrf_cycle_boundary = 0;
    fut = (FutureObj*)f;
  }
#endif
  if (self->alv_futures == nullptr) {
    self->alv_futures = PyList_New(0);
    if (self->alv_futures == nullptr) {
      Py_DECREF(fut);
      return nullptr;
    }
  }
  if (PyList_Append(self->alv_futures, (PyObject*)fut) < 0) {
    Py_DECREF(fut);
    return nullptr;
  }
  return (PyObject*)fut;
}

static PyObject* AsyncLazyValue_await(AsyncLazyValueObj* self) {
  switch (self->alv_state) {
    case ALV_NOT_STARTED: {
      PyObject* compute = AsyncLazyValue_new_computeobj(self);
      if (compute == nullptr) {
        return nullptr;
      }
      self->alv_state = ALV_RUNNING;
      return compute;
    }
    case ALV_RUNNING: {
      PyObject* fut = AsyncLazyValue_new_future(self, Py_None);
      if (fut == nullptr) {
        return nullptr;
      }
      PyObject* res = Py_TYPE(fut)->tp_iter(fut);
      Py_DECREF(fut);
      return res;
    }
    case ALV_DONE: {
      Py_INCREF(self);
      return (PyObject*)self;
    }
    default:
      Py_UNREACHABLE();
  }
}

static PyObject* create_task(PyObject* coro, PyObject* loop) {
  PyObject* create_task = nullptr;
  DEFINE_STATIC_STRING(create_task);
  int meth_found = _PyObject_GetMethod(loop, s_create_task, &create_task);
  if (create_task == nullptr) {
    return nullptr;
  }

  PyObject* task;
  if (meth_found) {
    PyObject* stack[2] = {loop, coro};
    task = PyObject_Vectorcall(create_task, stack, 2, nullptr);
  } else {
    PyObject* stack[1] = {coro};
    task = PyObject_Vectorcall(create_task, stack, 1, nullptr);
  }
  Py_DECREF(create_task);
  if (task == nullptr) {
    return nullptr;
  }
  Ref<PyMethodTableRef> t = cinderx::get_or_create_method_table(Py_TYPE(task));
  if (t == nullptr) {
    Py_DECREF(task);
    return nullptr;
  }
  if (get_state()->lastUsedTaskType() != Py_TYPE(task)) {
    // keep track of last used task type
    // applications typically don't use multiple task type
    // so we can avoid lookups in case if the same type is used all the time
    get_state()->setLastUsedTaskType(Py_TYPE(task));
    get_state()->setLastUsedTaskTypeTableRef(t);
  }
  PyObject* tb = t->source_traceback(task);
  if (tb == nullptr) {
    Py_DECREF(task);
    return nullptr;
  }
  int ok = 0;
  static PyObject* minus_one;
  if (minus_one == nullptr) {
    minus_one = PyLong_FromLong(-1);
  }
  if (tb != Py_None) {
    ok = PyObject_DelItem(tb, minus_one);
  }
  Py_DECREF(tb);
  if (ok < 0) {
    Py_CLEAR(task);
  }
  return task;
}

static PyObject* AsyncLazyValue_new_task(
    AsyncLazyValueObj* self,
    PyObject* loop) {
  assert(loop != Py_None);

  PyObject* computeobj = AsyncLazyValue_new_computeobj(self);
  if (computeobj == nullptr) {
    return nullptr;
  }
  PyObject* task = create_task(computeobj, loop);
  Py_DECREF(computeobj);
  return task;
}

static PyObject* AsyncLazyValue_ensure_future(
    AsyncLazyValueObj* self,
    PyObject* loop) {
  switch (self->alv_state) {
    case ALV_DONE: {
      PyObject* fut = future_new(loop);
      if (fut == nullptr) {
        return nullptr;
      }
      int retval = future_set_result((FutureObj*)fut, self->alv_result);
      if (retval == -1) {
        Py_DECREF(fut);
        return nullptr;
      }
      return (PyObject*)fut;
    }
    case ALV_RUNNING: {
      return AsyncLazyValue_new_future(self, loop);
    }
    case ALV_NOT_STARTED: {
      int release_loop = 0;
      if (loop == Py_None) {
        loop = get_event_loop();
        if (loop == nullptr) {
          return nullptr;
        }
        release_loop = 1;
      }
      PyObject* result = AsyncLazyValue_new_task(self, loop);
      if (release_loop) {
        Py_DECREF(loop);
      }
      if (result) {
        self->alv_state = ALV_RUNNING;
      }
      return result;
    }
    default:
      Py_UNREACHABLE();
  }
}

static PyObject* AsyncLazyValue_link(
    AsyncLazyValueObj* self,
    PyObject* Py_UNUSED(arg)) {
#if 0
  if (cinder_get_arg0_from_pyframe != nullptr) {
    PyObject* parent = call_get_arg0_from_pyframe(
        asyncio_alv_metadata_entrypoint_name, /*to_skip*/ one);
    if (parent == nullptr) {
      return nullptr;
    }
    if (parent != Py_None) {
      self->alv_parent = parent;
    } else {
      Py_DECREF(parent);
    }
  }
#endif
  Py_RETURN_NONE;
}

static PyObject* AsyncLazyValue_unlink(
    AsyncLazyValueObj* self,
    PyObject* Py_UNUSED(arg)) {
  Py_CLEAR(self->alv_parent);
  Py_RETURN_NONE;
}

static int
AsyncLazyValue_traverse(AsyncLazyValueObj* self, visitproc visit, void* arg) {
  Py_VISIT(self->alv_args);
  Py_VISIT(self->alv_kwargs);
  Py_VISIT(self->alv_futures);
  Py_VISIT(self->alv_result);
  Py_VISIT(self->alv_parent);
  return 0;
}

static int AsyncLazyValue_clear(AsyncLazyValueObj* self) {
  Py_CLEAR(self->alv_args);
  Py_CLEAR(self->alv_kwargs);
  Py_CLEAR(self->alv_futures);
  Py_CLEAR(self->alv_result);
  Py_CLEAR(self->alv_parent);
  return 0;
}

static void AsyncLazyValue_dealloc(AsyncLazyValueObj* self) {
  AsyncLazyValue_clear(self);

  PyObject_GC_UnTrack(self);
  Py_DECREF(((PyObject*)self)->ob_type);
  Py_TYPE(self)->tp_free(self);
}

static PySendResult AsyncLazyValue_itersend(
    AsyncLazyValueObj* self,
    PyObject* Py_UNUSED(sentValue),
    PyObject** pResult) {
  switch (self->alv_state) {
    case ALV_NOT_STARTED:
    case ALV_RUNNING: {
      PyErr_SetString(PyExc_TypeError, "AsyncLazyValue needs to be awaited");
      return PYGEN_ERROR;
    }
    case ALV_DONE: {
      Py_INCREF(self->alv_result);
      *pResult = self->alv_result;
      return PYGEN_RETURN;
    }
    default:
      Py_UNREACHABLE();
  }
}

static inline PyObject* gen_status_to_iter(
    PySendResult gen_status,
    PyObject* result) {
  if (gen_status == PYGEN_ERROR || gen_status == PYGEN_NEXT) {
    return result;
  }
  assert(gen_status == PYGEN_RETURN);
  _PyGen_SetStopIterationValue(result);
  Py_DECREF(result);
  return nullptr;
}

static PyObject* AsyncLazyValue_iternext(AsyncLazyValueObj* self) {
  PyObject* result;
  PySendResult gen_status = AsyncLazyValue_itersend(self, nullptr, &result);
  return gen_status_to_iter(gen_status, result);
}

static PyObject* AsyncLazyValue_get_awaiting_tasks(
    AsyncLazyValueObj* self,
    PyObject* Py_UNUSED(ignored)) {
  Py_ssize_t n =
      self->alv_futures != nullptr ? PyList_GET_SIZE(self->alv_futures) : 0;
  return PyLong_FromSsize_t(n);
}

static PyObject* AsyncLazyValue_get_state(
    AsyncLazyValueObj* self,
    PyObject* Py_UNUSED(ignored)) {
  DEFINE_STATIC_STRING(not_started);
  DEFINE_STATIC_STRING(running);
  DEFINE_STATIC_STRING(done);
  switch (self->alv_state) {
    case ALV_NOT_STARTED:
      return s_not_started;
    case ALV_RUNNING:
      return s_running;
    case ALV_DONE:
      return s_done;
  }

  PyErr_SetString(PyExc_RuntimeError, "unknown AsyncLazyValue state");
  return nullptr;
}

static PyMethodDef AsyncLazyValue_methods[] = {
    {"ensure_future",
     (PyCFunction)AsyncLazyValue_ensure_future,
     METH_O,
     nullptr},
    {"_link", (PyCFunction)AsyncLazyValue_link, METH_NOARGS, nullptr},
    {"_unlink", (PyCFunction)AsyncLazyValue_unlink, METH_NOARGS, nullptr},
    {nullptr, nullptr} /* Sentinel */
};

static PyGetSetDef AsyncLazyValue_getsetlist[] = {
    {"_awaiting_tasks",
     (getter)AsyncLazyValue_get_awaiting_tasks,
     nullptr,
     nullptr},
    {"alv_state", (getter)AsyncLazyValue_get_state, nullptr, nullptr},
    {nullptr} /* Sentinel */
};

PyType_Slot asynclazyvalue_slots[] = {
    {Py_tp_traverse, reinterpret_cast<void*>(AsyncLazyValue_traverse)},
    {Py_tp_clear, reinterpret_cast<void*>(AsyncLazyValue_clear)},
    {Py_tp_iter, reinterpret_cast<void*>(PyObject_SelfIter)},
    {Py_tp_iternext, reinterpret_cast<void*>(AsyncLazyValue_iternext)},
    {Py_tp_methods, AsyncLazyValue_methods},
    {Py_tp_getset, AsyncLazyValue_getsetlist},
    {Py_am_send, reinterpret_cast<void*>(AsyncLazyValue_itersend)},
    {Py_am_await, reinterpret_cast<void*>(AsyncLazyValue_await)},
    {Py_tp_init, reinterpret_cast<void*>(AsyncLazyValue_init)},
    {Py_tp_new, reinterpret_cast<void*>(PyType_GenericNew)},
    {Py_tp_dealloc, reinterpret_cast<void*>(AsyncLazyValue_dealloc)},
    {0, nullptr} /* Sentinel */
};

PyType_Spec _AsyncLazyValue_Spec = {
    .name = "cinderx.AsyncLazyValue",
    .basicsize = sizeof(AsyncLazyValueObj),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_BASETYPE |
        Py_TPFLAGS_IMMUTABLETYPE,
    .slots = asynclazyvalue_slots,
};

/*[clinic input]
class _asyncio.AsyncLazyValueCompute "AsyncLazyValueComputeObj *"
"&_AsyncLazyValueCompute_Type" [clinic start generated code]*/
/*[clinic end generated code: output=da39a3ee5e6b4b0d input=a22f5c3d11e2aa0f]*/

static int AsyncLazyValueCompute_traverse(
    AsyncLazyValueComputeObj* self,
    visitproc visit,
    void* arg) {
  Py_VISIT(self->alvc_target);
  Py_VISIT(self->alvc_coroobj);
  Py_VISIT(self->alvc_exc_state.exc_value);
  return 0;
}

static int AsyncLazyValueCompute_clear(AsyncLazyValueComputeObj* self) {
  Py_CLEAR(self->alvc_target);
  Py_CLEAR(self->alvc_coroobj);
  Py_CLEAR(self->alvc_exc_state.exc_value);
  // awaiter is borrowed
  self->alvc_pending_awaiter = nullptr;
  return 0;
}

static void AsyncLazyValueCompute_dealloc(AsyncLazyValueComputeObj* self) {
  AsyncLazyValueCompute_clear(self);

  PyObject_GC_UnTrack(self);
  Py_DECREF(((PyObject*)self)->ob_type);
  Py_TYPE(self)->tp_free(self);
}

static void forward_and_clear_pending_awaiter(AsyncLazyValueComputeObj* self) {
  assert(self->alvc_coroobj != nullptr);
#if ENABLE_GENERATOR_AWAITER
  if (self->alvc_pending_awaiter == nullptr) {
    return;
  }
  Ci_PyAwaitable_SetAwaiter(self->alvc_coroobj, self->alvc_pending_awaiter);
  self->alvc_pending_awaiter = nullptr;
#endif
}

/**
    Runs a function that was provided to AsyncLazyValue.
    - if function was not a coroutine - calls _PyCoro_GetAwaitableIter on a
   result and stores it for subsequent 'send' calls
    - if function was a coroutine and it was completed eagerly -
      sets 'did_step' indicator and returns the result
    - if function was a coroutine but it was not completed eagerly
      sets 'did_step' indicator, stores coroutine object for subsequent 'send'
   calls and return result of the step (typically it is future)
 */
static PyObject* AsyncLazyValueCompute_create_and_set_subcoro(
    AsyncLazyValueComputeObj* self,
    int* did_step) {
  Py_ssize_t nargs = PyTuple_GET_SIZE(self->alvc_target->alv_args);
  PyObject** args = &PyTuple_GET_ITEM(self->alvc_target->alv_args, 0);
  PyObject* kwargs = self->alvc_target->alv_kwargs;
  PyObject* result =
      PyObject_VectorcallDict(args[0], args + 1, (nargs - 1), kwargs);
  if (result == nullptr) {
    return nullptr;
  }

  // function being called is not a coroutine
  PyObject* iter = JitCoro_GetAwaitableIter(result);
  Py_DECREF(result);
  if (iter == nullptr) {
    return nullptr;
  }
  self->alvc_coroobj = iter;
  forward_and_clear_pending_awaiter(self);

  Py_RETURN_NONE;
}

/**
    Handle error being raised, effectively working as a catch block
    try: ...
    except Exception as e:
        notify-futures
        if isinstance(e, (GeneratorExit, StopIteration)):
            pass
        else:
            raise

 */
static PyObject* AsyncLazyValueCompute_handle_error(
    AsyncLazyValueComputeObj* self,
    PyThreadState* tstate,
    int reraise,
    int closing) {
  assert(PyErr_Occurred());

  PyObject *et, *ev, *tb;
  PyErr_Fetch(&et, &ev, &tb);
  PyErr_NormalizeException(&et, &ev, &tb);
  if (tb != nullptr) {
    PyException_SetTraceback(ev, tb);
  } else {
    PyException_SetTraceback(ev, Py_None);
  }

  // push information about current exception
  _PyErr_StackItem* previous_exc_info = tstate->exc_info;
  _PyErr_StackItem exc_info = {
      .exc_value = ev, .previous_item = previous_exc_info};
  tstate->exc_info = &exc_info;
  // set the error in async lazy value
  int err = AsyncLazyValue_set_error(self->alvc_target, ev);
  // pop current exception info
  tstate->exc_info = previous_exc_info;

  // 1. if exception was raised when setting the result - it will
  // shadow original exception so we can release it.
  // 2. also release it if we are not supposed to re-raise it
  if (err < 0 || !reraise) {
    Py_DECREF(et);
    Py_XDECREF(ev);
    Py_XDECREF(tb);
  }
  if (err < 0) {
    // for closing case ignore StopIteration and GeneratorExit
    if (closing &&
        (PyErr_ExceptionMatches(PyExc_StopIteration) ||
         PyErr_ExceptionMatches(PyExc_GeneratorExit))) {
      PyErr_Clear();
      Py_RETURN_NONE;
    }
    return nullptr;
  }
  if (reraise) {
    // reraise previous error
    PyErr_Restore(et, ev, tb);
    return nullptr;
  }
  Py_RETURN_NONE;
}

static void AsyncLazyValueCompute_set_awaiter(
    AsyncLazyValueComputeObj* self,
    PyObject* awaiter) {
#if ENABLE_GENERATOR_AWAITER
  if (self->alvc_coroobj != nullptr) {
    Ci_PyAwaitable_SetAwaiter(self->alvc_coroobj, awaiter);
  } else {
    self->alvc_pending_awaiter = awaiter;
  }
#endif
}

/**
  Implementation of a 'send' for AsyncLazyValueCompute
  */
static PyObject* AsyncLazyValueCompute_itersend_(
    AsyncLazyValueComputeObj* self,
    PyThreadState* tstate,
    PyObject* sentValue,
    int* pReturn) {
  if (self->alvc_coroobj == nullptr) {
    int did_step = 0;
    // here alvc_coroobj coroutine object was not created yet -
    // call coroutine and set coroutine object for subsequent sends
    PyObject* retval =
        AsyncLazyValueCompute_create_and_set_subcoro(self, &did_step);
    if (retval == nullptr) {
      // failed - handle error
      return AsyncLazyValueCompute_handle_error(self, tstate, 1, 0);
    }
    if (did_step) {
      // if we did step when calling coroutine - we attempted to run
      // coroutine eagerly which might have two outcomes
      if (self->alvc_coroobj == nullptr) {
        // 1. coroutine has finished eagerly
        // set the successful result to owning AsyncLazyValue
        int ok = AsyncLazyValue_set_result(self->alvc_target, retval);
        if (ok < 0) {
          Py_DECREF(retval);
          return AsyncLazyValueCompute_handle_error(self, tstate, 1, 0);
        }
        // ..and set return indicator
        *pReturn = 1;
      }
      // 2. coroutine was not finished eagerly but we did some work
      // return without setting return indicator - meaning we yielded
      return retval;
    }
    Py_DECREF(retval);
  }

  assert(self->alvc_coroobj != nullptr);

  PyObject* res;
  PySendResult gen_status = PyIter_Send(self->alvc_coroobj, sentValue, &res);

  if (gen_status == PYGEN_RETURN) {
    // RETURN
    int ok = AsyncLazyValue_set_result(self->alvc_target, res);
    if (ok < 0) {
      Py_DECREF(res);
      return nullptr;
    }
    *pReturn = 1;
    return res;
  }
  if (gen_status == PYGEN_NEXT) {
    // YIELD
    return res;
  }
  assert(gen_status == PYGEN_ERROR);
  // ERROR
  return AsyncLazyValueCompute_handle_error(self, tstate, 1, 0);
}

/**
  Entrypoint for gennext used by Py_TPFLAGS_HAVE_AM_SEND supported types
  */
static PySendResult AsyncLazyValueCompute_itersend(
    AsyncLazyValueComputeObj* self,
    PyObject* sentValue,
    PyObject** pResult) {
  PyThreadState* tstate = PyThreadState_GET();
  _PyErr_StackItem* previous_exc_info = tstate->exc_info;
  self->alvc_exc_state.previous_item = previous_exc_info;
  tstate->exc_info = &self->alvc_exc_state;

  int is_return = 0;
  PyObject* result =
      AsyncLazyValueCompute_itersend_(self, tstate, sentValue, &is_return);

  *pResult = result;

  tstate->exc_info = previous_exc_info;
  if (result == nullptr) {
    AsyncLazyValueCompute_clear(self);
    return PYGEN_ERROR;
  }
  if (is_return) {
    assert(result);
    AsyncLazyValueCompute_clear(self);
    return PYGEN_RETURN;
  }
  return PYGEN_NEXT;
}

static PyObject* AsyncLazyValueCompute_send(
    AsyncLazyValueComputeObj* self,
    PyObject* val) {
  PyObject* res;
  PySendResult gen_status = AsyncLazyValueCompute_itersend(self, val, &res);
  return gen_status_to_iter(gen_status, res);
}

static PyObject* AsyncLazyValueCompute_next(AsyncLazyValueComputeObj* self) {
  return AsyncLazyValueCompute_send(self, Py_None);
}

static int gen_close_iter(PyObject* yf) {
  PyObject* retval = nullptr;

  PyObject* meth;
  DEFINE_STATIC_STRING(close);
  if (PyObject_GetOptionalAttr(yf, s_close, &meth) < 0) {
    PyErr_WriteUnraisable(yf);
  }
  if (meth) {
    retval = PyObject_CallNoArgs(meth);
    Py_DECREF(meth);
    if (retval == nullptr) {
      return -1;
    }
  }
  Py_XDECREF(retval);
  return 0;
}

PyObject* coro_throw(PyObject* coro, PyObject* type) {
  PyObject* retval = nullptr;

  PyObject* meth;
  DEFINE_STATIC_STRING(throw);
  if (PyObject_GetOptionalAttr(coro, s_throw, &meth) < 0) {
    PyErr_WriteUnraisable(coro);
  }
  if (meth) {
    PyObject* args[] = {type};
    retval = PyObject_Vectorcall(meth, args, 1, nullptr);
    Py_DECREF(meth);
  }
  return retval;
}

static PyObject* AsyncLazyValueCompute_close_(AsyncLazyValueComputeObj* self) {
  if (self->alvc_coroobj == nullptr) {
    // coroutine is not started - just return
    Py_RETURN_NONE;
  }
  // close subgenerator
  int err = gen_close_iter(self->alvc_coroobj);
  Py_CLEAR(self->alvc_coroobj);

  if (err == 0) {
    PyErr_SetNone(PyExc_GeneratorExit);
  }

  // run the error handler with either error from subgenerator
  // or PyExc_GeneratorExit
  return AsyncLazyValueCompute_handle_error(
      self, PyThreadState_GET(), err < 0, 1);
}

static PyObject* AsyncLazyValueCompute_close(
    AsyncLazyValueComputeObj* self,
    PyObject* Py_UNUSED(arg)) {
  PyObject* res = AsyncLazyValueCompute_close_(self);
  if (res == nullptr) {
    AsyncLazyValueCompute_clear(self);
  }
  return res;
}

static int _gen_restore_error(PyObject* typ, PyObject* val, PyObject* tb) {
  /* First, check the traceback argument, replacing None with
     nullptr. */
  if (tb == Py_None) {
    tb = nullptr;
  } else if (tb != nullptr && !PyTraceBack_Check(tb)) {
    PyErr_SetString(
        PyExc_TypeError, "throw() third argument must be a traceback object");
    return -1;
  }

  Py_INCREF(typ);
  Py_XINCREF(val);
  Py_XINCREF(tb);

  if (PyExceptionClass_Check(typ)) {
    PyErr_NormalizeException(&typ, &val, &tb);

  } else if (PyExceptionInstance_Check(typ)) {
    /* Raising an instance.  The value should be a dummy. */
    if (val && val != Py_None) {
      PyErr_SetString(
          PyExc_TypeError, "instance exception may not have a separate value");
      goto failed_throw;
    } else {
      /* Normalize to raise <class>, <instance> */
      Py_XDECREF(val);
      val = typ;
      typ = PyExceptionInstance_Class(typ);
      Py_INCREF(typ);

      if (tb == nullptr) {
        /* Returns nullptr if there's no traceback */
        tb = PyException_GetTraceback(val);
      }
    }
  } else {
    /* Not something you can raise.  throw() fails. */
    PyErr_Format(
        PyExc_TypeError,
        "exceptions must be classes or instances "
        "deriving from BaseException, not %s",
        Py_TYPE(typ)->tp_name);
    goto failed_throw;
  }

  PyErr_Restore(typ, val, tb);
  return 0;

failed_throw:
  /* Didn't use our arguments, so restore their original refcounts */
  Py_DECREF(typ);
  Py_XDECREF(val);
  Py_XDECREF(tb);
  return -1;
}

static PyObject* AsyncLazyValueCompute_throw(
    AsyncLazyValueComputeObj* self,
    PyObject* type,
    PyObject* val,
    PyObject* tb) {
  if (self->alvc_coroobj == nullptr) {
    _gen_restore_error(type, val, tb);
    return nullptr;
  }

  if (PyErr_GivenExceptionMatches(type, PyExc_GeneratorExit)) {
    int err = gen_close_iter(self->alvc_coroobj);
    Py_CLEAR(self->alvc_coroobj);
    if (err < 0) {
      return AsyncLazyValueCompute_handle_error(
          self, PyThreadState_GET(), 1, 0);
    }
    if (_gen_restore_error(type, val, tb) == -1) {
      return nullptr;
    }
    return AsyncLazyValueCompute_handle_error(self, PyThreadState_GET(), 1, 0);
  }

  PyObject* res = coro_throw(self->alvc_coroobj, type);
  if (res != nullptr) {
    return res;
  }
  if (_PyGen_FetchStopIterationValue(&res) == 0) {
    int ok = AsyncLazyValue_set_result(self->alvc_target, res);
    AsyncLazyValueCompute_clear(self);
    if (ok < 0) {
      Py_DECREF(res);
      return nullptr;
    }
    _PyGen_SetStopIterationValue(res);
    Py_DECREF(res);
    return nullptr;
  } else {
    return AsyncLazyValueCompute_handle_error(self, PyThreadState_GET(), 1, 0);
  }
}

static PyObject* _asyncio_AsyncLazyValueCompute_throw_impl(
    AsyncLazyValueComputeObj* self,
    PyObject* type,
    PyObject* val,
    PyObject* tb) {
  PyObject* res = AsyncLazyValueCompute_throw(self, type, val, tb);
  if (res == nullptr) {
    AsyncLazyValueCompute_clear(self);
  }
  return res;
}

static PyObject* _asyncio_AsyncLazyValueCompute_throw(
    AsyncLazyValueComputeObj* self,
    PyObject* const* args,
    Py_ssize_t nargs) {
  PyObject* return_value = nullptr;
  PyObject* type;
  PyObject* val = nullptr;
  PyObject* tb = nullptr;

  if (!_PyArg_CheckPositional("throw", nargs, 1, 3)) {
    goto exit;
  }
  type = args[0];
  if (nargs < 2) {
    goto skip_optional;
  }
  val = args[1];
  if (nargs < 3) {
    goto skip_optional;
  }
  tb = args[2];
skip_optional:
  return_value = _asyncio_AsyncLazyValueCompute_throw_impl(self, type, val, tb);

exit:
  return return_value;
}

static PyMethodDef AsyncLazyValueCompute_methods[] = {
    {"send", (PyCFunction)AsyncLazyValueCompute_send, METH_O, nullptr},
    {"throw",
     (PyCFunction)(void (*)(void))_asyncio_AsyncLazyValueCompute_throw,
     METH_FASTCALL,
     "throw($self, type, val=<unrepresentable>, tb=<unrepresentable>, /)\n"},
    {"close", (PyCFunction)AsyncLazyValueCompute_close, METH_NOARGS, nullptr},
    {nullptr, nullptr} /* Sentinel */
};

#ifdef ENABLE_GENERATOR_AWAITER

Ci_AsyncMethodsWithExtra _async_lazy_value_compute_type_as_async = {
    .ame_async_methods =
        {
            (unaryfunc)PyObject_SelfIter, /* am_await */
            nullptr, /* am_aiter */
            nullptr, /* am_anext */
            (sendfunc)AsyncLazyValueCompute_itersend, /* am_send */
        },
    .ame_setawaiter = (setawaiterfunc)AsyncLazyValueCompute_set_awaiter,
};

#else

PyAsyncMethods _async_lazy_value_compute_type_as_async = {
    .am_await = (unaryfunc)PyObject_SelfIter,
    .am_aiter = nullptr,
    .am_anext = nullptr,
    .am_send = (sendfunc)AsyncLazyValueCompute_itersend,
};

#endif

PyType_Slot _AsyncLazyValueCompute_slots[] = {
    {Py_tp_traverse, reinterpret_cast<void*>(AsyncLazyValueCompute_traverse)},
    {Py_tp_clear, reinterpret_cast<void*>(AsyncLazyValueCompute_clear)},
    {Py_tp_methods, reinterpret_cast<void*>(AsyncLazyValueCompute_methods)},
    {Py_am_await, reinterpret_cast<void*>(PyObject_SelfIter)},
    {Py_am_send, reinterpret_cast<void*>(AsyncLazyValueCompute_itersend)},
    {Py_tp_iternext, reinterpret_cast<void*>(AsyncLazyValueCompute_next)},
    {Py_tp_iter, reinterpret_cast<void*>(PyObject_SelfIter)},
    {Py_tp_dealloc, reinterpret_cast<void*>(AsyncLazyValueCompute_dealloc)},
    {0, nullptr},
};

PyType_Spec _AsyncLazyValueCompute_Spec = {
    .name = "cinders.AsyncLazyValueCompute",
    .basicsize = sizeof(AsyncLazyValueComputeObj),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Ci_TPFLAGS_HAVE_AM_EXTRA | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = _AsyncLazyValueCompute_slots,
};

/*********************** AwaitableValue *********************/

static int _asyncio_AwaitableValue___init__(
    PyObject* self,
    PyObject* args,
    PyObject* kwargs) {
  PyObject* value;

  if (Py_IS_TYPE(self, get_state()->awaitableValueType()) &&
      !_PyArg_NoKeywords("AwaitableValue", kwargs)) {
    return -1;
  }
  if (!_PyArg_CheckPositional("AwaitableValue", PyTuple_GET_SIZE(args), 1, 1)) {
    return -1;
  }
  value = PyTuple_GET_ITEM(args, 0);

  Py_INCREF(value);
  Py_XDECREF(((AwaitableValueObj*)self)->av_value);
  ((AwaitableValueObj*)self)->av_value = value;

  return 0;
}

static int
AwaitableValueObj_traverse(AwaitableValueObj* obj, visitproc visit, void* arg) {
  Py_VISIT(obj->av_value);
  return 0;
}

static int AwaitableValueObj_clear(AwaitableValueObj* self) {
  Py_CLEAR(self->av_value);
  return 0;
}

static PySendResult AwaitableValueObj_itersend(
    AwaitableValueObj* self,
    PyObject* Py_UNUSED(sentValue),
    PyObject** pResult);

static PyObject* AwaitableValueObj_next(AwaitableValueObj* self) {
  _PyGen_SetStopIterationValue(self->av_value);
  return nullptr;
}

static void AwaitableValueObj_dealloc(AwaitableValueObj* self) {
  AwaitableValueObj_clear(self);

  PyObject_GC_UnTrack(self);
  Py_DECREF(((PyObject*)self)->ob_type);
  Py_TYPE(self)->tp_free(self);
}

static PySendResult AwaitableValueObj_itersend(
    AwaitableValueObj* self,
    PyObject* Py_UNUSED(sentValue),
    PyObject** pResult) {
  Py_INCREF(self->av_value);
  *pResult = self->av_value;
  return PYGEN_RETURN;
}

static PyObject* AwaitableValueObj_get_value(
    AwaitableValueObj* self,
    void* Py_UNUSED(ignored)) {
  Py_INCREF(self->av_value);
  return self->av_value;
}

static PyGetSetDef awaitable_value_type__getsetlist[] = {
    {"value", (getter)AwaitableValueObj_get_value, nullptr, nullptr},
    {nullptr} /* Sentinel */
};

PyType_Slot awaitabletype_slots[] = {
    {Py_tp_new, reinterpret_cast<void*>(PyType_GenericNew)},
    {Py_tp_traverse, reinterpret_cast<void*>(AwaitableValueObj_traverse)},
    {Py_tp_clear, reinterpret_cast<void*>(AwaitableValueObj_clear)},
    {Py_am_await, reinterpret_cast<void*>(PyObject_SelfIter)},
    {Py_am_send, reinterpret_cast<void*>(AwaitableValueObj_itersend)},
    {Py_tp_getset, reinterpret_cast<void*>(awaitable_value_type__getsetlist)},
    {Py_tp_iternext, reinterpret_cast<void*>(AwaitableValueObj_next)},
    {Py_tp_iter, reinterpret_cast<void*>(PyObject_SelfIter)},
    {Py_tp_dealloc, reinterpret_cast<void*>(AwaitableValueObj_dealloc)},
    {Py_tp_init, reinterpret_cast<void*>(_asyncio_AwaitableValue___init__)},
    {0, nullptr},
};

PyType_Spec AwaitableValue_Spec = {
    .name = "cinderx.AwaitableValue",
    .basicsize = sizeof(AwaitableValueObj),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = awaitabletype_slots,
};
}

#endif // PY_VERSION_HEX >= 0x030C0000
