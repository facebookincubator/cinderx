// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/watchers.h"

static int code_watcher_id = -1;
static int dict_watcher_id = -1;
static int func_watcher_id = -1;
static int type_watcher_id = -1;

int Ci_Watchers_Init(const WatcherState* state) {
  if ((code_watcher_id = PyCode_AddWatcher(state->code_watcher)) < 0) {
    return -1;
  }
  if ((dict_watcher_id = PyDict_AddWatcher(state->dict_watcher)) < 0) {
    return -1;
  }
  if ((func_watcher_id = PyFunction_AddWatcher(state->func_watcher)) < 0) {
    return -1;
  }
  if ((type_watcher_id = PyType_AddWatcher(state->type_watcher)) < 0) {
    return -1;
  }

  return 0;
}

int Ci_Watchers_Fini() {
  if (dict_watcher_id != -1 && PyDict_ClearWatcher(dict_watcher_id)) {
    return -1;
  }
  dict_watcher_id = -1;

  if (type_watcher_id != -1 && PyType_ClearWatcher(type_watcher_id)) {
    return -1;
  }
  type_watcher_id = -1;

  if (func_watcher_id != -1 && PyFunction_ClearWatcher(func_watcher_id)) {
    return -1;
  }
  func_watcher_id = -1;

  if (code_watcher_id != -1 && PyCode_ClearWatcher(code_watcher_id)) {
    return -1;
  }
  code_watcher_id = -1;

  return 0;
}

int Ci_Watchers_WatchDict(PyObject* dict) {
  return PyDict_Watch(dict_watcher_id, dict);
}

int Ci_Watchers_UnwatchDict(PyObject* dict) {
  return PyDict_Unwatch(dict_watcher_id, dict);
}

int Ci_Watchers_WatchType(PyTypeObject* type) {
  return PyType_Watch(type_watcher_id, reinterpret_cast<PyObject*>(type));
}

int Ci_Watchers_UnwatchType(PyTypeObject* type) {
  return PyType_Unwatch(type_watcher_id, reinterpret_cast<PyObject*>(type));
}
