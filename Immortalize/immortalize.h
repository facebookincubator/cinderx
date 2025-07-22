// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include <stdbool.h>

#if PY_VERSION_HEX < 0x030C0000
#define _Py_IMMORTAL_REFCNT kImmortalInitialCount
#endif

/*
 * Immortalizes a Python object but does not check if that makes sense to do so.
 * You probably want to use the `immortalize()` function instead.
 */
#if defined(Py_IMMORTAL_INSTANCES)
#define IMMORTALIZE(OBJ) Py_SET_IMMORTAL(OBJ)
#elif PY_VERSION_HEX >= 0x030E0000
#define IMMORTALIZE(OBJ) Py_SET_REFCNT((OBJ), _Py_IMMORTAL_INITIAL_REFCNT)
#elif PY_VERSION_HEX >= 0x030C0000
#define IMMORTALIZE(OBJ) Py_SET_REFCNT((OBJ), _Py_IMMORTAL_REFCNT)
#endif

/*
 * Check if a Python object can be immortalized.
 */
bool can_immortalize(PyObject* obj);

/*
 * Immortalize a Python object, returning true if the operation was successful
 * and false otherwise.
 */
bool immortalize(PyObject* obj);

/*
 * Immortalize the Python objects currently on the heap.
 *
 * NOTE: In 3.10.cinder, this imports `gc` and calls `gc.immortalize_heap()`
 */
PyObject* immortalize_heap(PyObject* mod);
