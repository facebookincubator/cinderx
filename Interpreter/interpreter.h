// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

PyObject * Ci_GetAIter(PyThreadState *tstate, PyObject *obj);
PyObject * Ci_GetANext(PyThreadState *tstate, PyObject *aiter);
PyObject* _Py_HOT_FUNCTION Ci_EvalFrame(PyThreadState *tstate, PyFrameObject *f, int throwflag);

#ifdef __cplusplus
}
#endif
