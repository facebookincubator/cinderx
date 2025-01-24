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

#if PY_VERSION_HEX < 0x030C0000
#define _Py_IMMORTAL_REFCNT kImmortalInitialCount
#endif

#if PY_VERSION_HEX >= 0x030C0000
#define IMMORTALIZE(OBJ) Py_SET_REFCNT((OBJ), _Py_IMMORTAL_REFCNT)
#elif defined(Py_IMMORTAL_INSTANCES)
#define IMMORTALIZE(OBJ) Py_SET_IMMORTAL(OBJ)
#endif
