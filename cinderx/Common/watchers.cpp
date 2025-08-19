// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/watchers.h"

namespace cinderx {

WatcherState::WatcherState() = default;

WatcherState::~WatcherState() {
  fini();
}

int WatcherState::init() {
  if (code_watcher_ != nullptr) {
    if ((code_watcher_id_ = PyCode_AddWatcher(code_watcher_)) < 0) {
      return -1;
    }
  }
  if (dict_watcher_ != nullptr) {
    if ((dict_watcher_id_ = PyDict_AddWatcher(dict_watcher_)) < 0) {
      return -1;
    }
  }
  if (func_watcher_ != nullptr) {
    if ((func_watcher_id_ = PyFunction_AddWatcher(func_watcher_)) < 0) {
      return -1;
    }
  }
  if (type_watcher_ != nullptr) {
    if ((type_watcher_id_ = PyType_AddWatcher(type_watcher_)) < 0) {
      return -1;
    }
  }
  return 0;
}

int WatcherState::fini() {
  if (type_watcher_id_ != -1 && PyType_ClearWatcher(type_watcher_id_) < 0) {
    return -1;
  }
  type_watcher_id_ = -1;

  if (func_watcher_id_ != -1 && PyFunction_ClearWatcher(func_watcher_id_) < 0) {
    return -1;
  }
  func_watcher_id_ = -1;

  if (dict_watcher_id_ != -1 && PyDict_ClearWatcher(dict_watcher_id_) < 0) {
    return -1;
  }
  dict_watcher_id_ = -1;

  if (code_watcher_id_ != -1 && PyCode_ClearWatcher(code_watcher_id_) < 0) {
    return -1;
  }
  code_watcher_id_ = -1;

  return 0;
}

void WatcherState::setCodeWatcher(CodeWatcher watcher) {
  code_watcher_ = watcher;
}

void WatcherState::setDictWatcher(DictWatcher watcher) {
  dict_watcher_ = watcher;
}

void WatcherState::setFuncWatcher(FuncWatcher watcher) {
  func_watcher_ = watcher;
}

void WatcherState::setTypeWatcher(TypeWatcher watcher) {
  type_watcher_ = watcher;
}

int WatcherState::watchDict(BorrowedRef<PyDictObject> dict) {
  return PyDict_Watch(dict_watcher_id_, dict);
}

int WatcherState::unwatchDict(BorrowedRef<PyDictObject> dict) {
  return PyDict_Unwatch(dict_watcher_id_, dict);
}

int WatcherState::watchType(BorrowedRef<PyTypeObject> type) {
  return PyType_Watch(type_watcher_id_, type.getObj());
}

int WatcherState::unwatchType(BorrowedRef<PyTypeObject> type) {
  return PyType_Unwatch(type_watcher_id_, type.getObj());
}

} // namespace cinderx
