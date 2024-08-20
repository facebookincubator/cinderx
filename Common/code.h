// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>
#ifdef __cplusplus
extern "C" {
#endif

// The following PyCodeObject functions were added in 3.11.
#if PY_VERSION_HEX < 0x030B0000

static inline PyObject* PyCode_GetCode(PyCodeObject* code) {
  return code->co_code;
}

static inline PyObject* PyCode_GetVarnames(PyCodeObject* code) {
  return code->co_varnames;
}

static inline PyObject* PyCode_GetCellvars(PyCodeObject* code) {
  return code->co_cellvars;
}

static inline PyObject* PyCode_GetFreevars(PyCodeObject* code) {
  return code->co_freevars;
}

static inline PyCodeObject* PyUnstable_Code_New(
    int argcount,
    int kwonlyargcount,
    int nlocals,
    int stacksize,
    int flags,
    PyObject* code,
    PyObject* consts,
    PyObject* names,
    PyObject* varnames,
    PyObject* freevars,
    PyObject* cellvars,
    PyObject* filename,
    PyObject* name,
    PyObject* Py_UNUSED(qualname),
    int firstlineno,
    PyObject* linetable,
    PyObject* Py_UNUSED(exceptiontable)) {
  return PyCode_New(
      argcount,
      kwonlyargcount,
      nlocals,
      stacksize,
      flags,
      code,
      consts,
      names,
      varnames,
      freevars,
      cellvars,
      filename,
      name,
      firstlineno,
      linetable);
}

#endif

// Get the internal _Py_CODEUNIT buffer from a code object.
_Py_CODEUNIT* codeUnit(PyCodeObject* code);

// Count the number of bytecode instructions in a code object.
size_t countInstrs(PyCodeObject* code);

// Convert a specialized opcode back to its base form.
int unspecialize(int opcode);

// Convert an instrumented opcode back to its base form.
int uninstrument(PyCodeObject* code, int index);

// Get the byte side of an opcode's inline cache.
//
// This needs to take a code object and an opcode index to process instrumented
// opcodes.
Py_ssize_t inlineCacheSize(PyCodeObject* code, int index);

// Get the name index from a LOAD_GLOBAL's oparg.
int loadGlobalIndex(int oparg);

#ifdef __cplusplus
} // extern "C"
#endif
