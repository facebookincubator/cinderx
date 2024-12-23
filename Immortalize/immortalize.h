// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

PyObject* is_immortal(PyObject* obj);

/**
 * Immortalize the Python objects currently on the heap.
 * NOTE: In 3.10.cinder, this imports `gc` and calls `gc.immortalize_heap()`
 */
PyObject* immortalize_heap(PyObject* /* mod */, PyObject*);
