// Copyright (c) Meta Platforms, Inc. and affiliates.

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
