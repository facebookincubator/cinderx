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


// _Py_IncRefTotal is used by internal functions in 3.12 dictobject.c.
// Pragmatically @Borrow'ing this doesn't seem worth it at this stage. We would
// need UpstreamBorrow.py to somehow not attempt/ignore failure to extract
// _Py_IncRefTotal on non-debug builds where it's deleted by the CPP. All the
// simple solutions I can think of seem just as ugly as manually copying. This
// is made worse by the fact internally _Py_IncRefTotal uses a macro which
// isn't easily visible to us as it's #undef'd after usage. So we'd need a fix
// or to copy that anyway.
#if defined(Py_DEBUG) && PY_VERSION_HEX >= 0x030C0000
#define _Py_IncRefTotal __Py_IncRefTotal
static void _Py_IncRefTotal(PyInterpreterState *interp) {
    interp->object_state.reftotal++;
}

#define _Py_DecRefTotal __Py_DecRefTotal
static void _Py_DecRefTotal(PyInterpreterState *interp) {
    interp->object_state.reftotal--;
}
#endif


// @Borrow CPP directives from Objects/dictobject.c

// These are global singletons and some of the functions we're borrowing
// check for them with pointer equality. Fortunately we are able to get
// the values in init_upstream_borrow().
#if PY_VERSION_HEX < 0x030C0000
static PyObject **empty_values = NULL;
#else
#undef Py_EMPTY_KEYS
static PyDictKeysObject *Py_EMPTY_KEYS = NULL;
#endif

// Internal dependencies for things borrowed from dictobject.c.
// @Borrow function dictkeys_get_index from Objects/dictobject.c [3.12]
// @Borrow function unicode_get_hash from Objects/dictobject.c [3.12]
// @Borrow function unicodekeys_lookup_unicode from Objects/dictobject.c [3.12]
// @Borrow function unicodekeys_lookup_generic from Objects/dictobject.c [3.12]
// @Borrow function dictkeys_generic_lookup from Objects/dictobject.c [3.12]
// Rename to avoid clashing with existing version when statically linking.
#define _Py_dict_lookup __Py_dict_lookup
// @Borrow function _Py_dict_lookup from Objects/dictobject.c [3.12]
// @Borrow function get_dict_state from Objects/dictobject.c
// @Borrow function new_values from Objects/dictobject.c [3.12]
// @Borrow function free_values from Objects/dictobject.c [3.12]
// @Borrow function shared_keys_usable_size from Objects/dictobject.c [3.12]
// @Borrow function free_keys_object from Objects/dictobject.c
// @Borrow function dictkeys_decref from Objects/dictobject.c
// @Borrow function dictkeys_incref from Objects/dictobject.c
// @Borrow function new_dict from Objects/dictobject.c
// @Borrow function new_dict_with_shared_keys from Objects/dictobject.c
// End internal dependencies.

#define _PyObjectDict_SetItem Cix_PyObjectDict_SetItem
// @Borrow function _PyObjectDict_SetItem from Objects/dictobject.c

#define _PyDict_LoadGlobal Cix_PyDict_LoadGlobal
// @Borrow function _PyDict_LoadGlobal from Objects/dictobject.c

// @Borrow function set_attribute_error_context from Objects/object.c

// Wrapper as set_attribute_error_context is declared "static inline".
int
Cix_set_attribute_error_context(PyObject *v, PyObject *name) {
    return set_attribute_error_context(v, name);
}

#if PY_VERSION_HEX >= 0x030C0000
// Internal dependencies for _PyTuple_FromArray.
// Unfortunately these macros can't be borrowed by pulling in the CPP
// directives as they are #undef'd after use.
#define STATE (interp->tuple)
#define FREELIST_FINALIZED (STATE.numfree[0] < 0)
// @Borrow function maybe_freelist_pop from Objects/tupleobject.c [3.12]
// @Borrow function tuple_get_empty from Objects/tupleobject.c [3.12]
// @Borrow function tuple_alloc from Objects/tupleobject.c [3.12]
// End internal dependencies for _PyTuple_FromArray.
#define _PyTuple_FromArray Cix_PyTuple_FromArray
// @Borrow function _PyTuple_FromArray from Objects/tupleobject.c [3.12]
#endif

int init_upstream_borrow(void) {
    PyObject *empty_dict = PyDict_New();
    if (empty_dict == NULL) {
        return -1;
    }
#if PY_VERSION_HEX < 0x030C0000
    empty_values = ((PyDictObject *)empty_dict)->ma_values;
#else
    Py_EMPTY_KEYS = ((PyDictObject *)empty_dict)->ma_keys;
#endif
    Py_DECREF(empty_dict);
    return 0;
}
