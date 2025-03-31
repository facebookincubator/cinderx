// Copyright (c) Meta Platforms, Inc. and affiliates.
#pragma once

#include <Python.h>

int JitGen_CheckExact(PyObject* o);
int JitCoro_CheckExact(PyObject* o);

PyObject* JitCoro_GetAwaitableIter(PyObject* o);
