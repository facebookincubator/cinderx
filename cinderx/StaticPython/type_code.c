// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/StaticPython/type_code.h"

#include "cinderx/StaticPython/vtable.h"

int _PyClassLoader_GetTypeCode(PyTypeObject* type) {
  if (type->tp_cache == NULL) {
    return TYPED_OBJECT;
  }

  return ((_PyType_VTable*)type->tp_cache)->vt_typecode;
}
