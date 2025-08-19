// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/ref.h"

namespace cinderx {

// Watcher callback types.
using CodeWatcher = int (*)(PyCodeEvent, PyCodeObject*);
using DictWatcher = int (*)(PyDict_WatchEvent, PyObject*, PyObject*, PyObject*);
using FuncWatcher =
    int (*)(PyFunction_WatchEvent, PyFunctionObject*, PyObject*);
using TypeWatcher = int (*)(PyTypeObject*);

class WatcherState {
 public:
  WatcherState();
  ~WatcherState();

  // Register and enable all watchers.  Can fail with a Python error.
  int init();

  // Disable all watchers.  Can fail with a Python error.
  int fini();

  // Set individual watchers.
  void setCodeWatcher(CodeWatcher watcher);
  void setDictWatcher(DictWatcher watcher);
  void setFuncWatcher(FuncWatcher watcher);
  void setTypeWatcher(TypeWatcher watcher);

  // Watch or unwatch a dictionary.
  int watchDict(BorrowedRef<PyDictObject> dict);
  int unwatchDict(BorrowedRef<PyDictObject> dict);

  // Watch or unwatch a type.
  int watchType(BorrowedRef<PyTypeObject> type);
  int unwatchType(BorrowedRef<PyTypeObject> type);

 private:
  CodeWatcher code_watcher_{nullptr};
  DictWatcher dict_watcher_{nullptr};
  FuncWatcher func_watcher_{nullptr};
  TypeWatcher type_watcher_{nullptr};

  int code_watcher_id_{-1};
  int dict_watcher_id_{-1};
  int func_watcher_id_{-1};
  int type_watcher_id_{-1};
};

} // namespace cinderx
