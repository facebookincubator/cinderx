// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

namespace jit {

// A PyObject that is used to indicate that an iterator has finished
// normally. This must never escape into managed code.
extern PyObject g_iterDoneSentinel;

// Invoke __next__ on iterator
PyObject* invokeIterNext(PyObject* iterator);

} // namespace jit
