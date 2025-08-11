// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#if PY_VERSION_HEX >= 0x030D0000
#include "internal/pycore_code.h"
#endif

#include <stdbool.h>
#include <stddef.h>

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

#if PY_VERSION_HEX < 0x030C0000

// Renamed in 3.12.
#define PyUnstable_Eval_RequestCodeExtraIndex _PyEval_RequestCodeExtraIndex
#define PyUnstable_Code_GetExtra _PyCode_GetExtra
#define PyUnstable_Code_SetExtra _PyCode_SetExtra

#endif

// Gets the qualified name of the code object or "<null>" if it's not set.
const char* codeName(PyCodeObject* code);

// Get the internal _Py_CODEUNIT buffer from a code object.
_Py_CODEUNIT* codeUnit(PyCodeObject* code);

// Count the number of "indices" in a code object.  This used to make more sense
// when each instruction occupied a fixed number of bytes in the bytecode.  In
// some cases it's still helpful to consider sizeof(_Py_CODEUNIT) sized chunks.
size_t countIndices(PyCodeObject* code);

// Convert a specialized opcode back to its base form.
int unspecialize(int opcode);

// Convert an instrumented opcode back to its base form.
int uninstrument(PyCodeObject* code, int index);

// Get the number of inline cache slots used by an opcode.
//
// This needs to take a code object and an opcode index to process instrumented
// opcodes.
Py_ssize_t inlineCacheSize(PyCodeObject* code, int index);

// Get the name index from a LOAD_ATTR's oparg.
int loadAttrIndex(int oparg);

// Get the name index from a LOAD_GLOBAL's oparg.
int loadGlobalIndex(int oparg);

// Before 3.12, Cinder relies on Shadowcode's call count tracking.
#define USE_CODE_EXTRA (PY_VERSION_HEX >= 0x030C0000)

// Extra data attached to a code object.
typedef struct {
  // Number of times the code object has been called.
  uint64_t calls;
} CodeExtra;

// Initialize and finalize the index of the extra data Cinder attaches onto code
// objects.
void initCodeExtraIndex();
void finiCodeExtraIndex();

// Allocate and set a new extra data object on a code object, and then return
// it.  If the code object already has an attached extra data object, just
// return that.
//
// Set a Python error and return nullptr on failure.
CodeExtra* initCodeExtra(PyCodeObject* code);

// Get the extra data object associated with a code object.
//
// Return NULL without setting an error if the code object doesn't have extra
// data attached to it.
CodeExtra* codeExtra(PyCodeObject* code);

// Count the various frame variables that a code object will use.
int numLocals(PyCodeObject* code);
int numCellvars(PyCodeObject* code);
int numFreevars(PyCodeObject* code);
int numLocalsplus(PyCodeObject* code);

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_instruments.h"

uint8_t Cix_GetOriginalOpcode(
    _PyCoLineInstrumentationData* line_data,
    int index);
#elif PY_VERSION_HEX >= 0x030C0000
static inline uint8_t Cix_GetOriginalOpcode(
    _PyCoLineInstrumentationData* line_data,
    int index) {
  return line_data[index].original_opcode;
}
#endif

#ifdef __cplusplus
} // extern "C"
#endif
