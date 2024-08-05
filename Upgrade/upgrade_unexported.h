// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <Python.h>

#if PY_VERSION_HEX >= 0x030C0000

#include "pycore_typeobject.h"

#ifdef __cplusplus
extern "C" {
#endif

// pycore_typeobject.h
static_builtin_state* _PyStaticType_GetState(PyInterpreterState *, PyTypeObject *);

// pycore_unionobject.h
PyObject *_Py_union_type_or(PyObject *, PyObject *);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // PY_VERSION_HEX >= 0x030C0000
