// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

// Use this header file whenever depending on internal Python APIs.
// It will make sure that our usage of static state works across
// multiple Python versions (e.g. Py_ID will determine the correct
// location at runtime to look in Python's _PyRuntime structure even
// if the size has changed).

#include "cinderx/python.h"

#include "internal/pycore_runtime.h"

#if PY_VERSION_HEX >= 0x030E0000
extern struct _Py_static_objects* _static_objects;

#undef _Py_GLOBAL_OBJECT
#define _Py_GLOBAL_OBJECT(NAME) _static_objects->NAME
#endif

#ifdef __cplusplus
namespace cinderx {
void initStaticObjects();
}
#endif
