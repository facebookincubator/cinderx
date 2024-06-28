// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

namespace jit {

#if PY_VERSION_HEX < 0x030C0000
// A PyObject that is used to indicate that an iterator has finished
// normally. This must never escape into managed code.
extern PyObject g_iterDoneSentinel;
#endif

// Invoke __next__ on iterator
PyObject* invokeIterNext(PyObject* iterator);

} // namespace jit
