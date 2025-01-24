// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

// The pycore_* headers all depend on Python.h implicitly.
//
// clang-format off
#include <Python.h>
// clang-format on

#include "cinderx/StrictModules/pycore_dependencies.h"

// remove conflicting macros from python-ast.h
#ifdef Compare
#undef Compare
#endif
#ifdef Set
#undef Set
#endif
#ifdef arg
#undef arg
#endif
#ifdef FunctionType
#undef FunctionType
#endif
