/* Copyright (c) Meta Platforms, Inc. and affiliates. */

#include "cinderx/StaticPython/vtable_defs.h"

#include "cinderx/CachedProperties/cached_properties.h"
#include "cinderx/Common/func.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/StaticPython/awaitable.h"
#include "cinderx/StaticPython/descrs.h"
#include "cinderx/StaticPython/functype.h"
#include "cinderx/StaticPython/thunks.h"
#include "cinderx/StaticPython/type.h"
#include "cinderx/StaticPython/typed_method_def.h"
#include "cinderx/StaticPython/vtable.h"

#if PY_VERSION_HEX < 0x030C0000
#include "cinder/exports.h"
#endif

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

_PyClassLoader_ThunkSignature* _PyClassLoader_ThunkSignature_New(
    Py_ssize_t argc) {
  return (_PyClassLoader_ThunkSignature*)PyMem_Malloc(
      sizeof(_PyClassLoader_ThunkSignature) + sizeof(uint8_t) * argc);
}

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
        sig = _PyClassLoader_ThunkSignature_New(arg_count + extra_args);
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
  sig = _PyClassLoader_ThunkSignature_New(arg_count);
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
      Py_TYPE(original) == &_PyTypedDescriptorWithDefaultValue_Type ||
      Py_TYPE(original) == &PyCachedPropertyWithDescr_Type ||
      Py_TYPE(original) == &PyAsyncCachedPropertyWithDescr_Type ||
      Py_TYPE(original) == &PyProperty_Type) {
    return &simple_sigs[1];
  } else if (Py_TYPE(original) == &_PyType_PropertyThunk) {
    // TODO: Test setter case?
    if (_PyClassLoader_PropertyThunk_Kind(original) == THUNK_SETTER) {
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

_PyClassLoader_ThunkSignature* _PyClassLoader_CopyThunkSig(
    _PyClassLoader_ThunkSignature* sig) {
  if (!sig->ta_allocated) {
    return sig;
  }

  _PyClassLoader_ThunkSignature* new_sig =
      _PyClassLoader_ThunkSignature_New(sig->ta_argcount);
  if (new_sig == NULL) {
    return NULL;
  }
  memcpy(
      new_sig,
      sig,
      sizeof(_PyClassLoader_ThunkSignature) +
          sig->ta_argcount * sizeof(uint8_t));
  return new_sig;
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

PyObject* call_descr(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* self,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* descr = state->tcs_value;
  PyObject* name = state->tcs_rt.rt_name;

  /* we have to perform the descriptor checks at runtime because the
   * descriptor type can be modified preventing us from being able to have
   * more optimized fast paths */
  if (!PyDescr_IsData(descr)) {
    PyObject** dictptr = _PyObject_GetDictPtr(self);
    if (dictptr != NULL) {
      PyObject* dict = *dictptr;
      if (dict != NULL) {
        PyObject* res = PyDict_GetItem(dict, PyTuple_GET_ITEM(name, 0));
        if (res != NULL) {
          Py_INCREF(res);
          return res;
        }
      }
    }
  }

  if (Py_TYPE(descr)->tp_descr_get != NULL) {
    PyObject* obj = args[0]; // object the descriptor is being accessed on
    PyObject* get =
        Py_TYPE(descr)->tp_descr_get(descr, obj, (PyObject*)Py_TYPE(obj));
    if (get == NULL) {
      return NULL;
    }

    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);

    PyObject* res = _PyObject_Vectorcall(
        get, args + 1, (nargs - 1) | PY_VECTORCALL_ARGUMENTS_OFFSET, NULL);
    Py_DECREF(get);
    return res;
  }
  return _PyObject_Vectorcall(descr, args, nargsf, NULL);
}

PyObject* _PyVTable_coroutine_property_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* self = args[0];
  PyObject* coro = call_descr(state, self, args, nargsf);
  if (coro == NULL) {
    return NULL;
  }

  int eager;

#if PY_VERSION_HEX < 0x030C0000
  PyObject* descr = state->tcs_value;
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
  return _PyClassLoader_NewAwaitableWrapper(
      coro, eager, (PyObject*)state, _PyClassLoader_CheckReturnCallback, NULL);
}

// Checks to see if the function is overridden in the instance. Returns true
// if we've computed the return value to the caller (either the callable to
// be invoked or NULL on an error) or false if the function is not overridden.
static bool check_instance_func(
    _PyClassLoader_TypeCheckThunk* thunk,
    PyObject* self,
    vectorcallfunc call,
    StaticMethodInfo* res) {
  PyObject** dictptr = _PyObject_GetDictPtr(self);
  PyObject* dict = dictptr != NULL ? *dictptr : NULL;
  if (dict == NULL) {
    return false;
  }
  PyObject* name = thunk->tcs_rt.rt_name;
  PyObject* callable = PyDict_GetItem(dict, name);
  if (callable == NULL) {
    return false;
  }
  _PyClassLoader_ThunkSignature* sig =
      _PyClassLoader_CopyThunkSig(thunk->tcs_rt.rt_base.mt_sig);
  if (sig == NULL) {
    goto error;
  }

  _PyClassLoader_TypeCheckThunk* new_thunk =
      (_PyClassLoader_TypeCheckThunk*)_PyClassLoader_TypeCheckThunk_New(
          callable,
          thunk->tcs_rt.rt_name,
          thunk->tcs_rt.rt_expected,
          thunk->tcs_rt.rt_optional,
          thunk->tcs_rt.rt_exact,
          sig);
  if (new_thunk == NULL) {
    _PyClassLoader_FreeThunkSignature(sig);
    goto error;
  }

  new_thunk->tcs_rt.rt_base.mt_call = call;

  res->lmr_func = (PyObject*)new_thunk;
  res->lmr_entry = (nativeentrypoint)_PyVTable_native_entry;
  return true;
error:
  res->lmr_func = NULL;
  res->lmr_entry = NULL;
  // We hit an error so we should bubble the error up to the caller.
  return true;
}

StaticMethodInfo _PyVTable_classmethod_load_overridable(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* self) {
  _PyClassLoader_ClassMethodThunk* inner_thunk =
      (_PyClassLoader_ClassMethodThunk*)state->tcs_value;
  PyTypeObject* decltype = inner_thunk->cmt_decl_type;

  StaticMethodInfo res;
  if (PyObject_TypeCheck(self, decltype) &&
      check_instance_func(
          state,
          self,
          (vectorcallfunc)_PyVTable_descr_typecheck_vectorcall,
          &res)) {
    return res;
  }

  res.lmr_func = Py_NewRef(state);
  res.lmr_entry = (nativeentrypoint)_PyVTable_native_entry;
  return res;
}

PyObject* _PyVTable_coroutine_vectorcall_no_self(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  return _PyClassLoader_CallCoroutineOverridden(
      state, state->tcs_value, args + 1, nargsf - 1);
}

PyObject* _PyVTable_coroutine_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  return _PyClassLoader_CallCoroutineOverridden(
      state, state->tcs_value, args, nargsf);
}

PyObject* _PyVTable_nonfunc_property_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    size_t nargsf) {
  PyObject* self = args[0];
  PyObject* res = call_descr(state, self, args, nargsf);

  return _PyClassLoader_CheckReturnType(
      Py_TYPE(self), res, (_PyClassLoader_RetTypeInfo*)state);
}

StaticMethodInfo _PyVTable_load_descr_typecheck(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* self) {
  PyObject* descr = state->tcs_value;
  StaticMethodInfo res;
  if (!PyDescr_IsData(descr) &&
      check_instance_func(state, self, state->tcs_rt.rt_base.mt_call, &res)) {
    return res;
  }

  if (Py_TYPE(descr)->tp_descr_get != NULL) {
    res.lmr_func =
        Py_TYPE(descr)->tp_descr_get(descr, self, (PyObject*)Py_TYPE(self));
    if (res.lmr_func == NULL) {
      res.lmr_entry = NULL;
    } else {
      _PyClassLoader_ThunkSignature* sig = NULL;
      if (state->tcs_rt.rt_base.mt_sig != NULL) {
        sig = _PyClassLoader_CopyThunkSig(state->tcs_rt.rt_base.mt_sig);
        if (sig == NULL) {
          res.lmr_func = NULL;
          res.lmr_entry = NULL;
          return res;
        }
      }
      _PyClassLoader_TypeCheckThunk* new_thunk =
          (_PyClassLoader_TypeCheckThunk*)_PyClassLoader_TypeCheckThunk_New(
              res.lmr_func,
              state->tcs_rt.rt_name,
              state->tcs_rt.rt_expected,
              state->tcs_rt.rt_optional,
              state->tcs_rt.rt_exact,
              sig);
      // _PyClassLoader_TypeCheckThunk_New refs the new thunk or we're erroring
      // out
      Py_DECREF(res.lmr_func);
      if (new_thunk == NULL) {
        res.lmr_func = NULL;
        res.lmr_entry = NULL;
        return res;
      }

      new_thunk->tcs_rt.rt_base.mt_call = state->tcs_rt.rt_base.mt_call;
      res.lmr_func = (PyObject*)new_thunk;
      res.lmr_entry = (nativeentrypoint)_PyVTable_native_entry;
    }
    return res;
  }

  // We need to type-check the return values
  res.lmr_func = Py_NewRef(state);
  res.lmr_entry = (nativeentrypoint)_PyVTable_native_entry;
  return res;
}

static _PyClassLoader_StaticCallReturn descr_get_native(
    PyObject* descr,
    PyObject* self) {
  _PyClassLoader_StaticCallReturn res;
  res.rax = _PyObject_Vectorcall(descr, &self, 1, NULL);
  res.rdx = (void*)(uint64_t)(res.rax != NULL);
  return res;
}

StaticMethodInfo _PyVTable_load_descr(PyObject* state, PyObject* self) {
  StaticMethodInfo res;
  res.lmr_func = Py_NewRef(state);
  res.lmr_entry = (nativeentrypoint)descr_get_native;
  return res;
}

PyObject* _PyVTable_descr_typecheck_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* res;
  PyObject* self = args[0];

  res = _PyObject_Vectorcall(
      state->tcs_value, (PyObject**)args + 1, nargsf - 1, NULL);

  return _PyClassLoader_CheckReturnType(
      Py_TYPE(self), res, (_PyClassLoader_RetTypeInfo*)state);
}

StaticMethodInfo _PyVTable_load_overridable(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* self) {
  StaticMethodInfo res;
  if (check_instance_func(
          state,
          self,
          (vectorcallfunc)_PyVTable_descr_typecheck_vectorcall,
          &res)) {
    return res;
  }

  // We need to type-check the return values
  res.lmr_func = Py_NewRef(state);
  res.lmr_entry = (nativeentrypoint)_PyVTable_native_entry;
  return res;
}

PyObject* _PyVTable_func_typecheck_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* res;
  PyObject* self = args[0];

  res = _PyObject_Vectorcall(state->tcs_value, (PyObject**)args, nargsf, NULL);

  return _PyClassLoader_CheckReturnType(
      Py_TYPE(self), res, (_PyClassLoader_RetTypeInfo*)state);
}

StaticMethodInfo _PyVTable_load_jitted_func(PyObject* state, PyObject* self) {
  StaticMethodInfo res;
  res.lmr_func = Py_NewRef(state);
  res.lmr_entry = (nativeentrypoint)JITRT_GET_STATIC_ENTRY(
      ((PyFunctionObject*)state)->vectorcall);
  return res;
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
    vtable->vt_entries[index].vte_load = _PyVTable_load_jitted_func;
    Py_INCREF(func);
    Py_DECREF(state);
  }
  return res;
}

PyObject* _PyVTable_staticmethod_vectorcall(
    _PyClassLoader_StaticMethodThunk* thunk,
    PyObject** args,
    size_t nargsf) {
  PyObject* func = (PyObject*)thunk->smt_func;
  return _PyObject_Vectorcall(func, ((PyObject**)args) + 1, nargsf - 1, NULL);
}

PyObject* _PyVTable_classmethod_vectorcall(
    _PyClassLoader_ClassMethodThunk* state,
    PyObject* const* args,
    size_t nargsf) {
  PyObject* classmethod = state->cmt_classmethod;
  assert(Py_TYPE(classmethod) == &PyClassMethod_Type);
  PyObject* func = Ci_PyClassMethod_GetFunc(classmethod);

  PyTypeObject* decltype = state->cmt_decl_type;
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

PyObject* _PyVTable_func_missing_vectorcall(
    _PyClassLoader_TypeCheckThunk* state,
    PyObject** args,
    Py_ssize_t nargsf) {
  PyObject* self = args[0];
  PyObject* name = state->tcs_rt.rt_name;
  PyErr_Format(
      PyExc_AttributeError,
      "'%s' object has no attribute %R",
      Py_TYPE(self)->tp_name,
      name);
  return NULL;
}

StaticMethodInfo _PyVTable_load_generic(PyObject* state, PyObject* self) {
  StaticMethodInfo res;
  res.lmr_func = Py_NewRef(state);
  res.lmr_entry = (nativeentrypoint)_PyVTable_native_entry;
  return res;
}

[[gnu::used]] _PyClassLoader_StaticCallReturn _PyVTable_thunk_native(
    _PyClassLoader_MethodThunk* thunk,
    void** args) {
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

#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
__attribute__((naked))
PyObject* _PyVTable_native_entry(PyObject* state, void** args) {
  __asm__(
      "push %rbp\n"
      "mov %rsp, %rbp\n"
      "push %rsp\n"
      /* We want to push the arguments passed natively onto the stack */
      /* so that we can recover them in hydrate_args.  So we push them */
      /* onto the stack and then move the address of them into rsi */
      /* which will make them available as the 2nd argument.  Note we */
      /* don't need to push rdi as it's the state argument which we're */
      /* passing in anyway */
      "push %r9\n"
      "push %r8\n"
      "push %rcx\n"
      "push %rdx\n"
      "push %rsi\n"
      "mov %rsp, %rsi\n"
      "call _PyVTable_thunk_native\n"
      /* We don't know if we're returning a floating point value or not */
      /* so we assume we are, and always populate the xmm registers */
      /* even if we don't need to */
      "movq %rax, %xmm0\n"
      "movq %rdx, %xmm1\n"
      "leave\n"
      "ret\n");
}
#else
PyObject* _PyVTable_native_entry(PyObject* state, void** args) {
  PyErr_SetString(
      PyExc_RuntimeError, "native entry points not available on non x-64");
  return NULL;
}
#endif
