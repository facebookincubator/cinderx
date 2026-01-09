#include "cinderx/python_runtime.h"

#include <cstddef>

#if PY_VERSION_HEX >= 0x030E0000

struct _Py_static_objects* _static_objects;

#endif

namespace cinderx {
void initStaticObjects() {
#if PY_VERSION_HEX >= 0x030E0000
  // The first small int is the first value in _Py_static_objects. Get that
  // object and then use that as the address of _static_objects. This makes our
  // usage of global singletons resilient across minor Python versions.
  static_assert(
      offsetof(struct _Py_static_objects, singletons.small_ints) == 0,
      "singletons have changed");

  PyObject* first_obj = PyLong_FromLong(-_PY_NSMALLNEGINTS);
  assert(first_obj != nullptr);

  _static_objects = (_Py_static_objects*)first_obj;
  Py_DECREF(first_obj);
#endif
}

} // namespace cinderx
