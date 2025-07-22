// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Reimplements the GET_AITER opcode */
PyObject* Ci_GetAIter(PyThreadState* tstate, PyObject* obj);

/* Reimplements the GET_ANEXT opcode */
PyObject* Ci_GetANext(PyThreadState* tstate, PyObject* aiter);

#ifdef __cplusplus
}
#endif
