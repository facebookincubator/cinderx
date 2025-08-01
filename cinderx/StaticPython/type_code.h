// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TYPED_INT_UNSIGNED 0
#define TYPED_INT_SIGNED 1

#define TYPED_INT_8BIT 0
#define TYPED_INT_16BIT 1
#define TYPED_INT_32BIT 2
#define TYPED_INT_64BIT 3

#define TYPED_INT8 (TYPED_INT_8BIT << 1 | TYPED_INT_SIGNED)
#define TYPED_INT16 (TYPED_INT_16BIT << 1 | TYPED_INT_SIGNED)
#define TYPED_INT32 (TYPED_INT_32BIT << 1 | TYPED_INT_SIGNED)
#define TYPED_INT64 (TYPED_INT_64BIT << 1 | TYPED_INT_SIGNED)

#define TYPED_UINT8 (TYPED_INT_8BIT << 1 | TYPED_INT_UNSIGNED)
#define TYPED_UINT16 (TYPED_INT_16BIT << 1 | TYPED_INT_UNSIGNED)
#define TYPED_UINT32 (TYPED_INT_32BIT << 1 | TYPED_INT_UNSIGNED)
#define TYPED_UINT64 (TYPED_INT_64BIT << 1 | TYPED_INT_UNSIGNED)

// Gets one of TYPED_INT_8BIT, TYPED_INT_16BIT, etc.. from TYPED_INT8,
// TYPED_UINT8, etc... also TYPED_SIZE(TYPED_BOOL) == TYPED_INT_8BIT
#define TYPED_SIZE(typed_int) ((typed_int >> 1) & 3)

#define TYPED_OBJECT 0x08
#define TYPED_DOUBLE 0x09
#define TYPED_SINGLE 0x0A
#define TYPED_CHAR 0x0B
// must be even: TYPED_BOOL & TYPED_INT_SIGNED should be false
#define TYPED_BOOL 0x0C
#define TYPED_VOID 0x0D
#define TYPED_STRING 0x0E
#define TYPED_ERROR 0x0F

#define TYPED_ARRAY 0x80

#define _Py_IS_TYPED_ARRAY(x) (x & TYPED_ARRAY)
#define _Py_IS_TYPED_ARRAY_SIGNED(x) (x & (TYPED_INT_SIGNED << 4))

#define Ci_METH_TYPED 0x0400

int _PyClassLoader_GetTypeCode(PyTypeObject* type);

#ifdef __cplusplus
}
#endif
