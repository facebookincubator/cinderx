// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/module_state.h"

namespace cinderx::jit {

extern PyType_Spec JitAnextAwaitable_Spec;

PyObject* JitGen_AnextAwaitable_New(
    cinderx::ModuleState* moduleState,
    PyObject* awaitable,
    PyObject* defaultValue);

} // namespace cinderx::jit
