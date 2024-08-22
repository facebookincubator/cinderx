/* Copyright (c) Meta Platforms, Inc. and affiliates. */
#include "cinderx/StaticPython/vtable_defs.h"

#include "cinderx/CachedProperties/cached_properties.h"
#include "cinderx/Common/func.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/StaticPython/awaitable.h"
#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/functype.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/type.h"
#include "cinderx/StaticPython/typed-args-info.h"
#include "cinderx/StaticPython/typed_method_def.h"
#include "cinderx/StaticPython/vtable.h"
#include "cinderx/Upgrade/upgrade_stubs.h"

#include "cinderx/Jit/compile.h"
#include "cinderx/Jit/entry.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif

#include <Python.h>

#define _PyClassMethod_Check(op) (Py_TYPE(op) == &PyClassMethod_Type)

Py_ssize_t _PyClassLoader_GetExpectedArgCount(PyObject** callable) {
  Py_ssize_t arg_count;
  PyObject* original = *callable;
  if (!PyFunction_Check(original)) {
    if (_PyClassMethod_Check(original)) {
      *callable = Ci_PyClassMethod_GetFunc(original);
      if (!PyFunction_Check(*callable)) {
        PyErr_SetString(PyExc_RuntimeError, "Not a function in a class method");
        return -1;
      }
      arg_count = ((PyCodeObject*)((PyFunctionObject*)*callable)->func_code)
                      ->co_argcount;
    } else if (Py_TYPE(original) == &PyStaticMethod_Type) {
      *callable = Ci_PyStaticMethod_GetFunc(original);
      if (!PyFunction_Check(*callable)) {
        PyErr_SetString(PyExc_RuntimeError, "Not a function in a class method");
        return -1;
      }
      // static method doesn't take self, but it's passed as an argument in an
      // INVOKE_METHOD.
      arg_count = ((PyCodeObject*)((PyFunctionObject*)*callable)->func_code)
                      ->co_argcount +
          1;
    } else if (Py_TYPE(original) == &_PyType_CachedPropertyThunk) {
      arg_count = 1;
      *callable =
          ((PyCachedPropertyDescrObject*)((_Py_CachedPropertyThunk*)original)
               ->propthunk_target)
              ->func;
    } else if (Py_TYPE(original) == &_PyType_TypedDescriptorThunk) {
      // TODO: Test setter case?
      if (((_Py_TypedDescriptorThunk*)original)->is_setter) {
        arg_count = 2;
      } else {
        arg_count = 1;
      }
      *callable =
          ((_Py_TypedDescriptorThunk*)original)->typed_descriptor_thunk_target;
    } else {
      PyErr_Format(PyExc_RuntimeError, "Not a function: %R", original);
      return -1;
    }
  } else {
    arg_count =
        ((PyCodeObject*)((PyFunctionObject*)*callable)->func_code)->co_argcount;
  }
  return arg_count;
}

static _PyClassLoader_StaticCallReturn return_to_native(
    PyObject* val,
    PyTypeObject* ret_type) {
  _PyClassLoader_StaticCallReturn ret;
  int type_code = _PyClassLoader_GetTypeCode(ret_type);
  if (val != NULL && type_code != TYPED_OBJECT) {
    ret.rax = (void*)_PyClassLoader_Unbox(val, type_code);
  } else {
    ret.rax = (void*)(uint64_t)val;
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

__attribute__((__used__)) PyObject* _PyVTable_coroutine_property_vectorcall(
    _PyClassLoader_TypeCheckState* state,
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
done:
  return _PyClassLoader_NewAwaitableWrapper(
      coro, eager, (PyObject*)state, _PyClassLoader_CheckReturnCallback, NULL);
}

__attribute__((__used__)) PyObject* _PyVTable_classmethod_vectorcall(
    PyObject* state,
    PyObject* const* args,
    Py_ssize_t nargsf);

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_coroutine_property_native(
    _PyClassLoader_TypeCheckState* state,
    void** args) {
  PyFunctionObject* original =
      (PyFunctionObject*)state->tcs_rt.rt_base.mt_original;

  PyCodeObject* code = (PyCodeObject*)original->func_code;
  Py_ssize_t arg_count = code->co_argcount;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) <
      0) {
    return StaticError;
  }
  _PyClassLoader_StaticCallReturn res;
  res.rax =
      _PyVTable_coroutine_property_vectorcall(state, call_args, arg_count);
  res.rdx = (void*)(uint64_t)(res.rax != NULL);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);

  return res;
}

VTABLE_THUNK(_PyVTable_coroutine_property, _PyClassLoader_TypeCheckState)

__attribute__((__used__)) PyObject* _PyVTable_coroutine_classmethod_vectorcall(
    _PyClassLoader_TypeCheckState* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* callable = PyTuple_GET_ITEM(state->tcs_value, 0);
  PyObject* coro;
#if PY_VERSION_HEX < 0x030C0000
  Py_ssize_t awaited = nargsf & Ci_Py_AWAITED_CALL_MARKER;
#else
  UPGRADE_ASSERT(AWAITED_FLAG)
  Py_ssize_t awaited = 0;
#endif

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

  return _PyClassLoader_NewAwaitableWrapper(
      coro, eager, (PyObject*)state, _PyClassLoader_CheckReturnCallback, NULL);
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_coroutine_classmethod_native(
    _PyClassLoader_TypeCheckState* state,
    void** args,
    Py_ssize_t nargsf) {
  PyObject* original = state->tcs_rt.rt_base.mt_original;
  assert(Py_TYPE(original) == &PyClassMethod_Type);
  PyFunctionObject* callable =
      (PyFunctionObject*)Ci_PyClassMethod_GetFunc(original);

  PyCodeObject* code = (PyCodeObject*)callable->func_code;
  Py_ssize_t arg_count = code->co_argcount;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) <
      0) {
    return StaticError;
  }

  int optional, exact, func_flags;
  PyTypeObject* type = (PyTypeObject*)_PyClassLoader_ResolveReturnType(
      (PyObject*)callable, &optional, &exact, &func_flags);

  _PyClassLoader_StaticCallReturn res = return_to_native(
      _PyVTable_coroutine_classmethod_vectorcall(state, call_args, arg_count),
      type);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);
  return res;
}

VTABLE_THUNK(_PyVTable_coroutine_classmethod, _PyClassLoader_TypeCheckState)

__attribute__((__used__)) PyObject* _PyVTable_coroutine_vectorcall(
    _PyClassLoader_TypeCheckState* state,
    PyObject* const* args,
    size_t nargsf) {
  return _PyClassLoader_CallCoroutine(state, args, nargsf);
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_coroutine_native(_PyClassLoader_TypeCheckState* state, void** args) {
  PyFunctionObject* original =
      (PyFunctionObject*)state->tcs_rt.rt_base.mt_original;

  PyCodeObject* code = (PyCodeObject*)original->func_code;
  Py_ssize_t arg_count = code->co_argcount;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) <
      0) {
    return StaticError;
  }
  _PyClassLoader_StaticCallReturn res;
  res.rax = _PyClassLoader_CallCoroutine(state, call_args, arg_count);
  res.rdx = (void*)(uint64_t)(res.rax != NULL);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);
  return res;
}

VTABLE_THUNK(_PyVTable_coroutine, _PyClassLoader_TypeCheckState)

__attribute__((__used__)) PyObject* _PyVTable_nonfunc_property_vectorcall(
    _PyClassLoader_TypeCheckState* state,
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

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_nonfunc_property_native(
    _PyClassLoader_TypeCheckState* state,
    void** args) {
  PyObject* original = state->tcs_rt.rt_base.mt_original;
  Py_ssize_t arg_count = _PyClassLoader_GetExpectedArgCount(&original);
  if (arg_count < 0) {
    return StaticError;
  }
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];
  // We can have a property-like object which doesn't have an original function,
  // for example a typed descriptor with a value.  TODO: Can one of those be a
  // primitive?
  if (PyFunction_Check(original)) {
    PyCodeObject* code =
        (PyCodeObject*)((PyFunctionObject*)original)->func_code;

    if (_PyClassLoader_HydrateArgs(
            code, arg_count, args, call_args, free_args) < 0) {
      return StaticError;
    }
  } else {
    for (Py_ssize_t i = 0; i < arg_count; i++) {
      call_args[i] = (PyObject*)args[i];
      free_args[i] = 0;
    }
  }
  PyObject* obj =
      _PyVTable_nonfunc_property_vectorcall(state, call_args, arg_count);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);
  return return_to_native(
      obj, ((_PyClassLoader_RetTypeInfo*)state)->rt_expected);
}

VTABLE_THUNK(_PyVTable_nonfunc_property, _PyClassLoader_TypeCheckState)

__attribute__((__used__)) PyObject* _PyVTable_nonfunc_vectorcall(
    _PyClassLoader_TypeCheckState* state,
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

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_nonfunc_native(_PyClassLoader_TypeCheckState* state, void** args) {
  PyObject* original = state->tcs_rt.rt_base.mt_original;
  Py_ssize_t arg_count = _PyClassLoader_GetExpectedArgCount(&original);
  if (arg_count < 0) {
    return StaticError;
  }

  PyCodeObject* code = (PyCodeObject*)((PyFunctionObject*)original)->func_code;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) <
      0) {
    return StaticError;
  }
  PyObject* obj = _PyVTable_nonfunc_vectorcall(state, call_args, arg_count);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);
  return return_to_native(
      obj, ((_PyClassLoader_RetTypeInfo*)state)->rt_expected);
}

VTABLE_THUNK(_PyVTable_nonfunc, _PyClassLoader_TypeCheckState)

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

__attribute__((__used__)) PyObject* _PyVTable_func_overridable_vectorcall(
    _PyClassLoader_TypeCheckState* state,
    PyObject** args,
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

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_func_overridable_native(
    _PyClassLoader_TypeCheckState* state,
    void** args) {
  PyFunctionObject* func =
      (PyFunctionObject*)((_PyClassLoader_MethodThunk*)state)->mt_original;
  Py_ssize_t arg_count = ((PyCodeObject*)func->func_code)->co_argcount;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(
          (PyCodeObject*)func->func_code,
          arg_count,
          args,
          call_args,
          free_args) < 0) {
    return StaticError;
  }

  PyObject* obj = _PyVTable_func_overridable_vectorcall(
      state, call_args, ((PyCodeObject*)func->func_code)->co_argcount);
  _PyClassLoader_FreeHydratedArgs(
      free_args, ((PyCodeObject*)func->func_code)->co_argcount);
  return return_to_native(
      obj, ((_PyClassLoader_RetTypeInfo*)state)->rt_expected);
}

VTABLE_THUNK(_PyVTable_func_overridable, _PyClassLoader_TypeCheckState)

/**
    This vectorcall entrypoint pulls out the function, slot index and replaces
    its own entrypoint in the v-table with optimized static vectorcall. (It also
    calls the underlying function and returns the value while doing so).
*/
__attribute__((__used__)) PyObject* _PyVTable_func_lazyinit_vectorcall(
    PyObject* state,
    PyObject** args,
    Py_ssize_t nargsf) {
  /* state is (vtable, index, function) */
  _PyType_VTable* vtable = (_PyType_VTable*)PyTuple_GET_ITEM(state, 0);
  long index = PyLong_AS_LONG(PyTuple_GET_ITEM(state, 1));
  PyFunctionObject* func = (PyFunctionObject*)PyTuple_GET_ITEM(state, 2);

  PyObject* res =
      func->vectorcall((PyObject*)func, (PyObject**)args, nargsf, NULL);

  // Update to the compiled function once the JIT has kicked in.
  if (vtable->vt_entries[index].vte_state == state &&
      !isJitEntryFunction(func->vectorcall)) {
    vtable->vt_entries[index].vte_state = (PyObject*)func;
    vtable->vt_entries[index].vte_entry =
        _PyClassLoader_GetStaticFunctionEntry(func);
    Py_INCREF(func);
    Py_DECREF(state);
  }

  return res;
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_func_lazyinit_native(PyObject* state, void** args) {
  PyFunctionObject* func = (PyFunctionObject*)PyTuple_GET_ITEM(state, 2);
  PyCodeObject* code = (PyCodeObject*)((PyFunctionObject*)func)->func_code;
  Py_ssize_t arg_count = code->co_argcount;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) <
      0) {
    return StaticError;
  }

  PyObject* res =
      _PyVTable_func_lazyinit_vectorcall(state, call_args, arg_count);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);
  int optional, exact, func_flags;
  PyTypeObject* type = (PyTypeObject*)_PyClassLoader_ResolveReturnType(
      (PyObject*)func, &optional, &exact, &func_flags);
  return return_to_native(res, type);
}

VTABLE_THUNK(_PyVTable_func_lazyinit, PyObject)

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

__attribute__((__used__)) PyObject*
_PyVTable_staticmethod_overridable_vectorcall(
    PyObject* thunk,
    PyObject** args,
    Py_ssize_t nargsf) {
  PyObject* method = ((_PyClassLoader_TypeCheckState*)thunk)->tcs_value;
  PyObject* func = Ci_PyStaticMethod_GetFunc(method);

  return _PyObject_Vectorcall(func, ((PyObject**)args) + 1, nargsf - 1, NULL);
}

__attribute__((__used__)) _PyClassLoader_StaticCallReturn
_PyVTable_staticmethod_overridable_native(PyObject* thunk, void** args) {
  PyObject* original = Ci_PyStaticMethod_GetFunc(
      ((_PyClassLoader_MethodThunk*)thunk)->mt_original);
  PyObject* method = ((_PyClassLoader_TypeCheckState*)thunk)->tcs_value;
  PyObject* func = Ci_PyStaticMethod_GetFunc(method);
  return invoke_from_native(original, func, args);
}

VTABLE_THUNK(_PyVTable_staticmethod_overridable, PyObject)

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

__attribute__((__used__)) PyObject*
_PyVTable_classmethod_overridable_vectorcall(
    _PyClassLoader_TypeCheckState* state,
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
_PyVTable_classmethod_overridable_native(
    _PyClassLoader_TypeCheckState* state,
    void** args) {
  PyObject* original = state->tcs_rt.rt_base.mt_original;
  PyFunctionObject* func =
      (PyFunctionObject*)Ci_PyClassMethod_GetFunc(original);

  PyCodeObject* code = (PyCodeObject*)func->func_code;
  Py_ssize_t arg_count = code->co_argcount;
  PyObject* call_args[arg_count];
  PyObject* free_args[arg_count];

  if (_PyClassLoader_HydrateArgs(code, arg_count, args, call_args, free_args) <
      0) {
    return StaticError;
  }
  PyObject* obj =
      _PyVTable_classmethod_overridable_vectorcall(state, call_args, arg_count);
  _PyClassLoader_FreeHydratedArgs(free_args, arg_count);
  return return_to_native(
      obj, ((_PyClassLoader_RetTypeInfo*)state)->rt_expected);
}

VTABLE_THUNK(_PyVTable_classmethod_overridable, _PyClassLoader_TypeCheckState)

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

static inline int is_static_entry(vectorcallfunc func) {
  return func == (vectorcallfunc)Ci_StaticFunction_Vectorcall;
}

vectorcallfunc _PyClassLoader_GetStaticFunctionEntry(PyFunctionObject* func) {
  assert(_PyClassLoader_IsStaticFunction((PyObject*)func));
  if (is_static_entry(func->vectorcall)) {
    /* this will always be invoked statically via the v-table */
    return (vectorcallfunc)vtable_static_function_dont_bolt;
  } else {
    assert(_PyJIT_IsCompiled(func));
    return JITRT_GET_STATIC_ENTRY(func->vectorcall);
  }
}
