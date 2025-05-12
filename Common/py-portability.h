// Copyright (c) Meta Platforms, Inc. and affiliates.
/*
 * Utilities to smooth out portability between base runtime versions.
 */

#pragma once

#include <Python.h>

#if PY_VERSION_HEX < 0x030C0000
#define CI_INTERP_IMPORT_FIELD(interp, field) interp->field
#else
#define CI_INTERP_IMPORT_FIELD(interp, field) interp->imports.field
#endif

#include "internal/pycore_interp.h"

#if PY_VERSION_HEX < 0x030C0000
#define _PyType_GetDict(type) ((type)->tp_dict)
#define _PyObject_CallNoArgs _PyObject_CallNoArg
#endif
// Basic renames that went into 3.13.
#if PY_VERSION_HEX < 0x030D0000
#define PyList_Extend _PyList_Extend
#define PyLong_AsInt _PyLong_AsInt
#define PyTime_AsSecondsDouble _PyTime_AsSecondsDouble
#define PyTime_t _PyTime_t
#define Py_IsFinalizing _Py_IsFinalizing
#endif

#if PY_VERSION_HEX < 0x030E0000
#define _PyGen_GetGeneratorFromFrame _PyFrame_GetGenerator
#endif
