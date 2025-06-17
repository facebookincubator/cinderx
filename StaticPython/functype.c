// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/StaticPython/functype.h"

#include "cinderx/CachedProperties/cached_properties.h"
#include "cinderx/Common/func.h"
#include "cinderx/Common/property.h"
#include "cinderx/StaticPython/awaitable.h"
#include "cinderx/StaticPython/descrs.h"
#include "cinderx/StaticPython/errors.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/type.h"
#include "cinderx/StaticPython/typed_method_def.h"
#if PY_VERSION_HEX < 0x030C0000
#include "cinder/hooks.h"
#endif

int _PyClassLoader_IsPropertyName(PyTupleObject* name) {
  if (PyTuple_GET_SIZE(name) != 2) {
    return 0;
  }
  PyObject* property_method_name = PyTuple_GET_ITEM(name, 1);
  if (!PyUnicode_Check(property_method_name)) {
    return 0;
  }
  return PyUnicode_CompareWithASCIIString(property_method_name, "fget") == 0 ||
      PyUnicode_CompareWithASCIIString(property_method_name, "fset") == 0 ||
      PyUnicode_CompareWithASCIIString(property_method_name, "fdel") == 0;
}

PyObject* _PyClassLoader_GetFunctionName(PyObject* name) {
  if (PyTuple_Check(name) &&
      _PyClassLoader_IsPropertyName((PyTupleObject*)name)) {
    return PyTuple_GET_ITEM(name, 0);
  }
  return name;
}

_PyTypedArgsInfo* _PyClassLoader_GetTypedArgsInfo(
    PyCodeObject* code,
    int only_primitives) {
  PyObject* checks = _PyClassLoader_GetCodeArgumentTypeDescrs(code);

  int count;
  if (only_primitives) {
    count = 0;
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(checks); i += 2) {
      PyObject* type_descr = PyTuple_GET_ITEM(checks, i + 1);
      if (_PyClassLoader_ResolvePrimitiveType(type_descr) != TYPED_OBJECT) {
        count++;
      }
    }
  } else {
    count = PyTuple_GET_SIZE(checks) / 2;
  }

  _PyTypedArgsInfo* arg_checks =
      PyObject_GC_NewVar(_PyTypedArgsInfo, &_PyTypedArgsInfo_Type, count);
  if (arg_checks == NULL) {
    return NULL;
  }

  int checki = 0;
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(checks); i += 2) {
    _PyTypedArgInfo* cur_check = &arg_checks->tai_args[checki];

    PyObject* type_descr = PyTuple_GET_ITEM(checks, i + 1);
    int optional, exact;
    PyTypeObject* ref_type =
        _PyClassLoader_ResolveType(type_descr, &optional, &exact);
    if (ref_type == NULL) {
      return NULL;
    }

    int prim_type = _PyClassLoader_GetTypeCode(ref_type);
    if (prim_type == TYPED_BOOL) {
      cur_check->tai_type = &PyBool_Type;
      cur_check->tai_optional = 0;
      cur_check->tai_exact = 1;
      Py_INCREF(&PyBool_Type);
      Py_DECREF(ref_type);
    } else if (prim_type == TYPED_DOUBLE) {
      cur_check->tai_type = &PyFloat_Type;
      cur_check->tai_optional = 0;
      cur_check->tai_exact = 0;
      Py_INCREF(&PyFloat_Type);
      Py_DECREF(ref_type);
    } else if (prim_type != TYPED_OBJECT) {
      assert(prim_type <= TYPED_INT64);
      cur_check->tai_type = &PyLong_Type;
      cur_check->tai_optional = 0;
      cur_check->tai_exact = 0;
      Py_INCREF(&PyLong_Type);
      Py_DECREF(ref_type);
    } else if (only_primitives) {
      Py_DECREF(ref_type);
      continue;
    } else {
      cur_check->tai_type = ref_type;
      cur_check->tai_optional = optional;
      cur_check->tai_exact = exact;
    }
    cur_check->tai_primitive_type = prim_type;
    cur_check->tai_argnum = PyLong_AsLong(PyTuple_GET_ITEM(checks, i));
    checki++;
  }
  return arg_checks;
}

static PyTypeObject* resolve_function_rettype(
    PyObject* funcobj,
    int* optional,
    int* exact,
    int* func_flags) {
  assert(PyFunction_Check(funcobj));
  PyFunctionObject* func = (PyFunctionObject*)funcobj;
  if (((PyCodeObject*)func->func_code)->co_flags & CO_COROUTINE) {
    *func_flags |= Ci_FUNC_FLAGS_COROUTINE;
  }
  return _PyClassLoader_ResolveType(
      _PyClassLoader_GetReturnTypeDescr(func), optional, exact);
}

static PyObject* classloader_get_static_type(const char* name) {
  PyObject* mod = PyImport_ImportModule("__static__");
  if (mod == NULL) {
    return NULL;
  }
  PyObject* type = PyObject_GetAttrString(mod, name);
  Py_DECREF(mod);
  return type;
}

PyObject* _PyClassLoader_ResolveReturnType(
    PyObject* func,
    int* optional,
    int* exact,
    int* func_flags) {
  *optional = *exact = *func_flags = 0;
  PyTypeObject* res = NULL;
  if (PyFunction_Check(func)) {
    if (_PyClassLoader_IsStaticFunction(func)) {
      res = resolve_function_rettype(func, optional, exact, func_flags);
    } else {
      res = &PyBaseObject_Type;
      Py_INCREF(res);
    }
  } else if (Py_TYPE(func) == &PyStaticMethod_Type) {
    PyObject* static_func = Ci_PyStaticMethod_GetFunc(func);
    if (_PyClassLoader_IsStaticFunction(static_func)) {
      res = resolve_function_rettype(static_func, optional, exact, func_flags);
    }
    *func_flags |= Ci_FUNC_FLAGS_STATICMETHOD;
  } else if (Py_TYPE(func) == &PyClassMethod_Type) {
    PyObject* static_func = Ci_PyClassMethod_GetFunc(func);
    if (_PyClassLoader_IsStaticFunction(static_func)) {
      res = resolve_function_rettype(static_func, optional, exact, func_flags);
    }
    *func_flags |= Ci_FUNC_FLAGS_CLASSMETHOD;
  } else if (Py_TYPE(func) == &PyProperty_Type) {
    Ci_propertyobject* property = (Ci_propertyobject*)func;
    PyObject* fget = property->prop_get;
    if (_PyClassLoader_IsStaticFunction(fget)) {
      res = resolve_function_rettype(fget, optional, exact, func_flags);
    }
  } else if (Py_TYPE(func) == &PyCachedPropertyWithDescr_Type) {
    PyCachedPropertyDescrObject* property = (PyCachedPropertyDescrObject*)func;
    if (_PyClassLoader_IsStaticFunction(property->func)) {
      res =
          resolve_function_rettype(property->func, optional, exact, func_flags);
    }
  } else if (Py_TYPE(func) == &PyAsyncCachedPropertyWithDescr_Type) {
    PyAsyncCachedPropertyDescrObject* property =
        (PyAsyncCachedPropertyDescrObject*)func;
    if (_PyClassLoader_IsStaticFunction(property->func)) {
      res =
          resolve_function_rettype(property->func, optional, exact, func_flags);
    }
  } else if (Py_TYPE(func) == &_PyType_PropertyThunk) {
    switch (_PyClassLoader_PropertyThunk_Kind(func)) {
      case THUNK_SETTER:
      case THUNK_DELETER: {
        res = Py_TYPE(Py_None);
        Py_INCREF(res);
        break;
      }
      case THUNK_GETTER: {
        PyObject* getter = _PyClassLoader_PropertyThunk_GetProperty(func);
        res = (PyTypeObject*)_PyClassLoader_ResolveReturnType(
            getter, optional, exact, func_flags);
        break;
      }
    }
  } else if (Py_TYPE(func) == &_PyTypedDescriptorWithDefaultValue_Type) {
    _PyTypedDescriptorWithDefaultValue* td =
        (_PyTypedDescriptorWithDefaultValue*)func;
    if (PyTuple_CheckExact(td->td_type)) {
      res = _PyClassLoader_ResolveType(
          td->td_type, &td->td_optional, &td->td_exact);
      *optional = td->td_optional;
      *exact = td->td_exact;
    } else { // Already resolved.
      assert(PyType_CheckExact(td->td_type));
      res = (PyTypeObject*)td->td_type;
      Py_INCREF(res);
      *optional = td->td_optional;
      *exact = td->td_exact;
    }
    if (res == NULL) {
      return NULL;
    }
  } else if (Py_TYPE(func) == &_PyType_StaticThunk) {
    _Py_StaticThunk* sthunk = (_Py_StaticThunk*)func;
    res = sthunk->thunk_tcs.tcs_rt.rt_expected;
    *optional = sthunk->thunk_tcs.tcs_rt.rt_optional;
    *exact = sthunk->thunk_tcs.tcs_rt.rt_exact;
    Py_INCREF(res);
  } else {
    Ci_PyTypedMethodDef* tmd = _PyClassLoader_GetTypedMethodDef(func);
    *optional = 0;
    if (tmd != NULL) {
      switch (tmd->tmd_ret) {
        case Ci_Py_SIG_VOID:
        case Ci_Py_SIG_ERROR: {
          // The underlying C implementations of these functions don't
          // produce a Python object at all, but we ensure (in
          // _PyClassLoader_ConvertRet and in JIT HIR builder) that
          // when we call them we produce a None.
          *exact = 0;
          res = Py_TYPE(Py_None);
          Py_INCREF(res);
          break;
        }
        case Ci_Py_SIG_STRING: {
          *exact = 0;
          res = &PyUnicode_Type;
          Py_INCREF(res);
          break;
        }
        case Ci_Py_SIG_INT8: {
          *exact = 1;
          return classloader_get_static_type("int8");
        }
        case Ci_Py_SIG_INT16: {
          *exact = 1;
          return classloader_get_static_type("int16");
        }
        case Ci_Py_SIG_INT32: {
          *exact = 1;
          return classloader_get_static_type("int32");
        }
        case Ci_Py_SIG_INT64: {
          *exact = 1;
          return classloader_get_static_type("int64");
        }
        case Ci_Py_SIG_UINT8: {
          *exact = 1;
          return classloader_get_static_type("uint8");
        }
        case Ci_Py_SIG_UINT16: {
          *exact = 1;
          return classloader_get_static_type("uint16");
        }
        case Ci_Py_SIG_UINT32: {
          *exact = 1;
          return classloader_get_static_type("uint32");
        }
        case Ci_Py_SIG_UINT64: {
          *exact = 1;
          return classloader_get_static_type("uint64");
        }
        default: {
          *exact = 0;
          res = &PyBaseObject_Type;
          Py_INCREF(res);
        }
      }
      Py_INCREF(res);
    } else if (
        Py_TYPE(func) == &PyMethodDescr_Type ||
        Py_TYPE(func) == &PyCFunction_Type) {
      // We emit invokes to untyped builtin methods; just assume they
      // return object.
      *exact = 0;
      res = &PyBaseObject_Type;
      Py_INCREF(res);
    }
  }
  return (PyObject*)res;
}

PyTypeObject* _PyClassLoader_ResolveCodeReturnType(
    PyCodeObject* code,
    int* optional,
    int* exact) {
  return _PyClassLoader_ResolveType(
      _PyClassLoader_GetCodeReturnTypeDescr(code), optional, exact);
}

PyObject* _PyClassLoader_GetReturnTypeDescr(PyFunctionObject* func) {
  return _PyClassLoader_GetCodeReturnTypeDescr((PyCodeObject*)func->func_code);
}

PyObject* _PyClassLoader_GetCodeReturnTypeDescr(PyCodeObject* code) {
  // last element of consts is ((arg_checks, ...), ret_type)
  PyObject* static_type_info =
      PyTuple_GET_ITEM(code->co_consts, PyTuple_GET_SIZE(code->co_consts) - 1);

  return PyTuple_GET_ITEM(static_type_info, 1);
}

PyObject* _PyClassLoader_GetCodeArgumentTypeDescrs(PyCodeObject* code) {
  // last element of consts is ((arg_checks, ...), ret_type)
  PyObject* static_type_info =
      PyTuple_GET_ITEM(code->co_consts, PyTuple_GET_SIZE(code->co_consts) - 1);

  return PyTuple_GET_ITEM(static_type_info, 0);
}

PyObject* _PyClassLoader_CheckReturnType(
    PyTypeObject* cls,
    PyObject* ret,
    _PyClassLoader_RetTypeInfo* rt_info) {
  if (ret == NULL) {
    return NULL;
  }

  int type_code = _PyClassLoader_GetTypeCode(rt_info->rt_expected);
  int overflow = 0;
  if (type_code != TYPED_OBJECT) {
    size_t int_val;
    switch (type_code) {
      case TYPED_BOOL:
        if (PyBool_Check(ret)) {
          return ret;
        }
        break;
      case TYPED_INT8:
      case TYPED_INT16:
      case TYPED_INT32:
      case TYPED_INT64:
      case TYPED_UINT8:
      case TYPED_UINT16:
      case TYPED_UINT32:
      case TYPED_UINT64:
        if (PyLong_Check(ret)) {
          if (_PyClassLoader_OverflowCheck(ret, type_code, &int_val)) {
            return ret;
          }
          overflow = 1;
        }
        break;
      default:
        PyErr_SetString(
            PyExc_RuntimeError, "unsupported primitive return type");
        Py_DECREF(ret);
        return NULL;
    }
  }

  if (overflow ||
      !(_PyObject_TypeCheckOptional(
          ret,
          rt_info->rt_expected,
          rt_info->rt_optional,
          rt_info->rt_exact))) {
    /* The override returned an incompatible value, report error */
    const char* msg;
    PyObject* exc_type = CiExc_StaticTypeError;
    if (overflow) {
      exc_type = PyExc_OverflowError;
      msg =
          "unexpected return type from %s%s%U, expected %s, got out-of-range "
          "%s (%R)";
    } else if (rt_info->rt_optional) {
      msg =
          "unexpected return type from %s%s%U, expected Optional[%s], "
          "got %s";
    } else {
      msg = "unexpected return type from %s%s%U, expected %s, got %s";
    }

    PyErr_Format(
        exc_type,
        msg,
        cls ? cls->tp_name : "",
        cls ? "." : "",
        _PyClassLoader_GetFunctionName(rt_info->rt_name),
        rt_info->rt_expected->tp_name,
        Py_TYPE(ret)->tp_name,
        ret);

    Py_DECREF(ret);
    return NULL;
  }
  return ret;
}

PyObject* _PyClassLoader_CheckReturnCallback(
    _PyClassLoader_Awaitable* awaitable,
    PyObject* result) {
  if (result == NULL) {
    return NULL;
  }
  return _PyClassLoader_CheckReturnType(
      Py_TYPE(awaitable),
      result,
      (_PyClassLoader_RetTypeInfo*)awaitable->state);
}

PyObject* _PyClassLoader_MaybeUnwrapCallable(PyObject* func) {
  if (func != NULL) {
    PyObject* res;
    if (Py_TYPE(func) == &PyStaticMethod_Type) {
      res = Ci_PyStaticMethod_GetFunc(func);
      Py_INCREF(res);
      return res;
    } else if (Py_TYPE(func) == &PyClassMethod_Type) {
      res = Ci_PyClassMethod_GetFunc(func);
      Py_INCREF(res);
      return res;
    } else if (Py_TYPE(func) == &PyProperty_Type) {
      Ci_propertyobject* prop = (Ci_propertyobject*)func;
      // A "callable" usually refers to the read path
      res = prop->prop_get;
      Py_INCREF(res);
      return res;
    }
  }
  return NULL;
}

static PyObject* check_coro_return(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* callable,
    PyObject* coro) {
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

PyObject* _PyClassLoader_CallCoroutineOverridden(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* func,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* coro = _PyObject_Vectorcall(func, args, nargsf, NULL);

  return check_coro_return(state, func, coro);
}

PyObject* _PyClassLoader_CallCoroutine(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* coro;
  PyObject* callable = state->tcs_value;
  if (PyFunction_Check(callable)) {
    coro = _PyObject_Vectorcall(callable, args, nargsf, NULL);
  } else if (Py_TYPE(callable) == &PyClassMethod_Type) {
    // We need to do some special set up for class methods when invoking.
    callable = Ci_PyClassMethod_GetFunc(state->tcs_value);
    coro = _PyObject_Vectorcall(callable, args, nargsf, NULL);
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
    // self isn't passed if we're not a descriptor
    coro = _PyObject_Vectorcall(callable, args + 1, nargsf - 1, NULL);
  }

  return check_coro_return(state, callable, coro);
}
