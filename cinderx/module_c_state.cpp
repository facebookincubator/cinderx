// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "cinderx/module_c_state.h"

#include "cinderx/module_state.h"

extern "C" {

vectorcallfunc Ci_PyFunction_Vectorcall;

int Ci_Watchers_WatchDict(PyObject* dict) {
  return cinderx::getModuleState()->watcherState().watchDict(dict);
}

int Ci_Watchers_UnwatchDict(PyObject* dict) {
  return cinderx::getModuleState()->watcherState().unwatchDict(dict);
}

int Ci_Watchers_WatchType(PyTypeObject* type) {
  return cinderx::getModuleState()->watcherState().watchType(type);
}

int Ci_Watchers_UnwatchType(PyTypeObject* type) {
  return cinderx::getModuleState()->watcherState().unwatchType(type);
}

} // extern "C"
