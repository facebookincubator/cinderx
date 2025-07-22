// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

int JitGen_CheckExact(PyObject* o);
int JitCoro_CheckExact(PyObject* o);

PyObject* JitCoro_GetAwaitableIter(PyObject* o);

#ifdef __cplusplus
} // extern "C"
#endif
