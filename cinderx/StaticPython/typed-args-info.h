// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

typedef struct {
  int tai_primitive_type;
  PyTypeObject* tai_type;
  int tai_argnum;
  int tai_optional;
  int tai_exact;
} _PyTypedArgInfo;

typedef struct {
  PyObject_VAR_HEAD _PyTypedArgInfo tai_args[1];
} _PyTypedArgsInfo;

extern PyTypeObject _PyTypedArgsInfo_Type;
