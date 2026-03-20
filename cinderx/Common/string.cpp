// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Common/string.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"

extern "C" PyObject* Ci_InitStaticStringImpl(const char* s) {
  PyObject* obj = PyUnicode_FromString(s);
  JIT_CHECK(
      obj != nullptr,
      "Fatal error, failed to initialize static string '{}'",
      s);

#if PY_VERSION_HEX >= 0x030E0000 && defined(Py_GIL_DISABLED)
  obj->ob_tid = _Py_UNOWNED_TID;
  obj->ob_ref_local = _Py_IMMORTAL_REFCNT_LOCAL;
  obj->ob_ref_shared = 0;
  _Py_atomic_or_uint8(&obj->ob_gc_bits, _PyGC_BITS_DEFERRED);
  _PyASCIIObject_CAST(obj)->state.statically_allocated = 1;
#else
  obj->ob_refcnt = 0x3fffffff;
#endif

  return obj;
}
