/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

// Helper function to dynamically create a builtin module using
// two phase initialization. Returns a strong reference to the newly
// created module.
PyObject* _Ci_CreateBuiltinModule(PyModuleDef* def, const char* name);

#ifdef __cplusplus
} // extern "C"
#endif
