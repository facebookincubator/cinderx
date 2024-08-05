// This file is processed by UpstreamBorrow.py. To see the generated output:
// buck build --out=- fbcode//cinderx/UpstreamBorrow:gen_borrowed.c

#include "cinderx/UpstreamBorrow/borrowed.h"

// @Borrow CPP directives from Objects/genobject.c

// Internal dependencies for _PyGen_yf which only exist in 3.12.
// @Borrow function is_resume from Objects/genobject.c [3.12]
// @Borrow function _PyGen_GetCode from Objects/genobject.c [3.12]
// End internal dependencies for _PyGen_yf.

#define _PyGen_yf Cix_PyGen_yf
// @Borrow function _PyGen_yf from Objects/genobject.c


// Internal dependencies for _PyCoro_GetAwaitableIter.
// @Borrow function gen_is_coroutine from Objects/genobject.c
// End internal dependencies for _PyCoro_GetAwaitableIter.

#define _PyCoro_GetAwaitableIter Cix_PyCoro_GetAwaitableIter
// @Borrow function _PyCoro_GetAwaitableIter from Objects/genobject.c


// Internal dependencies for _PyAsyncGenValueWrapperNew.
// @Borrow typedef _PyAsyncGenWrappedValue from Objects/genobject.c
// @Borrow function get_async_gen_state from Objects/genobject.c
// End internal dependencies for _PyAsyncGenValueWrapperNew.

#if PY_VERSION_HEX < 0x030C0000
#define _PyAsyncGenValueWrapperNew Cix_PyAsyncGenValueWrapperNew
#else
// In 3.12 we need a temporary name before wrapping to avoid conflicting with
// the forward declaration in genobject.h.
#define _PyAsyncGenValueWrapperNew __PyAsyncGenValueWrapperNew
#endif
// @Borrow function _PyAsyncGenValueWrapperNew from Objects/genobject.c
#if PY_VERSION_HEX >= 0x030C0000
// In 3.12 _PyAsyncGenValueWrapperNew needs thread-state. As this is used from
// the JIT we could get the value from the thread-state register. This would be
// slightly more efficient, but quite a bit more work and async-generators are
// rare. So we just wrap it up here.
PyObject*
Cix_PyAsyncGenValueWrapperNew(PyObject* value) {
  return __PyAsyncGenValueWrapperNew(PyThreadState_GET(), value);
}
#endif


// @Borrow function set_attribute_error_context from Objects/object.c

// Wrapper as set_attribute_error_context is declared "static inline".
int
Cix_set_attribute_error_context(PyObject *v, PyObject *name) {
    return set_attribute_error_context(v, name);
}
