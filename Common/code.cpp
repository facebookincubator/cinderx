// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/code.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/opcode_stubs.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove

#if PY_VERSION_HEX >= 0x030C0000

// Needed to tell pycore_opcode.h to define the _PyOpcode* arrays.
#define NEED_OPCODE_TABLES

// Have to rename these arrays because they're externally linked in CPython, so
// this will hit linker errors for duplicate definitions if they're pulled in
// directly.
#define _PyOpcode_Caches Ci_Opcode_Caches
#define _PyOpcode_Deopt Ci_Opcode_Deopt
#define _PyOpcode_Jump Ci_Opcode_Jump

#include "internal/pycore_opcode.h"

#endif

extern "C" {

_Py_CODEUNIT* codeUnit(PyCodeObject* code) {
  PyObject* bytes_obj = PyCode_GetCode(code);
  JIT_DCHECK(
      PyBytes_CheckExact(bytes_obj),
      "Code object must have its instructions stored as a byte string");
  return (_Py_CODEUNIT*)PyBytes_AS_STRING(PyCode_GetCode(code));
}

size_t countInstrs(PyCodeObject* code) {
  return PyBytes_GET_SIZE(PyCode_GetCode(code)) / sizeof(_Py_CODEUNIT);
}

int unspecialize(int opcode) {
#if PY_VERSION_HEX >= 0x030C0000
  // The deopt table has size 256, and pseudo-opcodes and stubs are by
  // definition unspecialized already.
  return (opcode >= 0 && opcode <= 255) ? Ci_Opcode_Deopt[opcode] : opcode;
#else
  return opcode;
#endif
}

int uninstrument(PyCodeObject* code, int index) {
  int opcode = _Py_OPCODE(codeUnit(code)[index]);

#if PY_VERSION_HEX >= 0x030C0000
  // Check if there's an equivalent opcode without instrumentation.
  uint8_t base_opcode = Cix_DEINSTRUMENT(static_cast<uint8_t>(opcode));
  if (base_opcode != 0) {
    return base_opcode;
  }

  // Instrumented lines and arbitrary instrumented instructions need to check
  // different tables.
  if (opcode == INSTRUMENTED_INSTRUCTION) {
    return code->_co_monitoring->per_instruction_opcodes[index];
  }
  if (opcode == INSTRUMENTED_LINE) {
    return code->_co_monitoring->lines[index].original_opcode;
  }
#endif

  return opcode;
}

Py_ssize_t inlineCacheSize(
    [[maybe_unused]] PyCodeObject* code,
    [[maybe_unused]] int index) {
#if PY_VERSION_HEX >= 0x030C0000
  return Ci_Opcode_Caches[unspecialize(uninstrument(code, index))];
#else
  return 0;
#endif
}

int loadAttrIndex(int oparg) {
#if PY_VERSION_HEX >= 0x030C0000
  return oparg >> 1;
#else
  return oparg;
#endif
}

int loadGlobalIndex(int oparg) {
#if PY_VERSION_HEX >= 0x030B0000
  return oparg >> 1;
#else
  return oparg;
#endif
}

} // extern "C"
