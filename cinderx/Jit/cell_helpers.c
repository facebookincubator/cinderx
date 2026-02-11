// Copyright (c) Meta Platforms, Inc. and affiliates.

// Ideally, we wouldn't need a separate C file and could define these in
// Jit/jit_rt.cpp, but internal/pycore_cell.h has a C++ compatibility issue.
// A fix has been submitted upstream:
// https://github.com/python/cpython/pull/144482
// Once the fix is backported to 3.14, we can move these back and remove this
// file.

#include "Python.h"

#if PY_VERSION_HEX >= 0x030D0000

#include "internal/pycore_cell.h"

PyObject* JITRT_LoadCellItem(PyCellObject* cell) {
  return PyCell_GetRef(cell);
}

PyObject* JITRT_SwapCellItem(PyCellObject* cell, PyObject* new_value) {
  return PyCell_SwapTakeRef(cell, new_value);
}

#endif
