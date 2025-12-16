// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/code_extra.h"

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

// Get the name of a Python opcode.
const char* opcodeName(int opcode);

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

// Initialize and finalize the index of the extra data Cinder attaches onto code
// objects.
void initCodeExtraIndex();
void finiCodeExtraIndex();

// Get the extra data object associated with a code object. Lazily allocates
// this data if this is the first access. Returns nullptr on failure with no
// Python error set.
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

#include "cinderx/Common/ref.h"

#include <string>

namespace jit {

std::string codeFullname(
    BorrowedRef<PyObject> module,
    BorrowedRef<PyCodeObject> code);
std::string funcFullname(BorrowedRef<PyFunctionObject> func);

// Given a code object and an index into f_localsplus, compute which of
// code->co_varnames, code->cellvars, or code->freevars contains the name of
// the variable. Return that tuple and adjust idx as needed.
PyObject* getVarnameTuple(BorrowedRef<PyCodeObject> code, int* idx);

// Similar to getVarnameTuple, but return the name itself rather than the
// containing tuple.
PyObject* getVarname(BorrowedRef<PyCodeObject> code, int idx);

uint32_t hashBytecode(BorrowedRef<PyCodeObject> code);

// Return the qualname of the given code object, falling back to its name or
// "<unknown>" if not set.
std::string codeQualname(BorrowedRef<PyCodeObject> code);

} // namespace jit

#endif
