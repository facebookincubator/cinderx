// Copyright (c) Meta Platforms, Inc. and affiliates.
#include "cinderx/StrictModules/parser_util.h"

#include "cinderx/StrictModules/pycore_dependencies.h"
#include "cinderx/Upgrade/upgrade_assert.h" // @donotremove

#include <fstream>

namespace strictmod {
std::optional<AstAndSymbols> readFromFile(
    const char* filenameStr,
    PyArena* arena,
    const std::vector<std::string>& checkSubStrings) {
  PyFutureFeatures* pyFutures = nullptr;
  PySymtable* symbols = nullptr;
  mod_ty mod = nullptr;
  bool futureAnnotations;
  PyCompilerFlags localflags = _PyCompilerFlags_INIT;

  // if checkSubStrs is available, preliminarily check
  // the file for those strings, and skip parsing if absent
  if (!checkSubStrings.empty()) {
    std::string line;
    bool found = false;

    std::ifstream checkFp(filenameStr);
    if (checkFp.is_open()) {
      while (!checkFp.eof() && !found) {
        getline(checkFp, line);
        for (auto& checkedSubStr : checkSubStrings) {
          if (line.find(checkedSubStr, 0) != std::string::npos) {
            found = true;
            break;
          }
        }
      }
      checkFp.close();
    }
    if (!found) {
      std::make_optional<AstAndSymbols>(mod, symbols, false, false);
    }
  }

  PyObject* filename = PyUnicode_DecodeFSDefault(filenameStr);
  FILE* fp = _Py_fopen_obj(filename, "rb");

  if (fp == nullptr || arena == nullptr) {
    goto error;
  }

#if PY_VERSION_HEX < 0x030C0000
  mod = _PyParser_ASTFromFile(
      fp,
      filename,
      nullptr,
      Py_file_input,
      nullptr,
      nullptr,
      &localflags,
      nullptr,
      arena);
#else
  UPGRADE_ASSERT(AST_UPDATES);
  localflags;
  mod = nullptr;
#endif

  if (mod == nullptr)
    goto error;
#if PY_VERSION_HEX < 0x030C0000
  pyFutures = _PyFuture_FromAST(mod, filename);
#else
  UPGRADE_ASSERT(AST_UPDATES);
#endif
  if (pyFutures == nullptr)
    goto error;
  futureAnnotations = pyFutures->ff_features & CO_FUTURE_ANNOTATIONS;
  symbols = _PySymtable_Build(mod, filename, pyFutures);
  if (symbols == nullptr)
    goto error;
  fclose(fp);
  PyObject_Free(pyFutures);
  Py_DECREF(filename);
  return std::make_optional<AstAndSymbols>(
      mod, symbols, futureAnnotations, true);

error:
  if (PyErr_Occurred()) {
    PyErr_Clear();
  }
  // do not free `mod` since its allocated via arena
  if (fp != nullptr)
    fclose(fp);
  Py_XDECREF(filename);
  if (pyFutures != nullptr)
    PyObject_Free(pyFutures);
#if PY_VERSION_HEX < 0x030C0000
  if (symbols != nullptr)
    _PySymtable_Free(symbols);
#else
  UPGRADE_ASSERT(AST_UPDATES);
#endif

  return {};
}

std::optional<AstAndSymbols> readFromSource(
    const char* source,
    const char* filenameStr,
    int mode,
    PyArena* arena) {
  PyFutureFeatures* pyFutures = nullptr;
  PySymtable* symbols = nullptr;
  mod_ty mod = nullptr;
  bool futureAnnotations;
  PyCompilerFlags localflags = _PyCompilerFlags_INIT;

  PyObject* filename = PyUnicode_DecodeFSDefault(filenameStr);
  if (arena == nullptr) {
    goto error;
  }

#if PY_VERSION_HEX < 0x030C0000
  mod = _PyParser_ASTFromString(source, filename, mode, &localflags, arena);
#else
  UPGRADE_ASSERT(AST_UPDATES);
  localflags;
#endif

  if (mod == nullptr)
    goto error;
#if PY_VERSION_HEX < 0x030C0000
  pyFutures = _PyFuture_FromAST(mod, filename);
#else
  UPGRADE_ASSERT(AST_UPDATES);
#endif
  if (pyFutures == nullptr)
    goto error;
  futureAnnotations = pyFutures->ff_features & CO_FUTURE_ANNOTATIONS;
  symbols = _PySymtable_Build(mod, filename, pyFutures);
  if (symbols == nullptr)
    goto error;
  PyObject_Free(pyFutures);
  Py_DECREF(filename);
  return std::make_optional<AstAndSymbols>(
      mod, symbols, futureAnnotations, true);

error:
  // do not free `mod` since its allocated via arena
  if (pyFutures != nullptr)
    PyObject_Free(pyFutures);
#if PY_VERSION_HEX < 0x030C0000
  if (symbols != nullptr)
    _PySymtable_Free(symbols);
#else
  UPGRADE_ASSERT(AST_UPDATES);
#endif
  return {};
}

} // namespace strictmod
