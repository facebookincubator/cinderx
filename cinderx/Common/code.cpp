// Copyright (c) Meta Platforms, Inc. and affiliates.

// For opcodeName().
#define NEED_OPCODE_NAMES

#include "cinderx/Common/code.h"

#include "cinderx/Common/log.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove

#if PY_VERSION_HEX >= 0x030C0000

#include "cpython/code.h"

#endif

namespace {

// Index used for fetching code object extra data.
Py_ssize_t code_extra_index = -1;

} // namespace

extern "C" {

const char* codeName(PyCodeObject* code) {
  if (code->co_qualname == nullptr) {
    return "<null>";
  }
  return PyUnicode_AsUTF8(code->co_qualname);
}

_Py_CODEUNIT* codeUnit(PyCodeObject* code) {
#if PY_VERSION_HEX >= 0x030C0000
  return _PyCode_CODE(code);
#else
  PyObject* bytes_obj = PyCode_GetCode(code);
  JIT_DCHECK(
      PyBytes_CheckExact(bytes_obj),
      "Code object must have its instructions stored as a byte string");
  return (_Py_CODEUNIT*)PyBytes_AS_STRING(PyCode_GetCode(code));
#endif
}

size_t countIndices(PyCodeObject* code) {
#if PY_VERSION_HEX >= 0x030C0000
  // PyCode_GetCode can allocate to create a copy of the de-opted code
  // which we don't need just to determine the number of indices.
  return _PyCode_NBYTES(code) / sizeof(_Py_CODEUNIT);
#else
  return PyBytes_GET_SIZE(PyCode_GetCode(code)) / sizeof(_Py_CODEUNIT);
#endif
}

int unspecialize(int opcode) {
#if PY_VERSION_HEX >= 0x030C0000
  // The deopt table has size 256, and pseudo-opcodes and stubs are by
  // definition unspecialized already.
  return (opcode >= 0 && opcode <= 255) ? _CiOpcode_Deopt[opcode] : opcode;
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
    return Cix_GetOriginalOpcode(code->_co_monitoring->lines, index);
  }
#endif

  return opcode;
}

const char* opcodeName(int opcode) {
  constexpr size_t num_opcodes =
      sizeof(_CiOpcode_OpName) / sizeof(_CiOpcode_OpName[0]);
  if (opcode < 0 || opcode >= num_opcodes) {
    return "<unrecognized opcode>";
  }
  const char* name = _CiOpcode_OpName[opcode];
  return name != nullptr ? name : "<unknown opcode>";
}

Py_ssize_t inlineCacheSize(
    [[maybe_unused]] PyCodeObject* code,
    [[maybe_unused]] int index) {
#if PY_VERSION_HEX >= 0x030C0000
  return _CiOpcode_Caches[unspecialize(uninstrument(code, index))];
#else
  return 0;
#endif
}

int loadAttrIndex(int oparg) {
  if constexpr (PY_VERSION_HEX >= 0x030C0000) {
    return oparg >> 1;
  }
  return oparg;
}

int loadGlobalIndex(int oparg) {
  if constexpr (PY_VERSION_HEX >= 0x030B0000) {
    return oparg >> 1;
  }
  return oparg;
}

void initCodeExtraIndex() {
  if constexpr (!USE_CODE_EXTRA) {
    return;
  }
  JIT_CHECK(
      code_extra_index == -1,
      "Cannot re-initialize code extra index without finalizing it first");

  code_extra_index = PyUnstable_Eval_RequestCodeExtraIndex(PyMem_Free);
}

void finiCodeExtraIndex() {
  if constexpr (!USE_CODE_EXTRA) {
    return;
  }
  JIT_CHECK(
      code_extra_index != -1,
      "Cannot finalize code extra index without initializing it first");

  code_extra_index = -1;
}

CodeExtra* codeExtra(PyCodeObject* code) {
  if constexpr (!USE_CODE_EXTRA) {
    return nullptr;
  }

  if (code_extra_index == -1) {
    return nullptr;
  }

  auto code_obj = reinterpret_cast<PyObject*>(code);

  void* data_ptr = nullptr;
  if (PyUnstable_Code_GetExtra(code_obj, code_extra_index, &data_ptr) < 0) {
    JIT_LOG("Failed to get code extra data for {}", codeName(code));
    jit::printPythonException();
    PyErr_Clear();
    return nullptr;
  }
  if (data_ptr != nullptr) {
    return reinterpret_cast<CodeExtra*>(data_ptr);
  }

  auto extra = reinterpret_cast<CodeExtra*>(PyMem_Calloc(1, sizeof(CodeExtra)));
  if (extra == nullptr) {
    return nullptr;
  }

  if (PyUnstable_Code_SetExtra(code_obj, code_extra_index, extra) < 0) {
    JIT_LOG("Failed to set code extra data for {}", codeName(code));
    jit::printPythonException();
    PyErr_Clear();
    PyMem_Free(extra);
    return nullptr;
  }

  return extra;
}

int numLocals(PyCodeObject* code) {
  return code->co_nlocals;
}

int numCellvars(PyCodeObject* code) {
#if PY_VERSION_HEX >= 0x030B0000
  return code->co_ncellvars;
#else
  return PyTuple_GET_SIZE(PyCode_GetCellvars(code));
#endif
}

int numFreevars(PyCodeObject* code) {
#if PY_VERSION_HEX >= 0x030B0000
  return code->co_nfreevars;
#else
  return PyTuple_GET_SIZE(PyCode_GetFreevars(code));
#endif
}

int numLocalsplus(PyCodeObject* code) {
#if PY_VERSION_HEX >= 0x030B0000
  return code->co_nlocalsplus;
#else
  return numLocals(code) + numCellvars(code) + numFreevars(code);
#endif
}

} // extern "C"
