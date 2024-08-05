// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <Python.h>

#ifdef __cplusplus
extern "C" {
#endif

PyObject *
Cix_PyGen_yf(PyGenObject *gen);

#ifdef __cplusplus
} // extern "C"
#endif
