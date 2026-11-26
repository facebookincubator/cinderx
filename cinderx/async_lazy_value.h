// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"
#include "cinderx/async_lazy_value_iface.h"

#include <utility>

/*
  Unboxed result of calling _asyncio_future_blocking
  Normally we'd use int with commit convention to indicate:
    true (>0), false (0), error(<0)
  However it is permitted for this property to return None
  we need to represent 4 states so enum fits quite nicely
*/
enum fut_blocking_state {
  BLOCKING_TRUE,
  BLOCKING_FALSE,
  BLOCKING_ERROR,
  BLOCKING_NONE,
};

using get_source_traceback = PyObject* (*)(PyObject * fut);

struct PyMethodTableRef {
  PyWeakReference weakref;
  // actual method table
  get_source_traceback source_traceback;
};

namespace cinderx {

class AsyncLazyValueState : public IAsyncLazyValueState {
 public:
  AsyncLazyValueState() {}
  ~AsyncLazyValueState() override {}

  bool init() override;

  BorrowedRef<PyTypeObject> asyncLazyValueType() override {
    return async_lazy_value_type_;
  }

  BorrowedRef<PyTypeObject> asyncLazyValueComputeType() {
    return async_lazy_value_compute_type_;
  }

  BorrowedRef<PyTypeObject> awaitableValueType() {
    return awaitable_value_type_;
  }

  BorrowedRef<PyTypeObject> futureType();

  BorrowedRef<PyTypeObject> methodTableRefType() {
    return _method_table_ref_type_;
  }

  BorrowedRef<> getEventLoop();

  PyMethodTableRef* lastUsedTaskTypeTableRef() {
    return last_used_task_type_table_ref_;
  }

  BorrowedRef<PyTypeObject> lastUsedTaskType() {
    return last_used_task_type_;
  }

  void setLastUsedTaskType(
      BorrowedRef<PyTypeObject> type,
      BorrowedRef<PyMethodTableRef> tableref) {
    // The old entries are released only after the new ones are in place, the
    // table before its type, so code run by a release never sees a stale
    // entry and a dying type's callback finds its table already gone.
    Ref<PyTypeObject> old_type = std::move(last_used_task_type_);
    Ref<PyMethodTableRef> old_tableref =
        std::move(last_used_task_type_table_ref_);
    last_used_task_type_ = Ref<PyTypeObject>::create(type);
    last_used_task_type_table_ref_ = Ref<PyMethodTableRef>::create(tableref);
  }

  BorrowedRef<PyObject> methodRefCallback() {
    return methodref_callback_;
  }

  BorrowedRef<> asyncioFutureBlocking();
  BorrowedRef<PyTypeObject> cancelledError();

  BorrowedRef<PyGetSetDescrObject> futureSourceTraceback();

  Ref<PyGetSetDescrObject> lookupFutureGetSet(const char* name);

  BorrowedRef<PyMethodTableRef> futureTableRef();

 private:
  Ref<PyTypeObject> async_lazy_value_type_;
  Ref<PyTypeObject> async_lazy_value_compute_type_;
  Ref<PyTypeObject> awaitable_value_type_;
  Ref<PyTypeObject> future_type_;
  Ref<PyTypeObject> _method_table_ref_type_;
  Ref<> asyncio_future_blocking_;
  Ref<> get_event_loop_;
  Ref<PyTypeObject> cancelled_error_;
  Ref<> methodref_callback_;
  Ref<PyGetSetDescrObject> future_source_traceback_;
  Ref<PyMethodTableRef> future_table_ref_;

  // The last task type that was created in create_task, and its method table.
  // Both are owned so that neither can be freed, nor the type's address
  // reused, while they are cached. Not used on free-threaded builds, where the
  // cache is not synchronized.
  Ref<PyTypeObject> last_used_task_type_;
  Ref<PyMethodTableRef> last_used_task_type_table_ref_;
};

} // namespace cinderx
