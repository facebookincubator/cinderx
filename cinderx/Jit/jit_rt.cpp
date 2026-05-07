// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/jit_rt.h"

#include "internal/pycore_call.h"
#include "internal/pycore_ceval.h"
#include "internal/pycore_object.h"
#include "internal/pycore_pyerrors.h"
#include "internal/pycore_pystate.h"

#include "cinderx/Common/log.h"
#include "cinderx/Common/py-portability.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/string.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/interpreter.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/compiled_function.h"
#include "cinderx/Jit/context.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_rt.h"
// NOLINTNEXTLINE(facebook-unused-include-check)
#include "cinderx/Immortalize/immortalize.h"
#include "cinderx/StaticPython/classloader.h"
#include "cinderx/UpstreamBorrow/borrowed.h"

#if PY_VERSION_HEX >= 0x030E0000
#include "internal/pycore_stackref.h"
#include "internal/pycore_unicodeobject.h"
#endif

#include "internal/pycore_typeobject.h"

#ifdef Py_GIL_DISABLED
#include "internal/pycore_gc.h"
#include "internal/pycore_qsbr.h"
#endif

#include <cmath>
#include <span>

static int JITRT_BindKeywords(
    PyObject** args,
    PyObject* kwnames,
    std::span<PyObject*>& arg_space,
    Py_ssize_t argcount,
    PyCodeObject* co,
    BorrowedRef<PyObject> kwdict) {
  // Handle keyword arguments passed as two strided arrays
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwnames); i++) {
    PyObject* keyword = PyTuple_GET_ITEM(kwnames, i);
    PyObject* value = args[argcount + i];
    Py_ssize_t j;

    if (keyword == nullptr || !PyUnicode_Check(keyword)) {
      return 0;
    }

    // Speed hack: do raw pointer compares. As names are
    //    normally interned this should almost always hit.
    for (j = co->co_posonlyargcount; j < arg_space.size(); j++) {
      PyObject* name = jit::getVarname(co, j);
      if (name == keyword) {
        goto kw_found;
      }
    }

    // Slow fallback, just in case
    for (j = co->co_posonlyargcount; j < arg_space.size(); j++) {
      PyObject* name = jit::getVarname(co, j);
      int cmp = PyObject_RichCompareBool(keyword, name, Py_EQ);
      if (cmp > 0) {
        goto kw_found;
      } else if (cmp < 0) {
        return 0;
      }
    }

    if (kwdict == nullptr || PyDict_SetItem(kwdict, keyword, value) == -1) {
      return 0;
    }
    continue;

  kw_found:
    if (arg_space[j] != nullptr) {
      return 0;
    }
    arg_space[j] = value;
  }
  return 1;
}

static int JITRT_BindDefaults(
    Py_ssize_t argcount,
    std::span<PyObject*>& arg_space,
    PyCodeObject* co,
    PyFunctionObject* func) {
  // Add missing positional arguments (copy default values from defs)
  if (argcount < co->co_argcount) {
    PyObject* defaults = func->func_defaults;
    Py_ssize_t defcount = defaults == nullptr ? 0 : PyTuple_GET_SIZE(defaults);
    Py_ssize_t first_default_arg = co->co_argcount - defcount;

    // Any unset slot before the defaults region means we are missing
    // a required positional argument.
    for (Py_ssize_t i = argcount; i < first_default_arg; i++) {
      if (arg_space[i] == nullptr) {
        return 0;
      }
    }

    if (defaults != nullptr) {
      PyObject* const* defs = &((PyTupleObject*)defaults)->ob_item[0];
      // Only slots in the defaults-covered tail can still need filling.
      Py_ssize_t arg_index = std::max(argcount, first_default_arg);
      for (; arg_index < co->co_argcount; arg_index++) {
        if (arg_space[arg_index] == nullptr) {
          Py_ssize_t def_index = arg_index - first_default_arg;
          arg_space[arg_index] = defs[def_index];
        }
      }
    }
  }
  return 1;
}

// This is mostly taken from ceval.c _PyEval_EvalCodeWithName
// We use the same logic to turn **args, nargsf, and kwnames into
// **args / nargsf.
// One significant difference is we don't need to incref the args
// in the new array.
static int JITRT_BindKeywordArgs(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    std::span<PyObject*>& arg_space,
    Ref<PyObject>& kwdict,
    Ref<PyObject>& varargs) {
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  Py_ssize_t argcount = PyVectorcall_NARGS(nargsf);

  for (int i = 0; i < arg_space.size(); i++) {
    arg_space[i] = nullptr;
  }

  // Copy all positional arguments into local variables
  Py_ssize_t n = std::min<Py_ssize_t>(argcount, co->co_argcount);
  for (Py_ssize_t j = 0; j < n; j++) {
    arg_space[j] = args[j];
  }

  // Create a dictionary for keyword parameters (**kwags)
  if (co->co_flags & CO_VARKEYWORDS) {
    kwdict = Ref<>::steal(PyDict_New());
    if (kwdict == nullptr) {
      return 0;
    }
    arg_space[arg_space.size() - 1] = kwdict;
  }

  // Pack other positional arguments into the *args argument
  if (co->co_flags & CO_VARARGS) {
    varargs = Ref<>::steal(Cix_PyTuple_FromArray(args + n, argcount - n));
    if (varargs == nullptr) {
      return 0;
    }

    Py_ssize_t i = arg_space.size() - 1;
    if (co->co_flags & CO_VARKEYWORDS) {
      i--;
    }
    arg_space[i] = varargs;
  }

  // Handle keyword arguments passed as two strided arrays
  if (kwnames != nullptr &&
      !JITRT_BindKeywords(args, kwnames, arg_space, argcount, co, kwdict)) {
    return 0;
  }

  // Check the number of positional arguments
  if ((argcount > co->co_argcount) && !(co->co_flags & CO_VARARGS)) {
    return 0;
  }

  // Add missing positional arguments (copy default values from defs)
  if (!JITRT_BindDefaults(argcount, arg_space, co, func)) {
    return 0;
  }

  // Add missing keyword arguments (copy default values from kwdefs)
  if (co->co_kwonlyargcount > 0) {
    Py_ssize_t missing = 0;
    PyObject* kwdefs = func->func_kwdefaults;
    for (Py_ssize_t i = co->co_argcount; i < arg_space.size(); i++) {
      PyObject* name;
      if (arg_space[i] != nullptr) {
        continue;
      }
      name = jit::getVarname(co, i);
      if (kwdefs != nullptr) {
        PyObject* def = PyDict_GetItemWithError(kwdefs, name);
        if (def) {
          arg_space[i] = def;
          continue;
        } else if (_PyErr_Occurred(_PyThreadState_GET())) {
          return 0;
        }
      }
      missing++;
    }
    if (missing) {
      return 0;
    }
  }

  return 1;
}

static int JITRT_BindKeywordArgsSimple(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    std::span<PyObject*>& arg_space) {
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  Py_ssize_t argcount = PyVectorcall_NARGS(nargsf);
  if (argcount > co->co_argcount) {
    return 0;
  }

  for (int i = 0; i < arg_space.size(); i++) {
    arg_space[i] = nullptr;
  }

  // Copy all positional arguments into local variables
  Py_ssize_t n = std::min<Py_ssize_t>(argcount, co->co_argcount);
  for (Py_ssize_t j = 0; j < n; j++) {
    arg_space[j] = args[j];
  }

  // Check the number of positional arguments
  return JITRT_BindKeywords(args, kwnames, arg_space, argcount, co, nullptr) &&
      JITRT_BindDefaults(argcount, arg_space, co, func);
}

// This uses JITRT_BindKeywordArgs to get the newly bound keyword
// arguments.   We then turn around and dispatch to the
// JITed function with the newly packed args.
// Rather than copying over all of the error reporting we instead
// just dispatch to the normal _PyFunction_Vectorcall if anything
// goes wrong which is indicated by JITRT_BindKeywordArgs returning 0.
PyObject* JITRT_CallWithKeywordArgs(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames) {
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  const Py_ssize_t total_args = co->co_argcount + co->co_kwonlyargcount +
      ((co->co_flags & CO_VARKEYWORDS) ? 1 : 0) +
      ((co->co_flags & CO_VARARGS) ? 1 : 0);
  auto arg_space = std::make_unique<PyObject*[]>(total_args);
  Ref<PyObject> kwdict, varargs;

  std::span<PyObject*> arguments(arg_space.get(), total_args);
  if (JITRT_BindKeywordArgs(
          func, args, nargsf, kwnames, arguments, kwdict, varargs)) {
    size_t new_nargsf = total_args;
    return JITRT_GET_REENTRY(func->vectorcall)(
        (PyObject*)func, arg_space.get(), new_nargsf, nullptr);
  }

  return Ci_PyFunction_Vectorcall((PyObject*)func, args, nargsf, kwnames);
}

// This uses JITRT_BindKeywordArgs to get the newly bound keyword
// arguments.   We then turn around and dispatch to the
// JITed function with the newly packed args.
// Rather than copying over all of the error reporting we instead
// just dispatch to the normal _PyFunction_Vectorcall if anything
// goes wrong which is indicated by JITRT_BindKeywordArgs returning 0.
PyObject* JITRT_CallWithKeywordArgsSimple(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames) {
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  JIT_DCHECK(
      !(co->co_flags & (CO_VARARGS | CO_VARKEYWORDS)),
      "JITRT_CallWithKeywordArgsSimple doesn't support varargs");
  JIT_DCHECK(
      !co->co_kwonlyargcount,
      "JITRT_CallWithKeywordArgsSimple doesn't support kw only args");
  const Py_ssize_t total_args = co->co_argcount;

  // This is a relatively hot-path so we want to stack-allocate these.
  auto arg_space = (PyObject**)alloca(total_args * sizeof(PyObject*));

  std::span<PyObject*> arguments(arg_space, total_args);
  if (JITRT_BindKeywordArgsSimple(func, args, nargsf, kwnames, arguments)) {
    size_t new_nargsf = total_args;
    return JITRT_GET_REENTRY(func->vectorcall)(
        (PyObject*)func, arg_space, new_nargsf, nullptr);
  }

  return Ci_PyFunction_Vectorcall((PyObject*)func, args, nargsf, kwnames);
}

using staticvectorcallfunc = JITRT_StaticCallReturn (*)(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

using staticvectorcallfuncfp = JITRT_StaticCallFPReturn (*)(
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

JITRT_StaticCallFPReturn JITRT_CallWithIncorrectArgcountFPReturn(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    int argcount) {
  PyObject* defaults = func->func_defaults;
  if (defaults == nullptr) {
    // Function has no defaults; there's nothing we can do.
    auto interpVectorcall = getInterpretedVectorcall(func);
    interpVectorcall((PyObject*)func, args, nargsf, nullptr);
    return {0.0, 0.0};
  }
  Py_ssize_t defcount = PyTuple_GET_SIZE(defaults);
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  auto arg_space = std::make_unique<PyObject*[]>(argcount);
  Py_ssize_t defaulted_args = argcount - nargs;

  if (nargs + defcount < argcount || nargs > argcount) {
    // Not enough args with defaults, or too many args without defaults.
    auto interpVectorcall = getInterpretedVectorcall(func);
    interpVectorcall((PyObject*)func, args, nargsf, nullptr);
    return {0.0, 0.0};
  }

  Py_ssize_t i;
  for (i = 0; i < nargs; i++) {
    arg_space[i] = *args++;
  }

  PyObject** def_items =
      &((PyTupleObject*)defaults)->ob_item[defcount - defaulted_args];
  for (; i < argcount; i++) {
    arg_space[i] = *def_items++;
  }

  size_t new_nargsf = argcount;

  return reinterpret_cast<staticvectorcallfuncfp>(
      JITRT_GET_REENTRY(func->vectorcall))(
      (PyObject*)func,
      arg_space.get(),
      new_nargsf,
      // We lie to C++ here, and smuggle in the number of defaulted args filled
      // in.
      (PyObject*)defaulted_args);
}

#ifdef _WIN32
PyObject*
#else
JITRT_StaticCallReturn
#endif
JITRT_CallWithIncorrectArgcount(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    int argcount) {
  PyObject* defaults = func->func_defaults;
  if (defaults == nullptr) {
    // Function has no defaults; there's nothing we can do.
    // Fallback to the default _PyFunction_Vectorcall implementation
    // to produce an appropriate exception.
    auto interpVectorcall = getInterpretedVectorcall(func);
#ifdef _WIN32
    return interpVectorcall((PyObject*)func, args, nargsf, nullptr);
#else
    return {interpVectorcall((PyObject*)func, args, nargsf, nullptr), nullptr};
#endif
  }
  Py_ssize_t defcount = PyTuple_GET_SIZE(defaults);
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  auto arg_space = std::make_unique<PyObject*[]>(argcount);
  Py_ssize_t defaulted_args = argcount - nargs;

  if (nargs + defcount < argcount || nargs > argcount) {
    // Not enough args with defaults, or too many args without defaults.
    auto interpVectorcall = getInterpretedVectorcall(func);
#ifdef _WIN32
    return interpVectorcall((PyObject*)func, args, nargsf, nullptr);
#else
    return {interpVectorcall((PyObject*)func, args, nargsf, nullptr), nullptr};
#endif
  }

  Py_ssize_t i;
  for (i = 0; i < nargs; i++) {
    arg_space[i] = *args++;
  }

  PyObject** def_items =
      &((PyTupleObject*)defaults)->ob_item[defcount - defaulted_args];
  for (; i < argcount; i++) {
    arg_space[i] = *def_items++;
  }

  size_t new_nargsf = argcount;

#ifdef _WIN32
  return JITRT_GET_REENTRY(func->vectorcall)(
      (PyObject*)func, arg_space.get(), new_nargsf, (PyObject*)defaulted_args);
#else
  return reinterpret_cast<staticvectorcallfunc>(
      JITRT_GET_REENTRY(func->vectorcall))(
      (PyObject*)func,
      arg_space.get(),
      new_nargsf,
      // We lie to C++ here, and smuggle in the number of defaulted args filled
      // in.
      (PyObject*)defaulted_args);
#endif
}

bool JITRT_PackStaticArgs(
    PyObject** args,
    _PyTypedArgsInfo* arg_info,
    void** arg_space,
    Py_ssize_t nargs) {
  Py_ssize_t arg_index = 0;

  for (Py_ssize_t i = 0; i < nargs; i++) {
    if (arg_index < Py_SIZE(arg_info) &&
        arg_info->tai_args[arg_index].tai_argnum == i) {
      _PyTypedArgInfo* cur_arg = &arg_info->tai_args[arg_index];
      PyObject* arg = args[i];
      if (cur_arg->tai_primitive_type == -1) {
        if (!_PyObject_TypeCheckOptional(
                arg,
                cur_arg->tai_type,
                cur_arg->tai_optional,
                cur_arg->tai_exact)) {
          return true;
        }
        arg_space[i] = arg;
      } else if (cur_arg->tai_primitive_type == TYPED_BOOL) {
        if (Py_TYPE(arg) != &PyBool_Type) {
          return true;
        }
        arg_space[i] = (void*)(arg == Py_True);
      } else if (cur_arg->tai_primitive_type == TYPED_DOUBLE) {
        if (!PyFloat_Check(arg)) {
          return true;
        }
        arg_space[i] = bit_cast<void*>(PyFloat_AsDouble(arg));
      } else if (cur_arg->tai_primitive_type <= TYPED_INT64) {
        // Primitive arg check
        if (!PyLong_Check(arg) ||
            !_PyClassLoader_OverflowCheck(
                arg, cur_arg->tai_primitive_type, (size_t*)&arg_space[i])) {
          return true;
        }
      } else {
        JIT_ABORT("Unsupported primitive type {}", cur_arg->tai_primitive_type);
      }
      arg_index++;
      continue;
    }
    arg_space[i] = args[i];
  }
  return false;
}

template <typename TRetType, typename TVectorcall>
TRetType JITRT_CallStaticallyWithPrimitiveSignatureWorker(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    _PyTypedArgsInfo* arg_info) {
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  auto arg_space = std::make_unique<void*[]>(nargs);
  if (JITRT_PackStaticArgs(args, arg_info, arg_space.get(), nargs)) {
    goto fail;
  }

  return reinterpret_cast<TVectorcall>(JITRT_GET_REENTRY(func->vectorcall))(
      (PyObject*)func, (PyObject**)arg_space.get(), nargsf, nullptr);

fail:
  auto interpVectorcall = getInterpretedVectorcall(func);
  PyObject* res = interpVectorcall((PyObject*)func, args, nargsf, nullptr);
  JIT_DCHECK(res == nullptr, "should alway be reporting an error");
  return TRetType();
}

static inline Py_ssize_t vectorcall_flags(size_t n) {
  return n & PY_VECTORCALL_ARGUMENTS_OFFSET;
}

// This can either be a static method returning a primitive or a Python object,
// so we use JITRT_StaticCallReturn.  If it's returning a primitive we'll return
// rdx from the function, or return nullptr for rdx when we dispatch to
// _PyFunction_Vectorcall for error generation.  If it returns a Python object
// we'll return an additional garbage rdx from our caller, but our caller won't
// care about it either.
template <typename TRetType, typename TVectorcall>
TRetType JITRT_CallStaticallyWithPrimitiveSignatureTemplate(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info) {
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  PyCodeObject* co = (PyCodeObject*)func->func_code;

  if ((kwnames || nargs != co->co_argcount ||
       co->co_flags & (CO_VARARGS | CO_VARKEYWORDS))) {
    // we need to fixup kwnames, defaults, etc...
    const Py_ssize_t total_args = co->co_argcount + co->co_kwonlyargcount +
        ((co->co_flags & CO_VARKEYWORDS) ? 1 : 0) +
        ((co->co_flags & CO_VARARGS) ? 1 : 0);
    auto arg_space = std::make_unique<PyObject*[]>(total_args);
    Ref<PyObject> kwdict, varargs;

    std::span<PyObject*> arguments(arg_space.get(), total_args);
    if (JITRT_BindKeywordArgs(
            func, args, nargsf, kwnames, arguments, kwdict, varargs)) {
      return JITRT_CallStaticallyWithPrimitiveSignatureWorker<
          TRetType,
          TVectorcall>(
          func,
          arg_space.get(),
          total_args | vectorcall_flags(nargsf),
          arg_info);
    }

    auto interpVectorcall = getInterpretedVectorcall(func);
    interpVectorcall((PyObject*)func, args, nargsf, kwnames);
    return TRetType();
  }

  return JITRT_CallStaticallyWithPrimitiveSignatureWorker<
      TRetType,
      TVectorcall>(func, args, nargsf, arg_info);
}

JITRT_StaticCallReturn JITRT_CallStaticallyWithPrimitiveSignature(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info) {
  return JITRT_CallStaticallyWithPrimitiveSignatureTemplate<
      JITRT_StaticCallReturn,
      staticvectorcallfunc>(func, args, nargsf, kwnames, arg_info);
}

JITRT_StaticCallFPReturn JITRT_CallStaticallyWithPrimitiveSignatureFP(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info) {
  return JITRT_CallStaticallyWithPrimitiveSignatureTemplate<
      JITRT_StaticCallFPReturn,
      staticvectorcallfuncfp>(func, args, nargsf, kwnames, arg_info);
}

JITRT_StaticCallFPReturn JITRT_ReportStaticArgTypecheckErrorsWithDoubleReturn(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* /* kwnames */) {
  PyObject* res =
      JITRT_ReportStaticArgTypecheckErrors(func, args, nargsf, nullptr);
  JIT_CHECK(res == nullptr, "should always return an error");
  return {0, 0};
}

JITRT_StaticCallReturn JITRT_ReportStaticArgTypecheckErrorsWithPrimitiveReturn(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* /* kwnames */) {
  PyObject* res =
      JITRT_ReportStaticArgTypecheckErrors(func, args, nargsf, nullptr);
  JIT_CHECK(res == nullptr, "should always return an error");
  return {nullptr, nullptr};
}

PyObject* JITRT_ReportStaticArgTypecheckErrors(
    PyObject* func_obj,
    PyObject** args,
    size_t nargsf,
    PyObject* /* kwnames */) {
  BorrowedRef<PyFunctionObject> func{func_obj};
  BorrowedRef<PyCodeObject> code{func->func_code};
  auto interpVectorcall = getInterpretedVectorcall(func);
  int nkwonly = code->co_kwonlyargcount;
  if (code == nullptr || nkwonly == 0) {
    // We explicitly pass in nullptr for kwnames as the default arg count can
    // be smuggled in to this function in place of kwnames.
    return interpVectorcall(func, args, nargsf, nullptr);
  }
  // This function is called after we've successfully bound all
  // arguments. However, we want to use the interpreter to construct the
  // typecheck error. If the function takes any keyword-only arguments we must
  // reconstruct kwnames so the the interpreted "prologue" in
  // _PyEval_EvalCodeWithName can validate that the keyword-only arguments were
  // passed as keywords.
  Ref<> new_kwnames = Ref<>::steal(PyTuple_New(nkwonly));
  if (new_kwnames == nullptr) {
    return nullptr;
  }
  for (Py_ssize_t i = code->co_argcount; i < code->co_argcount + nkwonly; i++) {
    auto name = Ref<>::create(jit::getVarname(code, i));
    PyTuple_SetItem(new_kwnames, i - code->co_argcount, std::move(name));
  }
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf) - nkwonly;
  if (code->co_flags & CO_VARKEYWORDS) {
    nargs -= 1;
  }
  Py_ssize_t flags = vectorcall_flags(nargsf);
  return interpVectorcall(func, args, nargs | flags, new_kwnames);
}

/*
 * The reference for these two functions is _PyEvalFramePushAndInit in ceval.c.
 */

static void init_and_link_interpreter_frame(
    PyFunctionObject* func,
    PyCodeObject* co,
    PyThreadState* tstate,
    _frameowner owner,
    _PyInterpreterFrame* frame,
    jit::CodeRuntime* code_rt = nullptr) {
  jit::jitFrameInit(
      tstate,
      frame,
      func,
      co,
      // Zero all of localsplus. This allows _PyFrame_ClearExceptCode to
      // safely clear the locals.
      0,
      owner,
      currentFrame(tstate),
      code_rt != nullptr ? code_rt->reifier() : nullptr);

  // Re-use the existing cframe to avoid having to manage a new one. There
  // should always be one due to the existence of a the per-thread root
  // cframe. The cframe idea seems to have only transiently been needed
  // in 3.11 and is now a loose end removed in 3.13.
  setCurrentFrame(tstate, frame);
}

static inline PyThreadState* allocate_and_link_interpreter_frame(
    PyFunctionObject* func,
    PyCodeObject* co) {
  PyThreadState* tstate = PyThreadState_GET();
  JIT_DCHECK(tstate != nullptr, "thread state cannot be null");
  JIT_DCHECK(
      PyCode_Check(func->func_code),
      "Non-code object for JIT function: {}",
      jit::repr(reinterpret_cast<PyObject*>(func)));

  // Frame allocation failure is very unlikely - it can only happen if we run
  // out of memory. If this happens we behave less gracefully than the
  // interpreter as we don't have references to args to allow for proper
  // clean-up. Maybe we'll want to change this in future if it limits
  // us from getting something like a stack-trace on this kind of failure.
  _PyInterpreterFrame* frame =
      Cix_PyThreadState_PushFrame(tstate, co->co_framesize);
  JIT_CHECK(frame != nullptr, "Failed to allocate _PyInterpreterFrame");

  init_and_link_interpreter_frame(
      func, co, tstate, FRAME_OWNED_BY_THREAD, frame);

  return tstate;
}

PyThreadState* JITRT_AllocateAndLinkInterpreterFrame_Debug(
    PyFunctionObject* func,
    PyCodeObject* jit_code_object) {
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  // Given this assertion we actually don't need to incref the code object as
  // happens in _PyFrame_Initialize.
  JIT_DCHECK(co == jit_code_object, "Code object mismatch");
  return allocate_and_link_interpreter_frame(func, co);
}

PyThreadState* JITRT_AllocateAndLinkInterpreterFrame_Release(
    PyFunctionObject* func) {
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  return allocate_and_link_interpreter_frame(func, co);
}

void JITRT_InitFrameCellVars(
    PyFunctionObject* func,
    int nvars,
    PyThreadState* tstate) {
  PyObject* closure = func->func_closure;
  PyCodeObject* co = (PyCodeObject*)func->func_code;
  int offset = co->co_nlocalsplus - nvars;
  _PyInterpreterFrame* frame = interpFrameFromThreadState(tstate);
  for (int i = 0; i < offset; i++) {
    frame->localsplus[i] = Ci_STACK_NULL;
  }
  for (int i = 0; i < nvars; i++) {
    frame->localsplus[offset + i] =
        Ci_STACK_NEWREF(PyTuple_GET_ITEM(closure, i));
  }
#if PY_VERSION_HEX < 0x030E0000
  frame->stacktop = nvars + offset;
#else
  frame->stackpointer = &frame->localsplus[nvars + offset];
#endif
}

std::pair<PyThreadState*, jit::GenDataFooter*>
JITRT_AllocateAndLinkGenAndInterpreterFrame(
    PyFunctionObject* func,
    jit::CodeRuntime* code_rt,
    GenResumeFunc resume_func,
    uint64_t original_frame_pointer) {
  JIT_DCHECK(
      PyCode_Check(func->func_code),
      "Non-code object for JIT function: {}",
      jit::repr(reinterpret_cast<PyObject*>(func)));
  BorrowedRef<PyCodeObject> co{func->func_code};
  JIT_DCHECK(co == code_rt->frameState()->code(), "Code object mismatch");

  uint64_t spill_words = code_rt->spillWords();
  PyThreadState* tstate = PyThreadState_GET();
  JIT_DCHECK(tstate != nullptr, "thread state cannot be null");
  auto [gen, gen_size] = cinderx::getModuleState()->jit_gen_free_list->allocate(
      co, spill_words * sizeof(uint64_t) + sizeof(jit::GenDataFooter));

  gen->gi_frame_state = FRAME_CREATED;
  gen->gi_weakreflist = nullptr;
  gen->gi_exc_state.exc_value = nullptr;
  gen->gi_exc_state.previous_item = nullptr;
  JIT_DCHECK(func->func_name != nullptr, "func_name is null");
  gen->gi_name = Py_NewRef(func->func_name);
  JIT_DCHECK(func->func_qualname != nullptr, "func_qualname is null");
  gen->gi_qualname = Py_NewRef(func->func_qualname);

#ifdef ENABLE_GENERATOR_AWAITER
  gen->gi_ci_awaiter = nullptr;
#endif

  gen->gi_hooks_inited = 0;
  gen->gi_closed = 0;
  gen->gi_running_async = 0;
  if (co->co_flags & CO_COROUTINE) {
    int origin_depth = tstate->coroutine_origin_tracking_depth;
    if (origin_depth == 0) {
      gen->gi_origin_or_finalizer = nullptr;
    } else {
      _PyInterpreterFrame* current_frame = interpFrameFromThreadState(tstate);
      PyObject* cr_origin = Cix_compute_cr_origin(origin_depth, current_frame);
      gen->gi_origin_or_finalizer = cr_origin;
      if (!cr_origin) {
        JIT_LOG(
            "Failed to compute cr_origin for {}",
            jit::repr(func->func_qualname));
        PyErr_Clear();
      }
    }
  } else {
    gen->gi_origin_or_finalizer = nullptr;
  }

  _PyInterpreterFrame* frame = generatorFrame(gen);
  auto footer =
      reinterpret_cast<jit::GenDataFooter*>( // NOLINT performance-no-int-to-ptr
          reinterpret_cast<uintptr_t>(gen) + gen_size -
          sizeof(jit::GenDataFooter));
  *jitGenDataFooterPtr(gen, co) = footer;
  init_and_link_interpreter_frame(
      func, co, tstate, FRAME_OWNED_BY_GENERATOR, frame, code_rt);

  footer->resumeEntry = resume_func;
  footer->yieldPoint = nullptr;
  footer->gen = static_cast<PyGenObject*>(gen);
  footer->code_rt = code_rt;
  footer->originalFramePointer = original_frame_pointer;
  footer->linkAddress =
      *reinterpret_cast<uint64_t*>( // NOLINT performance-no-int-to-ptr
          original_frame_pointer);
  footer->returnAddress =
      *(reinterpret_cast<uint64_t*>( // NOLINT performance-no-int-to-ptr
            original_frame_pointer) +
        1);

  // Copy any data the register allocator may have spilled to the stack frame
  // into the generator's data area. After the caller swaps the frame pointer
  // to point to the footer, spill slot reloads will read from the generator's
  // memory instead of the stack. This copy ensures those reloads find the
  // correct values.
  if (spill_words > 0) {
    size_t spill_bytes = spill_words * sizeof(uint64_t);
    auto* src = reinterpret_cast<char*>( // NOLINT performance-no-int-to-ptr
                    original_frame_pointer) -
        spill_bytes;
    auto* dst = reinterpret_cast<char*>(footer) - spill_bytes;
    memcpy(dst, src, spill_bytes);
  }

  PyObject_GC_Track(gen);

  return {tstate, footer};
}

std::pair<jit::JitGenObject*, jit::GenDataFooter*>
JITRT_UnlinkGenFrameAndReturnGenDataFooter(PyThreadState* tstate) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  setCurrentFrame(tstate, frame->previous);

  frame->previous = nullptr;

  BorrowedRef<PyGenObject> base_gen = _PyGen_GetGeneratorFromFrame(frame);
  jit::JitGenObject* gen = jit::JitGenObject::cast(base_gen.get());
  return {gen, gen->genDataFooter()};
}

void JITRT_DecrefFrame(PyFrameObject* frame) {
  if (Py_REFCNT(frame) > 1) {
    // If the frame escaped it needs to be tracked
    Py_DECREF(frame);
    if (!_PyObject_GC_IS_TRACKED(frame)) {
      PyObject_GC_Track(frame);
    }
  } else {
    Py_DECREF(frame);
  }
}

void JITRT_UnlinkFrame(PyThreadState* tstate) {
  /*
   * The reference for this is _PyEvalFrameClearAndPop in ceval.c.
   */

  _PyInterpreterFrame* frame = currentFrame(tstate);
  setCurrentFrame(tstate, frame->previous);

  // This is needed particularly because it handles the work of copying
  // data to a PyFrameObject if one has escaped the function.
  jit::jitFrameClearExceptCode(frame);
#if PY_VERSION_HEX >= 0x030E0000
  // Can't use a plain decref as this needs to be symmetric with
  // _PyFrame_Initialize() which uses PyStackRef_FromPyObjectNew() for
  // f_executable.
  PyStackRef_CLOSE(frame->f_executable);
#else
  Py_DECREF(frameExecutable(frame));
#endif

  if (jit::getConfig().frame_mode != jit::FrameMode::kLightweight) {
    _PyThreadState_PopFrame(tstate, frame);
  }

  // JIT frames are stack allocated so there's nothing to pop.
}

// Clean up the reifier and decref the executable for a lightweight frame.
// Shared by JITRT_UnlinkLightweightFrameFast and JITRT_UnlinkLeafFrame.
static void cleanupLightweightFrameExecutable(
    _PyInterpreterFrame* frame,
    [[maybe_unused]] jit::FrameHeader* header) {
#if PY_VERSION_HEX >= 0x030E0000
  PyStackRef_CLOSE(frame->f_executable);
#else
  // Replace the reifier in f_funcobj with the actual function so that any
  // escaped references to the frame see a valid function pointer, not a
  // dangling reifier callback.
  if (jit::hasRtfsFunction(frame)) {
    frame->f_funcobj = jit::jitFrameGetRtfs(frame)->func();
  } else {
    PyObject* func = jit::jitFrameGetFunction(frame);
    frame->f_funcobj = func;
    Py_XDECREF(func);
    header->rtfs = JIT_FRAME_INITIALIZED;
  }

  Py_DECREF(frameExecutable(frame));
#endif
}

void JITRT_UnlinkLightweightFrameFast(PyThreadState* tstate) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  setCurrentFrame(tstate, frame->previous);

  JIT_DCHECK(
      jit::getConfig().frame_mode == jit::FrameMode::kLightweight,
      "only safe to call with lightweight frames");
  JIT_DCHECK(
      frameCode(frame) != nullptr && frameCode(frame)->co_nfreevars == 0,
      "assumes no freevars");

  JIT_DCHECK(
      frameCode(frame) != nullptr &&
          !(frameCode(frame)->co_flags & jit::kCoFlagsAnyGenerator),
      "doesn't work with generators");

  // Fast path for non-generator frames with no freevars.
  // The frame header is directly before the frame for non-generators.
  auto* header = reinterpret_cast<jit::FrameHeader*>(frame) - 1;
  if (header->rtfs & JIT_FRAME_INITIALIZED) {
    // Frame was materialized by the runtime, use the slow path.
    jit::jitFrameClearExceptCode(frame);
  } else {
    // Common case: just close the function object.
    Ci_STACK_CLOSE(frame->f_funcobj);
  }

  cleanupLightweightFrameExecutable(frame, header);
}

void JITRT_UnlinkLeafFrame(PyThreadState* tstate) {
  _PyInterpreterFrame* frame = currentFrame(tstate);
  setCurrentFrame(tstate, frame->previous);

  // No deopts means the frame was never materialized — skip the
  // materialization check and just close funcobj + executable directly.
  Ci_STACK_CLOSE(frame->f_funcobj);

  auto* header = reinterpret_cast<jit::FrameHeader*>(frame) - 1;
  cleanupLightweightFrameExecutable(frame, header);
}

PyObject*
JITRT_LoadGlobal(PyObject* globals, PyObject* builtins, PyObject* name) {
  PyObject* result =
      _PyDict_LoadGlobal((PyDictObject*)globals, (PyDictObject*)builtins, name);
  if ((result == nullptr) && !PyErr_Occurred()) {
    // name is converted to a `char*` by format_exc_check_arg
    _PyEval_FormatExcCheckArg(
        _PyThreadState_GET(),
        PyExc_NameError,
        "name '%.200s' is not defined",
        name);
  }
  // PyDict_LoadGlobal returns a new reference on 3.14+
  if constexpr (PY_VERSION_HEX < 0x030E0000) {
    Py_XINCREF(result);
  }
  return result;
}

PyObject* JITRT_LoadGlobalFromThreadState(
    PyThreadState* tstate,
    PyObject* name) {
  jit::RuntimeFrameState rtfs = jit::runtimeFrameStateFromThreadState(tstate);
  return JITRT_LoadGlobal(rtfs.globals(), rtfs.builtins(), name);
}

PyObject* JITRT_LoadGlobalsDict(PyThreadState* tstate) {
  jit::RuntimeFrameState rtfs = jit::runtimeFrameStateFromThreadState(tstate);
  return rtfs.globals();
}

PyObject* JITRT_LoadFunctionIndirect(PyObject** func, PyObject* descr) {
  PyObject* res = *func;
  if (!res) {
    res = _PyClassLoader_ResolveFunction(descr, nullptr);
    Py_XDECREF(res);
  }

  return res;
}

static bool is_eval_breaker_set(PyThreadState* tstate) {
  auto value =
#if PY_VERSION_HEX >= 0x030D0000
      reinterpret_cast<std::atomic_int64_t*>(&tstate->eval_breaker)
#else
      reinterpret_cast<std::atomic_int32_t*>(
          &tstate->interp->ceval.eval_breaker)
#endif
      ;
  return value->load(std::memory_order_relaxed);
}

static bool handle_periodic_activities_on_call(
    PyThreadState* tstate,
    PyObject* res,
    PyObject* callable) {
#ifdef Py_GIL_DISABLED
  _Py_qsbr_quiescent_state(
      (reinterpret_cast<_PyThreadStateImpl*>(tstate))->qsbr);
#endif
  return res != nullptr && !PyFunction_Check(callable) &&
      is_eval_breaker_set(tstate) && _Py_HandlePending(tstate) != 0;
}

PyObject*
JITRT_CallFunctionEx(PyObject* func, PyObject* pargs, PyObject* kwargs) {
  // Normalize p + kw args to tuple and dict types exactly.
  Ref<> new_pargs;
  // Logically, I don't think this incref of kwargs is needed but not having it
  // breaks the C-version of functools.partial. The problem is a ref-count of 1
  // on "kw" going into partial_new() triggers an optimization where the kwargs
  // are not copied. This fails test_functoools.TestPartial*.test_kwargs_copy
  // which asserts it's not possible to alter the kwargs after the call. A
  // tempting alternative to this explicit ref management is to set-up
  // the memory effects of CallEx to steal the kwargs input. Unfortunately this
  // breaks test_contextlib.ContextManagerTestCase.test_nokeepref by keeping
  // kwargs and their contents alive for longer than expected.
  auto new_kwargs = Ref<>::create(kwargs);
  if (kwargs) {
    if (!PyDict_CheckExact(kwargs)) {
      PyObject* d = PyDict_New();
      if (d == nullptr) {
        return nullptr;
      }
      if (PyDict_Update(d, kwargs) != 0) {
        Py_DECREF(d);
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
          PyErr_Format(
              PyExc_TypeError,
              "%.200s%.200s argument after ** "
              "must be a mapping, not %.200s",
              PyEval_GetFuncName(func),
              PyEval_GetFuncDesc(func),
              kwargs->ob_type->tp_name);
        }
        return nullptr;
      }
      kwargs = d;
      new_kwargs = Ref<>::steal(kwargs);
    }
    JIT_DCHECK(PyDict_CheckExact(kwargs), "Expect kwargs to be a dict");
  }
  if (!PyTuple_CheckExact(pargs)) {
    if (pargs->ob_type->tp_iter == nullptr && !PySequence_Check(pargs)) {
      PyErr_Format(
          PyExc_TypeError,
          "%.200s%.200s argument after * "
          "must be an iterable, not %.200s",
          PyEval_GetFuncName(func),
          PyEval_GetFuncDesc(func),
          pargs->ob_type->tp_name);
      return nullptr;
    }
    pargs = PySequence_Tuple(pargs);
    if (pargs == nullptr) {
      return nullptr;
    }
    new_pargs = Ref<>::steal(pargs);
  }
  JIT_DCHECK(PyTuple_CheckExact(pargs), "Expected pargs to be a tuple");

  PyThreadState* tstate = _PyThreadState_GET();
  PyObject* res = _PyObject_Call(tstate, func, pargs, kwargs);
  // In 3.12 calls to non-Python functions will check for the eval breaker
  // We handle that here rather than bloat every function call w/ an extra
  // check.
  if (handle_periodic_activities_on_call(tstate, res, func)) {
    Py_DECREF(res);
    return nullptr;
  }
  return res;
}

PyObject* JITRT_Call(
    PyThreadState* tstate,
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  JIT_DCHECK(
      (nargsf & PY_VECTORCALL_ARGUMENTS_OFFSET),
      "JITRT_Call must always be called as a vectorcall");

  if constexpr (PY_VERSION_HEX >= 0x030E0000) {
    // Calling a bound method leaves us with an unused first arg.
    if (args[0] == nullptr) {
      args += 1;
      nargsf -= 1;
    }
  } else {
    // Trying to call a function rather than a method on an object.  Shift the
    // arguments over by one.
    //
    // In theory this is supposed to expect nullptr on the stack, but our HIR
    // implementation of LOAD_ATTR/LOAD_METHOD uses Py_None.  Check for nullptr
    // just in case.
    if (callable == nullptr || Py_IsNone(callable)) {
      callable = args[0];
      args += 1;
      nargsf -= 1;
    }
  }
  PyObject* res =
      _PyObject_VectorcallTstate(tstate, callable, args, nargsf, kwnames);
  // In 3.12 calls to non-Python functions will check for the eval breaker
  // We handle that here rather than bloat every function call w/ an extra
  // check.
  if (handle_periodic_activities_on_call(tstate, res, callable)) {
    Py_DECREF(res);
    return nullptr;
  }
  return res;
}

PyObject* JITRT_VectorcallTstate(
    PyThreadState* tstate,
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames) {
  PyObject* res =
      _PyObject_VectorcallTstate(tstate, callable, args, nargsf, kwnames);
  // In 3.12 calls to non-Python functions will check for the eval breaker
  // We handle that here rather than bloat every function call w/ an extra
  // check.
  if (handle_periodic_activities_on_call(tstate, res, callable)) {
    Py_DECREF(res);
    return nullptr;
  }
  return res;
}

PyObject* JITRT_UnaryNot(PyObject* value) {
  int res = PyObject_IsTrue(value);
  if (res == 0) {
    Py_INCREF(Py_True);
    return Py_True;
  } else if (res > 0) {
    Py_INCREF(Py_False);
    return Py_False;
  }
  return nullptr;
}

LoadMethodResult JITRT_GetMethod(PyObject* obj, PyObject* name) {
  PyObject* method = nullptr;
  int found = _PyObject_GetMethod(obj, name, &method);
  if (method == nullptr) {
    return {nullptr, nullptr};
  }
  if (!found) {
    Py_INCREF(Py_None);
    return {Py_None, method};
  }
  Py_INCREF(obj);
  return {method, obj};
}

static inline PyObject* super_lookup_method_or_attr(
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    int call_no_args,
    int* meth_found) {
  if (global_super != (PyObject*)&PySuper_Type) {
    Ref<> super_instance;
    if (call_no_args) {
      super_instance = Ref<>::steal(PyObject_CallNoArgs(global_super));
    } else {
      super_instance = Ref<>::steal(
          PyObject_CallFunctionObjArgs(global_super, type, self, NULL));
    }
    if (super_instance == nullptr) {
      return nullptr;
    }
    BorrowedRef<> result = PyObject_GetAttr(super_instance, name);
    if (meth_found) {
      *meth_found = 0;
    }
    return result;
  }
  // Check Py_TYPE(self) because in a class method super call
  // self can be a type. https://github.com/python/cpython/pull/106977
  if (Py_TYPE(self)->tp_getattro != PyObject_GenericGetAttr) {
    meth_found = nullptr;
  }
  return _PySuper_Lookup(type, self, name, meth_found);
}

LoadMethodResult JITRT_GetMethodFromSuper(
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call) {
  int meth_found = 0;
  PyObject* result = super_lookup_method_or_attr(
      global_super, type, self, name, no_args_in_super_call, &meth_found);
  if (result == nullptr) {
    return {nullptr, nullptr};
  }
  if (meth_found) {
    if (!(PyFunction_Check(result) || Py_TYPE(result) == &PyMethodDescr_Type ||
          Py_TYPE(result) == &PyWrapperDescr_Type ||
          PyType_HasFeature(Py_TYPE(result), Py_TPFLAGS_METHOD_DESCRIPTOR))) {
      meth_found = 0;
    }
  } else {
    meth_found = 0;
  }
  if (meth_found) {
    Py_INCREF(self);
    return {result, self};
  }
  Py_INCREF(Py_None);
  return {Py_None, result};
}

PyObject* JITRT_GetAttrFromSuper(
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call) {
  return super_lookup_method_or_attr(
      global_super, type, self, name, no_args_in_super_call, nullptr);
}

PyObject* JITRT_InvokeMethod(
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargs,
    PyObject*) {
  PyTypeObject* self_type = Py_TYPE(args[0]);
  _PyType_VTable* vtable = (_PyType_VTable*)self_type->tp_cache;

  return _PyClassLoader_InvokeMethod(vtable, slot, args, nargs);
}

PyObject* JITRT_InvokeClassMethod(
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargs,
    PyObject*) {
  PyTypeObject* self_type = (PyTypeObject*)args[0];
  _PyType_VTable* vtable = (_PyType_VTable*)self_type->tp_cache;

  return _PyClassLoader_InvokeMethod(vtable, slot, args, nargs);
}

/* This function is inlined to LIR via kCHelpersManual, so changes here will
 * have no effect. */
PyObject* JITRT_Cast(PyObject* obj, PyTypeObject* type) {
  if (PyObject_TypeCheck(obj, type)) {
    return obj;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected '%s', got '%s'",
      type->tp_name,
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

PyObject* JITRT_CastOptional(PyObject* obj, PyTypeObject* type) {
  if (_PyObject_TypeCheckOptional(obj, type, /* opt */ 1, /* exact */ 0)) {
    return obj;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected '%s', got '%s'",
      type->tp_name,
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

PyObject* JITRT_CastExact(PyObject* obj, PyTypeObject* type) {
  if (_PyObject_TypeCheckOptional(obj, type, /* opt */ 0, /* exact */ 1)) {
    return obj;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected exactly '%s', got '%s'",
      type->tp_name,
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

PyObject* JITRT_CastOptionalExact(PyObject* obj, PyTypeObject* type) {
  if (_PyObject_TypeCheckOptional(obj, type, /* opt */ 1, /* exact */ 1)) {
    return obj;
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected exactly '%s', got '%s'",
      type->tp_name,
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

/* Needed because cast to float does extra work that would be a pain to add to
 * the manual inlined LIR for JITRT_Cast. */
PyObject* JITRT_CastToFloat(PyObject* obj) {
  if (PyObject_TypeCheck(obj, &PyFloat_Type)) {
    // cast to float is not considered pass-through by refcount insertion (since
    // it may produce a new reference), so even if in fact it is pass-through
    // (because we got a float), we need to return a new reference.
    Py_INCREF(obj);
    return obj;
  } else if (PyObject_TypeCheck(obj, &PyLong_Type)) {
    // special case because Python typing pretends int subtypes float
    return PyFloat_FromDouble(PyLong_AsDouble(obj));
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected 'float', got '%s'",
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

PyObject* JITRT_CastToFloatOptional(PyObject* obj) {
  if (_PyObject_TypeCheckOptional(
          obj, &PyFloat_Type, /* opt */ 1, /* exact */ 0)) {
    // cast to float is not considered pass-through by refcount insertion (since
    // it may produce a new reference), so even if in fact it is pass-through
    // (because we got a float), we need to return a new reference.
    Py_INCREF(obj);
    return obj;
  } else if (PyObject_TypeCheck(obj, &PyLong_Type)) {
    // special case because Python typing pretends int subtypes float
    return PyFloat_FromDouble(PyLong_AsDouble(obj));
  }

  PyErr_Format(
      CiExc_StaticTypeError,
      "expected 'float', got '%s'",
      Py_TYPE(obj)->tp_name);

  return nullptr;
}

int64_t JITRT_ShiftLeft64(int64_t x, int64_t y) {
  return x << y;
}
int32_t JITRT_ShiftLeft32(int32_t x, int32_t y) {
  return x << y;
}

int64_t JITRT_ShiftRight64(int64_t x, int64_t y) {
  return x >> y;
}
int32_t JITRT_ShiftRight32(int32_t x, int32_t y) {
  return x >> y;
}

uint64_t JITRT_ShiftRightUnsigned64(uint64_t x, uint64_t y) {
  return x >> y;
}
uint32_t JITRT_ShiftRightUnsigned32(uint32_t x, uint32_t y) {
  return x >> y;
}

int64_t JITRT_Mod64(int64_t x, int64_t y) {
  return x % y;
}
int32_t JITRT_Mod32(int32_t x, int32_t y) {
  return x % y;
}

uint64_t JITRT_ModUnsigned64(uint64_t x, uint64_t y) {
  return x % y;
}
uint32_t JITRT_ModUnsigned32(uint32_t x, uint32_t y) {
  return x % y;
}

PyObject* JITRT_BoxI32(int32_t i) {
  return PyLong_FromLong(i);
}

PyObject* JITRT_BoxU32(uint32_t i) {
  return PyLong_FromUnsignedLong(i);
}

PyObject* JITRT_BoxBool(uint32_t i) {
  if (i) {
    return Py_True;
  }
  return Py_False;
}

PyObject* JITRT_BoxI64(int64_t i) {
  return PyLong_FromSsize_t(i);
}

PyObject* JITRT_BoxU64(uint64_t i) {
  return PyLong_FromSize_t(i);
}

PyObject* JITRT_BoxDouble(double_t d) {
  return PyFloat_FromDouble(d);
}

double JITRT_PowerDouble(double x, double y) {
  return pow(x, y);
}

double JITRT_SqrtDouble(double x) {
  return sqrt(x);
}

double JITRT_Power32(int32_t x, int32_t y) {
  return pow(x, y);
}

double JITRT_PowerUnsigned32(uint32_t x, uint32_t y) {
  return pow(x, y);
}

double JITRT_Power64(int64_t x, int64_t y) {
  return pow(x, y);
}

double JITRT_PowerUnsigned64(uint64_t x, uint64_t y) {
  return pow(x, y);
}

template <typename T>
static T checkedUnboxImpl(PyObject* obj) {
  constexpr bool is_signed = std::is_signed_v<T>;
  std::conditional_t<is_signed, int64_t, uint64_t> res;
  if constexpr (is_signed) {
    res = PyLong_AsSsize_t(obj);
  } else {
    res = PyLong_AsSize_t(obj);
  }
  if (T(res) == res || (!is_signed && res == T(-1) && PyErr_Occurred())) {
    return res;
  }
  PyErr_SetString(PyExc_OverflowError, "int overflow");
  return -1;
}

uint64_t JITRT_UnboxU64(PyObject* obj) {
  return PyLong_AsSize_t(obj);
}

uint32_t JITRT_UnboxU32(PyObject* obj) {
  return checkedUnboxImpl<uint32_t>(obj);
}

uint16_t JITRT_UnboxU16(PyObject* obj) {
  return checkedUnboxImpl<uint16_t>(obj);
}

uint8_t JITRT_UnboxU8(PyObject* obj) {
  return checkedUnboxImpl<uint8_t>(obj);
}

int64_t JITRT_UnboxI64(PyObject* obj) {
  return PyLong_AsSsize_t(obj);
}

int32_t JITRT_UnboxI32(PyObject* obj) {
  return checkedUnboxImpl<int32_t>(obj);
}

int16_t JITRT_UnboxI16(PyObject* obj) {
  return checkedUnboxImpl<int16_t>(obj);
}

int8_t JITRT_UnboxI8(PyObject* obj) {
  return checkedUnboxImpl<int8_t>(obj);
}

PyObject* JITRT_ImportName(
    PyThreadState* tstate,
    PyObject* name,
    PyObject* fromlist,
    PyObject* level) {
  DEFINE_STATIC_STRING(__import__);
  PyObject* globals = PyEval_GetGlobals();
  PyObject* builtins = tstate->interp->builtins;

  auto import_func =
      Ref<>::create(PyDict_GetItemWithError(builtins, s___import__));

  JIT_DCHECK(
      import_func || !PyErr_Occurred(),
      "_PyDict_GetItemIdWithError should only fail with invalid identifiers");
  if (import_func == nullptr) {
    PyErr_SetString(PyExc_ImportError, "__import__ not found");
    return nullptr;
  }

  /* Fast path for not overloaded __import__. */
  if (import_func == CI_INTERP_IMPORT_FIELD(tstate->interp, import_func)) {
    int ilevel = PyLong_AsInt(level);
    if (ilevel == -1 && _PyErr_Occurred(tstate)) {
      return nullptr;
    }
    return PyImport_ImportModuleLevelObject(
        name,
        globals,
        // Locals are not actually used by the builtin import.
        // This is documented behavior as of Python 3.7.
        Py_None,
        fromlist,
        ilevel);
  }

  // In this implementation we always pass None for locals as it's easier than
  // fully materializing them now. The CPython interpreter has strange (probably
  // broken) behavior - it will only pass a dictionary of locals to
  // __builtins__.__import__() if the locals have been materialized already, for
  // example by a call to locals(). Reliance on this behavior is unlikely.
  PyObject* locals = Py_None;

  return PyObject_CallFunctionObjArgs(
      import_func, name, globals, locals, fromlist, level, nullptr);
}

void JITRT_SetCurrentAwaiter(PyObject* awaitable, PyThreadState* ts) {
#ifdef ENABLE_GENERATOR_AWAITER

  _PyInterpreterFrame* frame = interpFrameFromThreadState(ts);
  // Matches SEND/SEND_GEN's check in bytecodes.c
  if (frame->owner != FRAME_OWNED_BY_GENERATOR ||
      (!(frame->f_code->co_flags & (CO_COROUTINE | CO_ASYNC_GENERATOR)))) {
    return;
  }
  auto awaiter =
      reinterpret_cast<PyObject*>(_PyGen_GetGeneratorFromFrame(frame));

  Ci_PyAwaitable_SetAwaiter(awaitable, awaiter);
#endif // ENABLE_GENERATOR_AWAITER
}

JITRT_GenSendRes JITRT_GenSend(
    PyObject* gen,
    PyObject* v,
    uint64_t finish_yield_from,
    _PyInterpreterFrame* frame) {
  if (v == nullptr) {
    return {nullptr, 1};
  }
  if (finish_yield_from) {
    Py_INCREF(v);
    return {v, 1};
  }
  PyObject* retval;

#ifdef ENABLE_GENERATOR_AWAITER
  if (_PyFrame_GetCode(frame)->co_flags & (CO_COROUTINE | CO_ASYNC_GENERATOR)) {
    BorrowedRef<PyGenObject> base_gen = _PyGen_GetGeneratorFromFrame(frame);
    Ci_PyAwaitable_SetAwaiter(gen, base_gen);
  }
#endif

  auto gen_status = PyIter_Send(gen, v, &retval);

  if (gen_status == PYGEN_RETURN) {
    return {retval, 1};
  }
  if (gen_status == PYGEN_ERROR) {
    return {nullptr, 1};
  }
  JIT_DCHECK(
      gen_status == PYGEN_NEXT,
      "Unexpected gen_status: {}",
      fmt::underlying(gen_status));
  return {retval, 0};
}

JITRT_GenSendRes JITRT_GenSendHandleStopAsyncIteration(
    PyObject* gen,
    PyObject* v,
    uint64_t finish_yield_from,
    _PyInterpreterFrame* frame) {
  JITRT_GenSendRes res = JITRT_GenSend(gen, v, finish_yield_from, frame);
  if ((res.retval == nullptr) && (res.done == 1) &&
      PyErr_ExceptionMatches(PyExc_StopAsyncIteration)) {
    PyErr_Clear();
    res.retval = &JITRT_IterDoneSentinel;
  }
  return res;
}

PyObject* JITRT_FormatValue(
    PyThreadState* tstate,
    PyObject* fmt_spec,
    PyObject* value,
    int conversion) {
  PyObject* (*conv_fn)(PyObject*);

  /* See if any conversion is specified. */
  switch (conversion) {
    case FVC_NONE:
      conv_fn = nullptr;
      break;
    case FVC_STR:
      conv_fn = PyObject_Str;
      break;
    case FVC_REPR:
      conv_fn = PyObject_Repr;
      break;
    case FVC_ASCII:
      conv_fn = PyObject_ASCII;
      break;
    default:
      _PyErr_Format(
          tstate,
          PyExc_SystemError,
          "unexpected conversion flag %d",
          conversion);
      return nullptr;
  }

  /* If there's a conversion function, call it and replace
     value with that result. Otherwise, just use value,
     without conversion. */
  Ref<> converted;
  if (conv_fn != nullptr) {
    converted = Ref<>::steal(conv_fn(value));
    if (converted == nullptr) {
      return nullptr;
    }
    value = converted.get();
  }

  /* If value is a unicode object, and there's no fmt_spec,
     then we know the result of format(value) is value
     itself. In that case, skip calling format(). I plan to
     move this optimization in to PyObject_Format()
     itself. */
  if (PyUnicode_CheckExact(value) && fmt_spec == nullptr) {
    /* Do nothing, just return. */
    Py_INCREF(value);
    return value;
  }

  /* Actually call format(). */
  return PyObject_Format(value, fmt_spec);
}

PyObject* JITRT_BuildString(
    PyThreadState* /*tstate*/,
    void* /*unused*/,
    PyObject** args,
    size_t nargsf,
    void* /*unused*/) {
  size_t nargs = PyVectorcall_NARGS(nargsf);

  Ref<> empty = Ref<>::steal(PyUnicode_New(0, 0));
  if (empty == nullptr) {
    return nullptr;
  }

  return _PyUnicode_JoinArray(empty, args, nargs);
}

JITRT_StaticCallReturn JITRT_FailedDeferredCompileShim(PyObject** args) {
  void* no_error = reinterpret_cast<void*>(1);

  // The function object is always the first argument in the static calling
  // convention.
  PyFunctionObject* func = reinterpret_cast<PyFunctionObject*>(args[0]);
  PyCodeObject* code = (PyCodeObject*)func->func_code;
  int total_args = code->co_argcount;
  if (code->co_flags & CO_VARARGS) {
    total_args++;
  }
  if (code->co_flags & CO_VARKEYWORDS) {
    total_args++;
  }

  // PyObject** args is:
  // arg0 - function object
  // arg1 - first real argument
  // argx - remaining register arguments
  // ...
  // previous frame pointer
  // return address to JITed code
  // memory argument 0 - first stack argument
  // memory argument 1
  // ...

  PyObject** dest_args;
  auto final_args = std::make_unique<PyObject*[]>(total_args);
  int cc_reg_args = jit::codegen::ARGUMENT_REGS.size();

  if (total_args < cc_reg_args) {
    // no gap in args to worry about
    dest_args = args + 1;
  } else {
    for (int i = 0; i < cc_reg_args - 1; i++) {
      final_args[i] = args[i + 1];
    }
    for (int i = cc_reg_args - 1; i < total_args; i++) {
      final_args[i] = args[i + 3];
    }
    dest_args = final_args.get();
  }

  _PyTypedArgsInfo* arg_info =
      jit::getContext()->findFunctionPrimitiveArgInfo(func);
  auto allocated_args = std::make_unique<PyObject*[]>(
      arg_info == nullptr ? 0 : Py_SIZE(arg_info));
  int allocated_count = 0;

  if (arg_info != nullptr) {
    // We have primitive values that need to be converted into boxed values
    // to run the interpreter loop.
    for (Py_ssize_t i = 0; i < Py_SIZE(arg_info); i++) {
      if (arg_info->tai_args[i].tai_primitive_type != -1) {
        // primitive type, box...
        int arg = arg_info->tai_args[i].tai_argnum + 1;
        uint64_t arg_val;
        if (arg >= cc_reg_args) {
          arg += 4;
        }
        arg_val = (uint64_t)args[arg];

        PyObject* new_val = _PyClassLoader_Box(
            arg_val, arg_info->tai_args[i].tai_primitive_type);

        if (new_val == nullptr) {
          for (int j = 0; j < allocated_count; j++) {
            Py_DECREF(allocated_args[j]);
          }
          return JITRT_StaticCallReturn{nullptr, nullptr};
        }

        // we can update the incoming arg array, either it's
        // the pushed values on the stack by the trampoline, or
        // it's final_args we allocated above.
        dest_args[arg - 1] = new_val;
        allocated_args[allocated_count++] = new_val;
      }
    }
  }

  PyObject* res =
      _PyObject_Vectorcall((PyObject*)func, dest_args, total_args, nullptr);

  for (int j = 0; j < allocated_count; j++) {
    Py_DECREF(allocated_args[j]);
  }

  // If there was an error, don't try to unbox null
  if (res == nullptr) {
    return JITRT_StaticCallReturn{res, nullptr};
  }

  // If we are supposed to be returning a primitive, it needs unboxing because
  // our caller expected this to be a static->static direct invoke, we just
  // failed to JIT the callee.
  int optional, exact;
  PyTypeObject* ret_type = _PyClassLoader_ResolveType(
      _PyClassLoader_GetReturnTypeDescr(func), &optional, &exact);
  int ret_code = _PyClassLoader_GetTypeCode(ret_type);
  Py_DECREF(ret_type);
  if (ret_code != TYPED_OBJECT) {
    // we can always unbox to 64-bit, the JIT will just ignore the higher bits.
    // This means that overflow here will give weird results, but overflow in
    // primitive ints in static python is undefined behavior right now anyway,
    // until we implement overflow checking. It doesn't make sense to implement
    // overflow checking just here in the "unjitable" code path, when overflow
    // won't be checked if the code is JITted.
    void* ival;
    if (ret_code == TYPED_BOOL) {
      ival = (void*)(res == Py_True);
    } else if (ret_code & TYPED_INT_SIGNED) {
      ival = (void*)JITRT_UnboxI64(res);
    } else {
      ival = (void*)JITRT_UnboxU64(res);
    }
    return JITRT_StaticCallReturn{ival, no_error};
  }

  return JITRT_StaticCallReturn{res, no_error};
}

PyObject* JITRT_UnpackExToTuple(
    PyThreadState* tstate,
    PyObject* iterable,
    int before,
    int after) {
  JIT_DCHECK(iterable != nullptr, "The iterable cannot be null.");

  Ref<> it = Ref<>::steal(PyObject_GetIter(iterable));
  if (it == nullptr) {
    if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
        iterable->ob_type->tp_iter == nullptr && !PySequence_Check(iterable)) {
      _PyErr_Format(
          tstate,
          PyExc_TypeError,
          "cannot unpack non-iterable %.200s object",
          iterable->ob_type->tp_name);
    }
    return nullptr;
  }

  int totalargs = before + after + 1;
  Ref<PyTupleObject> tuple = Ref<PyTupleObject>::steal(PyTuple_New(totalargs));
  if (tuple == nullptr) {
    return nullptr;
  }
  int ti = 0;

  for (int i = 0; i < before; i++) {
    PyObject* w = PyIter_Next(it);
    if (w == nullptr) {
      /* Iterator done, via error or exhaustion. */
      if (!_PyErr_Occurred(tstate)) {
        if (after == -1) {
          _PyErr_Format(
              tstate,
              PyExc_ValueError,
              "not enough values to unpack "
              "(expected %d, got %d)",
              before,
              i);
        } else {
          _PyErr_Format(
              tstate,
              PyExc_ValueError,
              "not enough values to unpack "
              "(expected at least %d, got %d)",
              before + after,
              i);
        }
      }
      return nullptr;
    }
    tuple->ob_item[ti++] = w;
  }

  JIT_DCHECK(
      after >= 0,
      "This function should only be used for UNPACK_EX, where after >= 0.");

  PyObject* list = PySequence_List(it);
  if (list == nullptr) {
    return nullptr;
  }
  tuple->ob_item[ti++] = list;

  Py_ssize_t list_size = PyList_GET_SIZE(list);
  if (list_size < after) {
    _PyErr_Format(
        tstate,
        PyExc_ValueError,
        "not enough values to unpack (expected at least %d, got %zd)",
        before + after,
        before + list_size);
    return nullptr;
  }

  /* Pop the "after-variable" args off the list. */
  for (int j = after; j > 0; j--) {
    tuple->ob_item[ti++] = PyList_GET_ITEM(list, list_size - j);
  }
  /* Resize the list. */
  Py_SET_SIZE(list, list_size - after);

  return reinterpret_cast<PyObject*>(tuple.release());
}

int JITRT_UnpackSequence(
    PyThreadState* tstate,
    PyObject* iterable,
    PyObject** items,
    int count) {
  JIT_DCHECK(iterable != nullptr, "The iterable cannot be null.");

  Ref<> it = Ref<>::steal(PyObject_GetIter(iterable));
  if (it == nullptr) {
    if (_PyErr_ExceptionMatches(tstate, PyExc_TypeError) &&
        iterable->ob_type->tp_iter == nullptr && !PySequence_Check(iterable)) {
      _PyErr_Format(
          tstate,
          PyExc_TypeError,
          "cannot unpack non-iterable %.200s object",
          iterable->ob_type->tp_name);
    }
    return -1;
  }

  for (int i = 0; i < count; i++) {
    PyObject* w = PyIter_Next(it);
    if (w == nullptr) {
      /* Iterator done, via error or exhaustion. */
      if (!_PyErr_Occurred(tstate)) {
        _PyErr_Format(
            tstate,
            PyExc_ValueError,
            "not enough values to unpack "
            "(expected %d, got %d)",
            count,
            i);
      }
      /* Clean up any items already stored. */
      for (int j = 0; j < i; j++) {
        Py_DECREF(items[j]);
      }
      return -1;
    }
    items[i] = w;
  }

  /* We should have exhausted the iterator now. */
  PyObject* w = PyIter_Next(it);
  if (w != nullptr) {
    Py_DECREF(w);
    _PyErr_Format(
        tstate,
        PyExc_ValueError,
        "too many values to unpack (expected %d)",
        count);
    for (int j = 0; j < count; j++) {
      Py_DECREF(items[j]);
    }
    return -1;
  }
  if (_PyErr_Occurred(tstate)) {
    for (int j = 0; j < count; j++) {
      Py_DECREF(items[j]);
    }
    return -1;
  }

  return 0;
}

int JITRT_UnicodeEquals(PyObject* s1, PyObject* s2, int equals) {
  // one of these must be unicode for the quality comparison to be okay
  assert(PyUnicode_CheckExact(s1) || PyUnicode_CheckExact(s2));
  if (s1 == s2) {
    return equals == Py_EQ;
  }

  if (PyUnicode_CheckExact(s1) && PyUnicode_CheckExact(s2)) {
    if (PyUnicode_READY(s1) < 0 || PyUnicode_READY(s2) < 0) {
      return -1;
    }

    Py_ssize_t length = PyUnicode_GET_LENGTH(s1);
    if (length != PyUnicode_GET_LENGTH(s2)) {
      return equals == Py_NE;
    }

    Py_hash_t hash1 = ((PyASCIIObject*)s1)->hash;
    Py_hash_t hash2 = ((PyASCIIObject*)s2)->hash;
    if (hash1 != hash2 && hash1 != -1 && hash2 != -1) {
      return equals == Py_NE;
    }

    int kind = PyUnicode_KIND(s1);
    if (kind != PyUnicode_KIND(s2)) {
      return equals == Py_NE;
    }
    void* data1 = PyUnicode_DATA(s1);
    void* data2 = PyUnicode_DATA(s2);
    if (PyUnicode_READ(kind, data1, 0) != PyUnicode_READ(kind, data2, 0)) {
      return equals == Py_NE;
    } else if (length == 1) {
      return equals == Py_EQ;
    } else {
      int result = memcmp(data1, data2, (size_t)(length * kind));
      return (equals == Py_EQ) ? (result == 0) : (result != 0);
    }
  }
  return PyObject_RichCompareBool(s1, s2, equals);
}

PyObject* JITRT_SequenceContains(PyObject* haystack, PyObject* needle) {
  int result = PySequence_Contains(haystack, needle);
  if (result < 0) {
    return nullptr;
  }
  if (result) {
    Py_RETURN_TRUE;
  }
  Py_RETURN_FALSE;
}

PyObject* JITRT_SequenceNotContains(PyObject* haystack, PyObject* needle) {
  int result = PySequence_Contains(haystack, needle);
  if (result < 0) {
    return nullptr;
  }
  if (result) {
    Py_RETURN_FALSE;
  }
  Py_RETURN_TRUE;
}

int JITRT_NotContainsBool(PyObject* w, PyObject* v) {
  int res = PySequence_Contains(w, v);
  if (res == -1) {
    return -1;
  }
  return !res;
}

/* Perform a rich comparison with integer result.  This wraps
   PyObject_RichCompare(), returning -1 for error, 0 for false, 1 for true. */
int JITRT_RichCompareBool(PyObject* v, PyObject* w, int op) {
  Ref<> res = Ref<>::steal(PyObject_RichCompare(v, w, op));

  if (res == nullptr) {
    return -1;
  } else if (PyBool_Check(res)) {
    return res == Py_True;
  }

  return PyObject_IsTrue(res);
}

/* perform a batch decref to the objects in args */
void JITRT_BatchDecref(PyObject** args, int nargs) {
  for (int i = 0; i < nargs; i++) {
    Py_DECREF(args[i]);
  }
}

Py_ssize_t JITRT_CheckSequenceBounds(PyObject* s, Py_ssize_t i) {
  JIT_DCHECK(!PyErr_Occurred(), "called with error set");
  i = i < 0 ? i + Py_SIZE(s) : i;
  if (i < 0 || i >= Py_SIZE(s)) {
    // If the access is out of bounds then call the runtime lookup function just
    // to make sure we get a consistent exceptions between interpreter + JIT.
    Ref<> i_obj = Ref<>::steal(PyLong_FromSsize_t(i));
    if (i_obj == nullptr) {
      return -1;
    }
    JIT_CHECK(
        PyObject_GetItem(s, i_obj) == nullptr,
        "JIT found bound error, but runtime did not");
    return -1;
  }
  return i;
}

PyObject* JITRT_GetLength(PyObject* obj) {
  // Same as GET_LEN handler in Python/ceval.c
  Py_ssize_t len = PyObject_Length(obj);
  if (len < 0) {
    return nullptr;
  }
  return PyLong_FromSsize_t(len);
}

int JITRT_DictUpdate(PyThreadState* tstate, PyObject* dict, PyObject* update) {
  if (PyDict_Update(dict, update) < 0) {
    if (_PyErr_ExceptionMatches(tstate, PyExc_AttributeError)) {
      _PyErr_Format(
          tstate,
          PyExc_TypeError,
          "'%.200s' object is not a mapping",
          Py_TYPE(update)->tp_name);
    }
    return -1;
  }
  return 0;
}

int JITRT_DictMerge(
    PyThreadState* tstate,
    PyObject* dict,
    PyObject* update,
    PyObject* func) {
#if PY_VERSION_HEX >= 0x030F0000
  PyObject* dupkey = NULL;
  if (_PyDict_MergeUniq(dict, update, &dupkey) < 0) {
    _PyEval_FormatKwargsError(tstate, func, update, dupkey);
    Py_XDECREF(dupkey);
    return -1;
  }
#else
  if (_PyDict_MergeEx(dict, update, 2) < 0) {
    _PyEval_FormatKwargsError(tstate, func, update);
    return -1;
  }
#endif
  return 0;
}

PyObject* JITRT_CopyDictWithoutKeys(PyObject* subject, PyObject* keys) {
  // Copied from Python/ceval.c implementation of COPY_DICT_WITHOUT_KEYS.
  Ref<> rest(Ref<>::steal(PyDict_New()));
  if (rest == nullptr || PyDict_Update(rest, subject)) {
    return nullptr;
  }
  JIT_DCHECK(PyTuple_CheckExact(keys), "Expected keys to be an exact tuple");
  for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(keys); i++) {
    if (PyDict_DelItem(rest, PyTuple_GET_ITEM(keys, i))) {
      return nullptr;
    }
  }
  return rest.release();
}

PyObject* JITRT_LoadName(PyThreadState* tstate, int name_idx) {
  jit::RuntimeFrameState rtfs = jit::runtimeFrameStateFromThreadState(tstate);
  return PyTuple_GET_ITEM(rtfs.code()->co_names, name_idx);
}

void JITRT_FormatAwaitableError(
    PyThreadState* tstate,
    PyTypeObject* type,
    bool is_aenter) {
  if (type->tp_as_async != nullptr && type->tp_as_async->am_await != nullptr) {
    return;
  }
  const char* msg = is_aenter
      ? "'async with' received an object from __aenter__ "
        "that does not implement __await__: %.100s"
      : "'async with' received an object from __aexit__ "
        "that does not implement __await__: %.100s";
  _PyErr_Format(tstate, PyExc_TypeError, msg, type->tp_name);
}

void JITRT_IncRefTotal() {
#ifdef Py_REF_DEBUG
  _Py_INCREF_IncRefTotal();
#endif
}

void JITRT_DecRefTotal() {
#ifdef Py_REF_DEBUG
  _Py_DECREF_DecRefTotal();
#endif
}

PyObject* JITRT_LookupAttrSpecial(
    PyObject* obj,
    PyObject* attr,
    const char* failure_fmt_str) {
  PyObject* res = _PyObject_LookupSpecial(obj, attr);
  if (res == nullptr && !_PyErr_Occurred(_PyThreadState_GET())) {
    _PyErr_Format(
        _PyThreadState_GET(),
        PyExc_TypeError,
        failure_fmt_str,
        Py_TYPE(obj)->tp_name);
  }
  return res;
}

LoadMethodResult JITRT_LoadSpecial(
    [[maybe_unused]] PyObject* self,
    [[maybe_unused]] int special_idx) {
#if PY_VERSION_HEX >= 0x030E0000
  _PyStackRef method_and_self[2] = {
      PyStackRef_NULL, PyStackRef_FromPyObjectNew(self)};
  PyObject* name = _Py_SpecialMethods[special_idx].name;
  int err = _PyObject_LookupSpecialMethod(name, method_and_self);
  if (err <= 0) {
    PyStackRef_CLOSE(method_and_self[1]);
    if (err < 0) {
      // When __get__ raises, method_and_self[0] holds a ref to the descriptor
      PyStackRef_XCLOSE(method_and_self[0]);
    } else if (err == 0) {
      PyObject* owner = PyStackRef_AsPyObjectBorrow(method_and_self[1]);
      const char* errfmt = _PyEval_SpecialMethodCanSuggest(owner, special_idx)
          ? _Py_SpecialMethods[special_idx].error_suggestion
          : _Py_SpecialMethods[special_idx].error;
      PyThreadState* tstate = PyThreadState_GET();
      JIT_CHECK(!_PyErr_Occurred(tstate), "Unexpected existing exception");
      JIT_CHECK(errfmt, "No error message for special method");
      _PyErr_Format(tstate, PyExc_TypeError, errfmt, owner);
    }
    return {nullptr, nullptr};
  }
  LoadMethodResult result;
  result.callable = PyStackRef_AsPyObjectSteal(method_and_self[0]);
  result.self_or_null = PyStackRef_IsNull(method_and_self[1])
      ? nullptr
      : PyStackRef_AsPyObjectSteal(method_and_self[1]);
  return result;
#endif
  JIT_ABORT("JITRT_LoadSpecial not valid with this version of Python");
}

#ifdef Py_GIL_DISABLED
void JITRT_AtQuiescentState(PyThreadState* tstate) {
  _Py_qsbr_quiescent_state(
      (reinterpret_cast<_PyThreadStateImpl*>(tstate))->qsbr);
}
#endif

PyObject JITRT_IterDoneSentinel = {
    _PyObject_EXTRA_INIT
#if PY_VERSION_HEX >= 0x030E0000
// clang-format off
#ifdef Py_GIL_DISABLED
    .ob_tid = _Py_UNOWNED_TID,
    .ob_flags = _Py_STATICALLY_ALLOCATED_FLAG,
    .ob_mutex = {0},
    .ob_gc_bits = _PyGC_BITS_DEFERRED,
    .ob_ref_local = _Py_IMMORTAL_REFCNT_LOCAL,
    .ob_ref_shared = 0,
#else
    {.ob_refcnt = _Py_IMMORTAL_INITIAL_REFCNT,
     .ob_flags = _Py_STATIC_FLAG_BITS},
#endif
// clang-format on

#else
    {.ob_refcnt = _Py_IMMORTAL_REFCNT},
#endif
    nullptr};

PyObject* JITRT_InvokeIterNext(PyObject* iterator) {
  iternextfunc iternext_f = Py_TYPE(iterator)->tp_iternext;
  if (iternext_f == nullptr) {
    PyErr_Format(
        PyExc_TypeError,
        "'%.100s' object is not an iterator",
        Py_TYPE(iterator)->tp_name);
    return nullptr;
  }
  PyObject* val = iternext_f(iterator);
  if (val != nullptr) {
    return val;
  }
  if (PyErr_Occurred()) {
    if (!PyErr_ExceptionMatches(PyExc_StopIteration)) {
      return nullptr;
    }
    PyErr_Clear();
  }
  Py_INCREF(&JITRT_IterDoneSentinel);
  return &JITRT_IterDoneSentinel;
}
