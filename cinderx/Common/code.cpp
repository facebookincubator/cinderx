// Copyright (c) Meta Platforms, Inc. and affiliates.

// For opcodeName().
#define NEED_OPCODE_NAMES

#include "cinderx/Common/code.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove
#include "cinderx/module_state.h"

#ifdef ENABLE_ZLIB
#include <zlib.h>
#endif

#include "cpython/code.h"

namespace jit {
static std::string fullnameImpl(PyObject* module, PyObject* qualname) {
  auto safe_str = [](BorrowedRef<> str) {
    if (str == nullptr || !PyUnicode_Check(str)) {
      return "<invalid>";
    }
    return PyUnicode_AsUTF8(str);
  };
  return fmt::format("{}:{}", safe_str(module), safe_str(qualname));
}

std::string codeFullname(
    BorrowedRef<PyObject> module,
    BorrowedRef<PyCodeObject> code) {
  return fullnameImpl(module, code->co_qualname);
}

std::string funcFullname(BorrowedRef<PyFunctionObject> func) {
  return fullnameImpl(func->func_module, func->func_qualname);
}

PyObject* getVarnameTuple(BorrowedRef<PyCodeObject> code, int* idx) {
  if (*idx < code->co_nlocals) {
    return PyCode_GetVarnames(code);
  }

  *idx -= code->co_nlocals;
  auto cellvars = Ref<>::steal(PyCode_GetCellvars(code));
  auto ncellvars = PyTuple_GET_SIZE(cellvars.get());
  if (*idx < ncellvars) {
    return cellvars.release();
  }

  *idx -= ncellvars;
  return PyCode_GetFreevars(code);
}

PyObject* getVarname(BorrowedRef<PyCodeObject> code, int idx) {
  return PyTuple_GET_ITEM(code->co_localsplusnames, idx);
}

uint32_t hashBytecode(BorrowedRef<PyCodeObject> code) {
  auto bc = Ref<>::steal(PyCode_GetCode(code));
#ifdef ENABLE_ZLIB
  uint32_t crc = crc32(0, nullptr, 0);
  if (!PyBytes_Check(bc)) {
    return crc;
  }

  char* buffer;
  Py_ssize_t len;
  if (PyBytes_AsStringAndSize(bc, &buffer, &len) < 0) {
    return crc;
  }

  return crc32(crc, reinterpret_cast<unsigned char*>(buffer), len);
#else
  return PyObject_Hash(bc);
#endif
}

std::string codeQualname(BorrowedRef<PyCodeObject> code) {
  if (code->co_qualname != nullptr) {
    return unicodeAsString(code->co_qualname);
  }
  if (code->co_name != nullptr) {
    return unicodeAsString(code->co_name);
  }
  return "<unknown>";
}

} // namespace jit

extern "C" {

const char* codeName(PyCodeObject* code) {
  if (code->co_qualname == nullptr) {
    return "<null>";
  }
  return PyUnicode_AsUTF8(code->co_qualname);
}

_Py_CODEUNIT* codeUnit(PyCodeObject* code) {
  return _PyCode_CODE(code);
}

size_t countIndices(PyCodeObject* code) {
  // PyCode_GetCode can allocate to create a copy of the de-opted code
  // which we don't need just to determine the number of indices.
  return _PyCode_NBYTES(code) / sizeof(_Py_CODEUNIT);
}

int unspecialize(int opcode) {
  // The deopt table has size 256, and pseudo-opcodes and stubs are by
  // definition unspecialized already.
  return (opcode >= 0 && opcode <= 255) ? _CiOpcode_Deopt[opcode] : opcode;
}

int uninstrument(PyCodeObject* code, int index) {
  int opcode = _Py_OPCODE(codeUnit(code)[index]);

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

Py_ssize_t inlineCacheSize(PyCodeObject* code, int index) {
  return _CiOpcode_Caches[unspecialize(uninstrument(code, index))];
}

int loadAttrIndex(int oparg) {
  return oparg >> 1;
}

int loadGlobalIndex(int oparg) {
  return oparg >> 1;
}

void initCodeExtraIndex() {
  auto state = cinderx::getModuleState();
  JIT_CHECK(
      state != nullptr,
      "Trying to initialize code extra index but there's no module state");
  JIT_CHECK(
      state->code_extra_index == -1,
      "Cannot re-initialize code extra index without finalizing it first");

  state->code_extra_index = PyUnstable_Eval_RequestCodeExtraIndex(PyMem_Free);
}

void finiCodeExtraIndex() {
  auto state = cinderx::getModuleState();
  JIT_CHECK(
      state != nullptr,
      "Trying to finalize code extra index but there's no module state");
  JIT_CHECK(
      state->code_extra_index != -1,
      "Cannot finalize code extra index without initializing it first");

  state->code_extra_index = -1;
}

CodeExtra* codeExtra(PyCodeObject* code) {
  auto* state = cinderx::getModuleState();
  // On shutdown the module state becomes inaccessible.
  if (state == nullptr) {
    return nullptr;
  }
  Py_ssize_t extra_index = state->code_extra_index;
  if (extra_index == -1) {
    return nullptr;
  }

  auto code_obj = reinterpret_cast<PyObject*>(code);

  // Lock the code object to prevent concurrent get-or-create races under
  // FT-Python. Under GIL builds this is a no-op.
  jit::CriticalSectionGuard guard(code_obj);

  void* data_ptr = nullptr;
  if (PyUnstable_Code_GetExtra(code_obj, extra_index, &data_ptr) < 0) {
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

  if (PyUnstable_Code_SetExtra(code_obj, extra_index, extra) < 0) {
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
  return code->co_ncellvars;
}

int numFreevars(PyCodeObject* code) {
  return code->co_nfreevars;
}

int numLocalsplus(PyCodeObject* code) {
  return code->co_nlocalsplus;
}

} // extern "C"
