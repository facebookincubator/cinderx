/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#pragma once

#include "cinderx/python.h"

#include "cinderx/StaticPython/strictmoduleobject.h"
#include "cinderx/StaticPython/thunks.h"

#ifdef __cplusplus
extern "C" {
#endif

int _PyClassLoader_UpdateModuleName(
    Ci_StrictModuleObject* mod,
    PyObject* name,
    PyObject* new_value);

_Py_StaticThunk* _PyClassLoader_GetOrMakeThunk(
    PyObject* func,
    PyObject* original,
    PyObject* container,
    PyObject* name);

#ifdef __cplusplus
}
#endif
