/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/vtable_defs.h"

#include "cinderx/CachedProperties/cached_properties.h"
#include "cinderx/Common/func.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/StaticPython/awaitable.h"
#include "cinderx/StaticPython/descrs.h"
#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/functype.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/type.h"
#include "cinderx/StaticPython/typed-args-info.h"
#include "cinderx/StaticPython/typed_method_def.h"
#include "cinderx/StaticPython/vtable.h"
#include "cinderx/Upgrade/upgrade_stubs.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif

#include <Python.h>

#define _PyClassMethod_Check(op) (Py_TYPE(op) == &PyClassMethod_Type)

// For simple signatures which are the most common that take and return Python
// objects we have a number of built-in fixed values that can be re-used w/o
// allocation.
static _PyClassLoader_ThunkSignature simple_sigs[] = {
    THUNK_SIG(0),
    THUNK_SIG(1),
    THUNK_SIG(2),
    THUNK_SIG(3),
    THUNK_SIG(4),
    THUNK_SIG(5),
    THUNK_SIG(6),
    THUNK_SIG(7),
    THUNK_SIG(8),
    THUNK_SIG(9),
    THUNK_SIG(10),
};

// Gets a thunk signature for a given code object. If the code object has all
// primitive values and a limited number of args we can use the fixed simple
// signatures. Otherwise we'll need to allocate a new signature object.
// Can specify an extra argument for self which is used when we have a static
// method where the argument is logically there for the invoke, but not in the
// code object.
_PyClassLoader_ThunkSignature* _PyClassLoader_GetThunkSignatureFromCode(
    PyCodeObject* code,
    int extra_args) {
  PyObject* checks = _PyClassLoader_GetCodeArgumentTypeDescrs(code);
  if (checks == NULL) {
    return NULL;
  }
  int optional, exact;
  PyTypeObject* ret_type =
      _PyClassLoader_ResolveCodeReturnType(code, &optional, &exact);
  if (ret_type == NULL) {
    return NULL;
  }
  int ret_typecode = _PyClassLoader_GetTypeCode(ret_type);
  Py_DECREF(ret_type);

  // Scan the signature and see if it has any primitive arguments
  Py_ssize_t arg_count = code->co_argcount;
  Py_ssize_t check_count = _PyClassLoader_GetArgumentDescrLength(checks);
  _PyClassLoader_ThunkSignature* sig = NULL;
  for (Py_ssize_t i = 0; i < check_count; i++) {
    PyObject* type_descr = _PyClassLoader_GetArgumentDescrType(checks, i);
    int arg_type = _PyClassLoader_ResolvePrimitiveType(type_descr);
    if (arg_type != TYPED_OBJECT) {
      if (sig == NULL) {
        // First primitive argument, allocate and initialize the signature
        sig = (_PyClassLoader_ThunkSignature*)PyMem_Malloc(
            sizeof(_PyClassLoader_ThunkSignature) +
            sizeof(uint8_t) * (arg_count + extra_args));
        if (sig == NULL) {
          return NULL;
        }
        sig->ta_argcount = arg_count + extra_args;
        sig->ta_has_primitives = 1;
        sig->ta_allocated = 1;
        sig->ta_rettype = ret_typecode;
        // Checks are sparse and are (arg_num, type_descr) and are in-order. So
        // we initialize all positions to TYPED_OBJECT and will overwrite the
        // primitive ones.
        for (int j = 0; j < arg_count + extra_args; j++) {
          sig->ta_argtype[j] = TYPED_OBJECT;
        }
      }
      int arg_pos = _PyClassLoader_GetArgumentDescrPosition(checks, i);
      sig->ta_argtype[arg_pos + extra_args] = arg_type;
    }
  }
  if (sig != NULL) {
    // We have primitive arguments and an initialized signature.
    return sig;
  }

  // See if we have a fixed-size signature for a method w/ no primitives.
  if ((arg_count + extra_args) <
          sizeof(simple_sigs) / sizeof(_PyClassLoader_ThunkSignature) &&
      ret_typecode == TYPED_OBJECT) {
    return &simple_sigs[arg_count + extra_args];
  }

  // Long signature or primitive return, we need to allocate a signature object.
  sig = (_PyClassLoader_ThunkSignature*)PyMem_Malloc(
      sizeof(_PyClassLoader_ThunkSignature) + sizeof(uint8_t) * arg_count);
  if (sig == NULL) {
    return NULL;
  }

  sig->ta_argcount = arg_count + extra_args;
  sig->ta_allocated = 1;
  sig->ta_has_primitives = 0;
  sig->ta_rettype = ret_typecode;
  for (int j = 0; j < arg_count + extra_args; j++) {
    sig->ta_argtype[j] = TYPED_OBJECT;
  }
  return sig;
}

_PyClassLoader_ThunkSignature* _PyClassLoader_GetThunkSignatureFromFunction(
    PyObject* function,
    int extra_args) {
  return _PyClassLoader_GetThunkSignatureFromCode(
      (PyCodeObject*)((PyFunctionObject*)function)->func_code, extra_args);
}

_PyClassLoader_ThunkSignature* _PyClassLoader_GetThunkSignature(
    PyObject* original) {
  if (PyFunction_Check(original)) {
    return _PyClassLoader_GetThunkSignatureFromFunction(original, 0);
  } else if (_PyClassMethod_Check(original)) {
    original = Ci_PyClassMethod_GetFunc(original);
    if (!PyFunction_Check(original)) {
      PyErr_SetString(PyExc_RuntimeError, "Not a function in a class method");
      return NULL;
    }
    return _PyClassLoader_GetThunkSignatureFromFunction(original, 0);
  } else if (Py_TYPE(original) == &PyStaticMethod_Type) {
    original = Ci_PyStaticMethod_GetFunc(original);
    if (!PyFunction_Check(original)) {
      PyErr_SetString(PyExc_RuntimeError, "Not a function in a class method");
      return NULL;
    }
    // static method doesn't take self, but it's passed as an argument in an
    // INVOKE_METHOD.
    return _PyClassLoader_GetThunkSignatureFromFunction(original, 1);
  } else if (
      Py_TYPE(original) == &_PyType_CachedPropertyThunk ||
      Py_TYPE(original) == &_PyTypedDescriptorWithDefaultValue_Type ||
      Py_TYPE(original) == &_PyType_AsyncCachedPropertyThunk ||
      Py_TYPE(original) == &PyCachedPropertyWithDescr_Type ||
      Py_TYPE(original) == &PyAsyncCachedPropertyWithDescr_Type ||
      Py_TYPE(original) == &PyProperty_Type) {
    return &simple_sigs[1];
  } else if (Py_TYPE(original) == &_PyType_TypedDescriptorThunk) {
    // TODO: Test setter case?
    if (((_Py_TypedDescriptorThunk*)original)->type == THUNK_SETTER) {
      return &simple_sigs[2];
    } else {
      return &simple_sigs[1];
    }
  } else if (Py_TYPE(original) == &PyMethodDescr_Type) {
    PyMethodDescrObject* descr = (PyMethodDescrObject*)original;
    if (descr->d_method->ml_flags == METH_NOARGS) {
      return &simple_sigs[0];
    } else if (descr->d_method->ml_flags == METH_O) {
      return &simple_sigs[1];
    }
  } else if (Py_TYPE(original) == &PyCFunction_Type) {
    PyCFunctionObject* func = (PyCFunctionObject*)original;
    if (func->m_ml->ml_flags == Ci_METH_TYPED) {
      Ci_PyTypedMethodDef* def = (Ci_PyTypedMethodDef*)func->m_ml->ml_meth;
      const Ci_Py_SigElement* const* sig = def->tmd_sig;
      Py_ssize_t argcnt = 0;
      while (*sig != NULL) {
        argcnt++;
        sig++;
      }
      switch (argcnt) {
        case 0:
          return &simple_sigs[0];
        case 1:
          return &simple_sigs[1];
        case 2:
          return &simple_sigs[2];
      }
    } else if (func->m_ml->ml_flags == METH_NOARGS) {
      return &simple_sigs[0];
    } else if (func->m_ml->ml_flags == METH_O) {
      return &simple_sigs[1];
    } else {
      // This is a hack, ultimately we shouldn't be using code for this and
      // instead should be getting typed arg info in a function-independent
      // way. Right now we only have one function which is METHOD_VARARGS
      // and we know it takes 2.
      assert(strcmp(func->m_ml->ml_name, "_property_missing_fset") == 0);
      return &simple_sigs[2];
    }
  }

  return NULL;
}

static _PyClassLoader_StaticCallReturn return_to_native(
    PyObject* val,
    PyTypeObject* ret_type) {
  _PyClassLoader_StaticCallReturn ret;
  int type_code =
      ret_type != NULL ? _PyClassLoader_GetTypeCode(ret_type) : TYPED_OBJECT;
  if (val != NULL && type_code != TYPED_OBJECT) {
    ret.rax = (void*)_PyClassLoader_Unbox(val, type_code);
  } else {
    ret.rax = (void*)(uint64_t)val;
  }
  ret.rdx = (void*)(uint64_t)(val != NULL);
  return ret;
}

static _PyClassLoader_StaticCallReturn return_to_native_typecode(
    PyObject* val,
    int type_code) {
  _PyClassLoader_StaticCallReturn ret;
  if (val != NULL && type_code != TYPED_OBJECT) {
    ret.rax = (void*)_PyClassLoader_Unbox(val, type_code);
  } else {
    ret.rax = (void*)val;
  }
  ret.rdx = (void*)(uint64_t)(val != NULL);
  return ret;
}

int _PyClassLoader_HydrateArgs(
    PyCodeObject* code,
    Py_ssize_t arg_count,
    void** args,
    PyObject** call_args,
    PyObject** free_args) {
  _PyTypedArgsInfo* typed_arg_info = _PyClassLoader_GetTypedArgsInfo(code, 1);
  PyObject** extra_args = (PyObject**)args[5];
  for (Py_ssize_t i = 0, cur_arg = 0; i < arg_count; i++) {
    void* original;
    if (i < 5) {
      original = args[i]; // skip the v-table state
    } else {
      original = extra_args[i - 3];
    }

    if (cur_arg < Py_SIZE(typed_arg_info) &&
        typed_arg_info->tai_args[cur_arg].tai_argnum == i) {
      call_args[i] = _PyClassLoader_Box(
          (uint64_t)original,
          typed_arg_info->tai_args[cur_arg].tai_primitive_type);
      if (call_args[i] == NULL) {
        for (Py_ssize_t free_arg = 0; free_arg < i; free_arg++) {
          Py_CLEAR(free_args[free_arg]);
        }
        return -1;
      }
      free_args[i] = call_args[i];
      cur_arg++;
    } else {
      free_args[i] = NULL;
      call_args[i] = (PyObject*)original;
    }
  }
  return 0;
}

int _PyClassLoader_HydrateArgsFromSig(
    _PyClassLoader_ThunkSignature* sig,
    Py_ssize_t arg_count,
    void** args,
    PyObject** call_args,
    PyObject** free_args) {
  PyObject** extra_args = (PyObject**)args[5];
  for (Py_ssize_t i = 0; i < arg_count; i++) {
    void* original;
    if (i < 5) {
      original = args[i]; // skip the v-table state
    } else {
      // The original args came in on the stack, so we have to skip the frame
      // pointer, the return address and then add one more.
      original = extra_args[i - 3];
    }

    if (sig->ta_has_primitives && sig->ta_argtype[i] != TYPED_OBJECT) {
      call_args[i] = _PyClassLoader_Box((uint64_t)original, sig->ta_argtype[i]);
      if (call_args[i] == NULL) {
        for (Py_ssize_t free_arg = 0; free_arg < i; free_arg++) {
          Py_CLEAR(free_args[free_arg]);
        }
        return -1;
      }
      free_args[i] = call_args[i];
    } else {
      free_args[i] = NULL;
      call_args[i] = (PyObject*)original;
    }
  }
  return 0;
}

void _PyClassLoader_FreeHydratedArgs(
    PyObject** free_args,
    Py_ssize_t arg_count) {
  for (Py_ssize_t i = 0; i < arg_count; i++) {
    Py_XDECREF(free_args[i]);
  }
}

_PyClassLoader_StaticCallReturn
invoke_from_native(PyObject* original, PyObject* func, void** args) {
  PyCodeObject* code = (PyCodeObject*)((PyFunctionObject*)original)->func_code;
  Py_ssize_t arg_count = code->co_argcount;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) <
      0) {
    return StaticError;
  }

  PyObject* res =
      ((PyFunctionObject*)func)->vectorcall(func, call_args, arg_count, NULL);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);

  int optional, exact, func_flags;
  PyTypeObject* type = (PyTypeObject*)_PyClassLoader_ResolveReturnType(
      original, &optional, &exact, &func_flags);
  return return_to_native(res, type);
}

PyObject* _PyVTable_coroutine_property_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* self = args[0];
  PyObject* descr = state->tcs_value;
  PyObject* name = state->tcs_rt.rt_name;
  PyObject* coro;
  int eager;

  /* we have to perform the descriptor checks at runtime because the
   * descriptor type can be modified preventing us from being able to have
   * more optimized fast paths */
  if (!PyDescr_IsData(descr)) {
    PyObject** dictptr = _PyObject_GetDictPtr(self);
    if (dictptr != NULL) {
      PyObject* dict = *dictptr;
      if (dict != NULL) {
        coro = PyDict_GetItem(dict, PyTuple_GET_ITEM(name, 0));
        if (coro != NULL) {
          Py_INCREF(coro);
          eager = 0;
          goto done;
        }
      }
    }
  }

  if (Py_TYPE(descr)->tp_descr_get != NULL) {
    PyObject* self = args[0];
    PyObject* get =
        Py_TYPE(descr)->tp_descr_get(descr, self, (PyObject*)Py_TYPE(self));
    if (get == NULL) {
      return NULL;
    }

    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);

    coro = _PyObject_Vectorcall(get, args + 1, (nargs - 1), NULL);
    Py_DECREF(get);
  } else {
    coro = _PyObject_Vectorcall(descr, args, nargsf, NULL);
  }

  if (coro == NULL) {
    return NULL;
  }

#if PY_VERSION_HEX < 0x030C0000
  eager = Ci_PyWaitHandle_CheckExact(coro);
  if (eager) {
    Ci_PyWaitHandleObject* handle = (Ci_PyWaitHandleObject*)coro;
    if (handle->wh_waiter == NULL) {
      if (_PyClassLoader_CheckReturnType(
              Py_TYPE(descr),
              handle->wh_coro_or_result,
              (_PyClassLoader_RetTypeInfo*)state)) {
        return coro;
      }
      Ci_PyWaitHandle_Release(coro);
      return NULL;
    }
  }
#else
  eager = 0;
#endif
done:
  return _PyClassLoader_NewAwaitableWrapper(
      coro, eager, (PyObject*)state, _PyClassLoader_CheckReturnCallback, NULL);
}

__attribute__((__used__)) PyObject* _PyVTable_classmethod_vectorcall(
    PyObject* state,
    PyObject* const* args,
    Py_ssize_t nargsf);

static bool try_call_instance_coroutine(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf,
    PyObject** res) {
  PyObject* self = args[0];
  PyObject** dictptr = _PyObject_GetDictPtr(self);
  PyObject* dict = dictptr != NULL ? *dictptr : NULL;
  if (dict == NULL) {
    return false;
  }

  PyObject* name = state->tcs_rt.rt_name;
  PyObject* callable = PyDict_GetItem(dict, name);
  if (callable == NULL) {
    return false;
  }

  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  *res = _PyClassLoader_CallCoroutineOverridden(
      state, callable, args + 1, (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET);
  return true;
}

PyObject* _PyVTable_coroutine_classmethod_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* callable = PyTuple_GET_ITEM(state->tcs_value, 0);
  PyObject* coro;
#if PY_VERSION_HEX < 0x030C0000
  Py_ssize_t awaited = nargsf & Ci_Py_AWAITED_CALL_MARKER;
#else
  Py_ssize_t awaited = 0;
#endif

  PyObject* res = NULL;
  PyTypeObject* decltype = (PyTypeObject*)PyTuple_GET_ITEM(state->tcs_value, 1);
  if (PyObject_TypeCheck(args[0], decltype) &&
      try_call_instance_coroutine(state, args, nargsf, &res)) {
    return res;
  }

  if (Py_TYPE(callable) == &PyClassMethod_Type) {
    // We need to do some special set up for class methods when invoking.
    coro = _PyVTable_classmethod_vectorcall(state->tcs_value, args, nargsf);
  } else if (Py_TYPE(callable)->tp_descr_get != NULL) {
    PyObject* self = args[0];
    PyObject* get = Py_TYPE(callable)->tp_descr_get(
        callable, self, (PyObject*)Py_TYPE(self));
    if (get == NULL) {
      return NULL;
    }

    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);

    coro = _PyObject_Vectorcall(get, args + 1, (nargs - 1), NULL);
    Py_DECREF(get);
  } else {
    // In this case, we have a patched class method, and the self has been
    // handled via descriptors already.
    coro = _PyObject_Vectorcall(
        callable,
        args + 1,
        (PyVectorcall_NARGS(nargsf) - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET |
            awaited,
        NULL);
  }

  if (coro == NULL) {
    return NULL;
  }

#if PY_VERSION_HEX < 0x030C0000
  int eager = Ci_PyWaitHandle_CheckExact(coro);
  if (eager) {
    Ci_PyWaitHandleObject* handle = (Ci_PyWaitHandleObject*)coro;
    if (handle->wh_waiter == NULL) {
      if (_PyClassLoader_CheckReturnType(
              Py_TYPE(callable),
              handle->wh_coro_or_result,
              (_PyClassLoader_RetTypeInfo*)state)) {
        return coro;
      }
      Ci_PyWaitHandle_Release(coro);
      return NULL;
    }
  }
#else
  int eager = 0;
#endif

  return _PyClassLoader_NewAwaitableWrapper(
      coro, eager, (PyObject*)state, _PyClassLoader_CheckReturnCallback, NULL);
}

PyObject* _PyVTable_coroutine_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* res;
  if (try_call_instance_coroutine(state, args, nargsf, &res)) {
    return res;
  }

  return _PyClassLoader_CallCoroutine(state, args, nargsf);
}

PyObject* _PyVTable_nonfunc_property_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    size_t nargsf) {
  PyObject* self = args[0];
  PyObject* descr = state->tcs_value;
  PyObject* name = state->tcs_rt.rt_name;
  PyObject* res;

  /* we have to perform the descriptor checks at runtime because the
   * descriptor type can be modified preventing us from being able to have
   * more optimized fast paths */
  if (!PyDescr_IsData(descr)) {
    PyObject** dictptr = _PyObject_GetDictPtr(self);
    if (dictptr != NULL) {
      PyObject* dict = *dictptr;
      if (dict != NULL) {
        res = PyDict_GetItem(dict, PyTuple_GET_ITEM(name, 0));
        if (res != NULL) {
          Py_INCREF(res);
          goto done;
        }
      }
    }
  }

  if (Py_TYPE(descr)->tp_descr_get != NULL) {
    PyObject* self = args[0];
    PyObject* get =
        Py_TYPE(descr)->tp_descr_get(descr, self, (PyObject*)Py_TYPE(self));
    if (get == NULL) {
      return NULL;
    }

    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);

    res = _PyObject_Vectorcall(
        get, args + 1, (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
    Py_DECREF(get);
    goto done;
  }
  res = _PyObject_Vectorcall(descr, args, nargsf, NULL);
done:
  return _PyClassLoader_CheckReturnType(
      Py_TYPE(self), res, (_PyClassLoader_RetTypeInfo*)state);
}

PyObject* _PyVTable_nonfunc_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    size_t nargsf) {
  PyObject* self = args[0];
  PyObject* descr = state->tcs_value;
  PyObject* name = state->tcs_rt.rt_name;
  PyObject* res;
  /* we have to perform the descriptor checks at runtime because the
   * descriptor type can be modified preventing us from being able to have
   * more optimized fast paths */
  if (!PyDescr_IsData(descr)) {
    PyObject** dictptr = _PyObject_GetDictPtr(self);
    if (dictptr != NULL) {
      PyObject* dict = *dictptr;
      if (dict != NULL) {
        PyObject* value = PyDict_GetItem(dict, name);
        if (value != NULL) {
          /* descriptor was overridden by instance value */
          Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
          res = _PyObject_Vectorcall(value, args + 1, nargs - 1, NULL);
          goto done;
        }
      }
    }
  }

  if (Py_TYPE(descr)->tp_descr_get != NULL) {
    PyObject* self = args[0];
    PyObject* get =
        Py_TYPE(descr)->tp_descr_get(descr, self, (PyObject*)Py_TYPE(self));
    if (get == NULL) {
      return NULL;
    }

    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);

    res = _PyObject_Vectorcall(
        get, args + 1, (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
    Py_DECREF(get);
    goto done;
  }
  res = _PyObject_Vectorcall(descr, args + 1, nargsf - 1, NULL);
done:
  return _PyClassLoader_CheckReturnType(
      Py_TYPE(self), res, (_PyClassLoader_RetTypeInfo*)state);
}

__attribute__((__used__)) PyObject*
_PyVTable_descr_vectorcall(PyObject* descr, PyObject** args, size_t nargsf) {
  return _PyObject_Vectorcall(descr, args, nargsf, NULL);
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_descr_native(PyObject* descr, void** args) {
  _PyClassLoader_StaticCallReturn res;
  res.rax = _PyVTable_descr_vectorcall(descr, (PyObject**)args, 1);
  res.rdx = (void*)(uint64_t)(res.rax != NULL);
  return res;
}

VTABLE_THUNK(_PyVTable_descr, PyObject)

__attribute__((__used__)) PyObject* vtable_static_function_vectorcall(
    PyObject* state,
    PyObject** args,
    Py_ssize_t nargsf) {
  return Ci_PyFunction_CallStatic((PyFunctionObject*)state, args, nargsf, NULL);
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
vtable_static_function_native(PyObject* state, void** args) {
  return invoke_from_native(state, state, args);
}

VTABLE_THUNK(vtable_static_function, PyObject)

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_thunk_ret_primitive_not_jitted_native(PyObject* state, void** args) {
  PyFunctionObject* func = (PyFunctionObject*)PyTuple_GET_ITEM(state, 0);
  PyTypeObject* ret_type = (PyTypeObject*)PyTuple_GET_ITEM(state, 1);
  Py_ssize_t arg_count = ((PyCodeObject*)func->func_code)->co_argcount;

  _PyClassLoader_StaticCallReturn res;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(
          (PyCodeObject*)func->func_code,
          arg_count,
          args,
          call_args,
          free_args)) {
    res.rax = NULL;
    res.rdx = NULL;
    return res;
  }

  PyObject* obj = func->vectorcall((PyObject*)func, call_args, arg_count, NULL);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);
  if (obj != NULL) {
    res.rax =
        (void*)_PyClassLoader_Unbox(obj, _PyClassLoader_GetTypeCode(ret_type));
  } else {
    res.rax = NULL;
  }
  res.rdx = (void*)(uint64_t)(obj != NULL);
  return res;
}

__attribute__((__used__)) PyObject*
_PyVTable_thunk_ret_primitive_not_jitted_vectorcall(
    PyObject* state,
    PyObject** args,
    Py_ssize_t nargsf) {
  PyFunctionObject* func = (PyFunctionObject*)PyTuple_GET_ITEM(state, 0);

  return func->vectorcall((PyObject*)func, args, nargsf, NULL);
}

VTABLE_THUNK(_PyVTable_thunk_ret_primitive_not_jitted, PyObject)

__attribute__((__used__)) void* _PyVTable_thunk_vectorcall_only_vectorcall(
    PyObject* state,
    PyObject** args,
    Py_ssize_t nargsf) {
  return PyObject_Vectorcall(state, args, nargsf, NULL);
}

__attribute__((__used__)) void* _PyVTable_thunk_vectorcall_only_native(
    PyObject* state,
    void** args) {
  PyErr_SetString(PyExc_RuntimeError, "unsupported native call");
  return NULL;
}

VTABLE_THUNK(_PyVTable_thunk_vectorcall_only, PyObject)

PyObject* _PyVTable_func_overridable_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* self = args[0];
  PyObject** dictptr = _PyObject_GetDictPtr(self);
  PyObject* dict = dictptr != NULL ? *dictptr : NULL;
  PyObject* res;
  if (dict != NULL) {
    /* ideally types using INVOKE_METHOD are defined w/o out dictionaries,
     * which allows us to avoid this lookup.  If they're not then we'll
     * fallback to supporting looking in the dictionary */
    PyObject* name = state->tcs_rt.rt_name;
    PyObject* callable = PyDict_GetItem(dict, name);
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (callable != NULL) {
      res = _PyObject_Vectorcall(
          callable,
          args + 1,
          (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET,
          NULL);
      goto done;
    }
  }

  res = _PyObject_Vectorcall(state->tcs_value, (PyObject**)args, nargsf, NULL);

done:
  return _PyClassLoader_CheckReturnType(
      Py_TYPE(self), res, (_PyClassLoader_RetTypeInfo*)state);
}

/**
    This vectorcall entrypoint pulls out the function, slot index and replaces
    its own entrypoint in the v-table with optimized static vectorcall. (It
    also calls the underlying function and returns the value while doing so).
*/
PyObject* _PyVTable_func_lazyinit_vectorcall(
    _PyClassLoader_LazyFuncJitThunk* state,
    PyObject** args,
    Py_ssize_t nargsf) {
  PyFunctionObject* func = (PyFunctionObject*)state->lf_func;

  PyObject* res =
      func->vectorcall((PyObject*)func, (PyObject**)args, nargsf, NULL);

  _PyType_VTable* vtable = (_PyType_VTable*)state->lf_vtable;
  long index = state->lf_slot;

  // Update to the compiled function once the JIT has kicked in.
  if (vtable->vt_entries[index].vte_state == (PyObject*)state &&
      isJitCompiled(func)) {
    vtable->vt_entries[index].vte_state = (PyObject*)func;
    vtable->vt_entries[index].vte_entry =
        _PyClassLoader_GetStaticFunctionEntry(func);
    Py_INCREF(func);
    Py_DECREF(state);
  }
  return res;
}

__attribute__((__used__)) PyObject* _PyVTable_staticmethod_vectorcall(
    PyObject* method,
    PyObject** args,
    Py_ssize_t nargsf) {
  PyObject* func = Ci_PyStaticMethod_GetFunc(method);
  return _PyObject_Vectorcall(func, ((PyObject**)args) + 1, nargsf - 1, NULL);
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_staticmethod_native(PyObject* method, void** args) {
  PyObject* func = Ci_PyStaticMethod_GetFunc(method);
  PyCodeObject* code = (PyCodeObject*)((PyFunctionObject*)func)->func_code;
  Py_ssize_t arg_count =
      code->co_argcount + 1; // hydrate self and then we'll drop it
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) <
      0) {
    return StaticError;
  }

  PyObject* res =
      _PyVTable_staticmethod_vectorcall(method, call_args, arg_count);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);
  int optional, exact, func_flags;
  PyTypeObject* type = (PyTypeObject*)_PyClassLoader_ResolveReturnType(
      (PyObject*)func, &optional, &exact, &func_flags);
  return return_to_native(res, type);
}

VTABLE_THUNK(_PyVTable_staticmethod, PyObject)

__attribute__((__used__)) PyObject* _PyVTable_classmethod_vectorcall(
    PyObject* state,
    PyObject* const* args,
    Py_ssize_t nargsf) {
  PyObject* classmethod = PyTuple_GET_ITEM(state, 0);
  PyTypeObject* decltype = (PyTypeObject*)PyTuple_GET_ITEM(state, 1);
  PyObject* func = Ci_PyClassMethod_GetFunc(classmethod);
  if (!PyObject_TypeCheck(args[0], decltype)) {
    return _PyObject_Vectorcall(func, args, nargsf, NULL);
  }

  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  PyObject* stack[nargs];
  stack[0] = (PyObject*)Py_TYPE(args[0]);
  for (Py_ssize_t i = 1; i < nargs; i++) {
    stack[i] = args[i];
  }
  return _PyObject_Vectorcall(func, stack, nargsf, NULL);
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_classmethod_native(PyObject* state, void** args) {
  PyObject* classmethod = PyTuple_GET_ITEM(state, 0);
  PyTypeObject* decltype = (PyTypeObject*)PyTuple_GET_ITEM(state, 1);
  PyObject* func = Ci_PyClassMethod_GetFunc(classmethod);
  PyCodeObject* code = (PyCodeObject*)((PyFunctionObject*)func)->func_code;
  Py_ssize_t arg_count = code->co_argcount;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) <
      0) {
    return StaticError;
  }
  if (PyObject_TypeCheck(call_args[0], decltype)) {
    call_args[0] = (PyObject*)Py_TYPE(call_args[0]);
  }

  PyObject* res =
      ((PyFunctionObject*)func)->vectorcall(func, call_args, arg_count, NULL);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);

  int optional, exact, func_flags;
  PyTypeObject* type = (PyTypeObject*)_PyClassLoader_ResolveReturnType(
      func, &optional, &exact, &func_flags);
  return return_to_native(res, type);
}

VTABLE_THUNK(_PyVTable_classmethod, PyObject)

PyObject* _PyVTable_classmethod_overridable_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    size_t nargsf) {
  PyObject* clsmethod = PyTuple_GET_ITEM(state->tcs_value, 0);
  if (_PyClassMethod_Check(clsmethod)) {
    return _PyVTable_classmethod_vectorcall(state->tcs_value, args, nargsf);
  }
  // Invoked via an instance, we need to check its dict to see if the
  // classmethod was overridden.
  PyObject* self = args[0];
  PyObject** dictptr = _PyObject_GetDictPtr(self);
  PyObject* dict = dictptr != NULL ? *dictptr : NULL;
  PyObject* res;
  if (dict != NULL) {
    /* ideally types using INVOKE_METHOD are defined w/o out dictionaries,
     * which allows us to avoid this lookup.  If they're not then we'll
     * fallback to supporting looking in the dictionary */
    PyObject* name = state->tcs_rt.rt_name;
    PyObject* callable = PyDict_GetItem(dict, name);
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (callable != NULL) {
      res = _PyObject_Vectorcall(
          callable,
          args + 1,
          (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET,
          NULL);
      return _PyClassLoader_CheckReturnType(
          Py_TYPE(self), res, (_PyClassLoader_RetTypeInfo*)state);
    }
  }

  return _PyObject_Vectorcall(clsmethod, args, nargsf, NULL);
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_func_missing_native(PyObject* state, void** args) {
  PyFunctionObject* original = (PyFunctionObject*)PyTuple_GET_ITEM(state, 3);
  PyCodeObject* code = (PyCodeObject*)original->func_code;
  PyObject* call_args[code->co_argcount];
  PyObject* free_args[code->co_argcount];

  if (_PyClassLoader_HydrateArgs(
          code, code->co_argcount, args, call_args, free_args)) {
    return StaticError;
  }

  PyObject* self = call_args[0];
  PyObject* name = PyTuple_GET_ITEM(state, 0);
  PyErr_Format(
      PyExc_AttributeError,
      "'%s' object has no attribute %R",
      Py_TYPE(self)->tp_name,
      name);
  _PyClassLoader_FreeHydratedArgs(free_args, code->co_argcount);
  return StaticError;
}

__attribute__((__used__)) void* _PyVTable_func_missing_vectorcall(
    PyObject* state,
    PyObject** args,
    Py_ssize_t nargsf) {
  PyObject* self = args[0];
  PyObject* name = PyTuple_GET_ITEM(state, 0);
  PyErr_Format(
      PyExc_AttributeError,
      "'%s' object has no attribute %R",
      Py_TYPE(self)->tp_name,
      name);
  return NULL;
}

VTABLE_THUNK(_PyVTable_func_missing, PyObject)

__attribute__((__used__)) PyObject* _PyVTable_thunk_vectorcall(
    _PyClassLoader_MethodThunk* thunk,
    PyObject* const* args,
    size_t nargsf) {
  return thunk->mt_call((PyObject*)thunk, args, nargsf, NULL);
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_thunk_native(_PyClassLoader_MethodThunk* thunk, void** args) {
  assert(
      PyObject_IsInstance((PyObject*)thunk, (PyObject*)&_PyType_MethodThunk));
  _PyClassLoader_ThunkSignature* sig = thunk->mt_sig;
  Py_ssize_t arg_count = sig->ta_argcount;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgsFromSig(
          sig, arg_count, args, call_args, free_args) < 0) {
    return StaticError;
  }
  PyObject* obj = thunk->mt_call((PyObject*)thunk, call_args, arg_count, NULL);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);

  return return_to_native_typecode(obj, sig->ta_rettype);
}

VTABLE_THUNK(_PyVTable_thunk, _PyClassLoader_MethodThunk)

static inline int is_static_entry(vectorcallfunc func) {
  return func == (vectorcallfunc)Ci_StaticFunction_Vectorcall;
}

vectorcallfunc _PyClassLoader_GetStaticFunctionEntry(PyFunctionObject* func) {
  assert(_PyClassLoader_IsStaticFunction((PyObject*)func));
  if (is_static_entry(func->vectorcall)) {
    /* this will always be invoked statically via the v-table */
    return (vectorcallfunc)vtable_static_function_dont_bolt;
  }
  assert(isJitCompiled(func));
  return JITRT_GET_STATIC_ENTRY(func->vectorcall);
}
