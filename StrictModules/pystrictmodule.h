// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <Python.h>

#include "cinderx/StrictModules/strict_module_checker_interface.h"

#ifndef Py_LIMITED_API
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  PyObject_HEAD
  StrictModuleChecker* checker;
} StrictModuleLoaderObject;

typedef struct {
  PyObject_HEAD
  int valid_module;
  PyObject* module_name;
  PyObject* file_name;
  int module_kind;
  int stub_kind;
  PyObject* ast;
  PyObject* symtable;
  PyObject* errors;
} StrictModuleAnalysisResult;

extern PyTypeObject Ci_StrictModuleLoader_Type;
extern PyTypeObject Ci_StrictModuleAnalysisResult_Type;

// module kind
#define Ci_NONSTRICT_MODULE_KIND 0
#define Ci_STRICT_MODULE_KIND 1
#define Ci_STATIC_MODULE_KIND 2

// stub kind
#define Ci_STUB_KIND_MASK_NONE 0b000
#define Ci_STUB_KIND_MASK_ALLOWLIST 0b011
#define Ci_STUB_KIND_MASK_TYPING 0b100
#define Ci_STUB_KIND_MASK_STRICT 0b001

#define StrictModuleLoaderObject_Check(v) \
  (Py_TYPE(v) == &Ci_StrictModuleLoader_Type)

#ifdef __cplusplus
}
#endif
#endif /* Py_LIMITED_API */
