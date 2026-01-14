// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/generators_rt.h"

#if PY_VERSION_HEX >= 0x030C0000

#include "internal/pycore_frame.h"
#include "internal/pycore_genobject.h"
#include "internal/pycore_pyerrors.h" // _PyErr_ClearExcState()

#include "cinderx/Common/log.h"
#include "cinderx/Jit/deopt.h"
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/generators_borrowed.h"
#include "cinderx/Jit/generators_mm.h"
#include "cinderx/Jit/runtime.h"
#include "cinderx/UpstreamBorrow/borrowed.h"
#include "cinderx/module_state.h"

#include <string_view>

namespace jit {

PyObject* JitGenObject::yieldFrom() {
  GenDataFooter* gen_footer = genDataFooter();
  const GenYieldPoint* yield_point = gen_footer->yieldPoint;
  PyObject* yield_from = nullptr;
  if (gi_frame_state < FRAME_COMPLETED && yield_point) {
    yield_from = yieldFromValue(gen_footer, yield_point);
    Py_XINCREF(yield_from);
  }
  return yield_from;
}

namespace {

const destructor original_gen_dealloc = PyGen_Type.tp_dealloc;
const destructor original_coro_dealloc = PyCoro_Type.tp_dealloc;

void jitgen_dealloc(PyObject* self) {
  if (!deopt_jit_gen(self)) {
    JIT_ABORT("Tried to dealloc a running JIT generator");
  }

  // CPython deallocation modified to respect our free-list.
  Cix_gen_dealloc_with_custom_free(self);
}

int jitgen_traverse(PyObject* obj, visitproc visit, void* arg) {
  JitGenObject* jit_gen = JitGenObject::cast(obj);
  if (jit_gen != nullptr) {
    const GenDataFooter* gen_footer = jit_gen->genDataFooter();
    if (gen_footer->yieldPoint == nullptr) {
      return 0;
    }
    size_t deopt_idx = gen_footer->yieldPoint->deoptIdx();
    const DeoptMetadata& meta =
        gen_footer->code_rt->getDeoptMetadata(deopt_idx);
    for (const LiveValue& value : meta.live_values) {
      if (value.ref_kind != hir::RefKind::kOwned) {
        continue;
      }
      codegen::PhyLocation loc = value.location;
      JIT_CHECK(
          !loc.is_register(),
          "DeoptMetadata for Yields should not reference registers");
      PyObject* v = *reinterpret_cast<PyObject**>(
          reinterpret_cast<uintptr_t>(gen_footer) + loc.loc);
      Py_VISIT(v);
    }
    JIT_CHECK(JitGen_CheckAny(obj), "Deopted during GC traversal");
  }
  // Try to use CPython traverse as much as we can as it has internals which
  // are hard to borrow in 3.14 (compares 'visit' to a speicifc internal
  // function).
  return PyGen_Type.tp_traverse(obj, visit, arg);
}

void raise_already_running_exception(JitGenObject* jit_gen) {
  // If the executor is running we cannot deopt so have to replicate the
  // errors from CPython here.
  const char* msg = "generator already executing";
  if (Py_TYPE(jit_gen) == cinderx::getModuleState()->coroType()) {
    msg = "coroutine already executing";
  }
  PyErr_SetString(PyExc_ValueError, msg);
}

// Resumes a JIT generator. Calling this performs the same work as invoking the
// interpreter on a generator with a freshly created/suspended frame. As much
// as possible is broken out into C++ before control is passed to JIT code.
Ref<> send_core(JitGenObject* jit_gen, PyObject* arg, PyThreadState* tstate) {
  PyObject* gen_obj = reinterpret_cast<PyObject*>(jit_gen);
  GenDataFooter* gen_footer = jit_gen->genDataFooter();

  _PyInterpreterFrame* frame = generatorFrame(jit_gen);
  // See comment about reusing the cframe in jit_rt.cpp,
  // allocate_and_link_interpreter_frame().

  frame->previous = currentFrame(tstate);
  setCurrentFrame(tstate, frame);

  // Enter generated code.
  JIT_DCHECK(
      gen_footer->yieldPoint != nullptr,
      "Attempting to resume a generator with no yield point");
  Ref<> result = Ref<>::steal(gen_footer->resumeEntry(
      gen_obj, arg, 0 /* finish_yield_from (not used in 3.12+) */, tstate));

  // If we deopted then the interpreter will handle setting frame state and
  // there will no longer be any JIT state. We can check if this happened by
  // seeing if the type of the generator object is no longer a JitGenObject.
  if (JitGen_CheckAny(gen_obj)) {
    tstate->exc_info = jit_gen->gi_exc_state.previous_item;
    jit_gen->gi_exc_state.previous_item = nullptr;

    setCurrentFrame(tstate, frame->previous);

    frame->previous = nullptr;
    if (jit_gen->gi_frame_state == FRAME_COMPLETED) {
      jit_gen->gi_frame_state = FRAME_CLEARED;
      jitFrameClearExceptCode(frame);
    } else {
#if PY_VERSION_HEX >= 0x030E0000
      jit_gen->gi_frame_state = gen_footer->yieldPoint->isYieldFrom()
          ? FRAME_SUSPENDED_YIELD_FROM
          : FRAME_SUSPENDED;
#else
      jit_gen->gi_frame_state = FRAME_SUSPENDED;
#endif
    }
  }

  return result;
}

// This is a cut down version of gen_send_ex2() from genobject.c which only
// handles sending in values, and calls send_core() above to dispatch to a
// JIT function rather than executing with the interpreter. If any of the
// inputs would lead to an exception, try to deopt and hand back to the CPython
// version.
PySendResult jitgen_am_send(PyObject* obj, PyObject* arg, PyObject** presult) {
  JitGenObject* gen = JitGenObject::cast(obj);
  if (gen == nullptr) {
    return Py_TYPE(obj)->tp_as_async->am_send(obj, arg, presult);
  }

  // Check for user programming errors.
  if (gen->gi_frame_state >= FRAME_EXECUTING ||
      (gen->gi_frame_state == FRAME_CREATED && arg && !Py_IsNone(arg))) {
    // Try to deopt to easily reproduce CPython errors.
    if (!deopt_jit_gen(obj)) {
      raise_already_running_exception(gen);
      *presult = nullptr;
      return PYGEN_ERROR;
    }
    return Py_TYPE(obj)->tp_as_async->am_send(obj, arg, presult);
  }

  if (arg == nullptr) {
    arg = Py_None;
  }

  PyThreadState* tstate = PyThreadState_Get();
  _PyErr_StackItem* prev_exc_info = tstate->exc_info;
  gen->gi_exc_state.previous_item = prev_exc_info;
  tstate->exc_info = &gen->gi_exc_state;

  gen->gi_frame_state = FRAME_EXECUTING;
  EVAL_CALL_STAT_INC(EVAL_CALL_GENERATOR);

  // Execution happens here
  PyObject* result = send_core(gen, arg, tstate).release();

  JIT_DCHECK(tstate->exc_info == prev_exc_info, "Invalid exc_info");
  JIT_DCHECK(gen->gi_exc_state.previous_item == nullptr, "Invalid exc_state");
  JIT_DCHECK(gen->gi_frame_state != FRAME_EXECUTING, "Invalid frame state");
  _PyInterpreterFrame* frame = generatorFrame(gen);
  JIT_DCHECK(
      JitGen_CheckAny(obj) || frame->previous == nullptr,
      "Previous frame still linked");

  /* If the generator just returned (as opposed to yielding), signal
   * that the generator is exhausted. */
  if (result) {
    if (gen->gi_frame_state < FRAME_COMPLETED) {
      *presult = result;
      return PYGEN_NEXT;
    }

    JIT_DCHECK(
        Py_IsNone(result) || !PyAsyncGen_CheckExact(gen), "Invalid result");
    if (Py_IsNone(result) && !PyAsyncGen_CheckExact(gen) && !arg) {
      /* Return NULL if called by gen_iternext() */
      Py_CLEAR(result);
    }

  } else {
    JIT_DCHECK(
        !PyErr_ExceptionMatches(PyExc_StopIteration),
        "Generator should not raise StopIteration");
    JIT_DCHECK(
        !PyAsyncGen_CheckExact(gen) ||
            !PyErr_ExceptionMatches(PyExc_StopAsyncIteration),
        "Async gen should not raise StopAsyncIteration");
  }

#ifdef ENABLE_GENERATOR_AWAITER
  Py_CLEAR(gen->gi_ci_awaiter);
#endif

#if PY_VERSION_HEX < 0x030E0000
  _PyErr_ClearExcState(&gen->gi_exc_state);
#else
  JIT_DCHECK(
      gen->gi_exc_state.exc_value == nullptr,
      "Should not have an exception by now");
#endif
  JIT_DCHECK(gen->gi_frame_state == FRAME_CLEARED, "Frame not cleared");

  *presult = result;
  return result ? PYGEN_RETURN : PYGEN_ERROR;
}

PyObject* jitgen_send(PyObject* obj, PyObject* arg) {
  PyObject* result = nullptr;
  if (jitgen_am_send(obj, arg, &result) == PYGEN_RETURN) {
    if (result == Py_None) {
      PyErr_SetNone(PyExc_StopIteration);
    } else {
      _PyGen_SetStopIterationValue(result);
    }
    Py_CLEAR(result);
  }
  return result;
}

PyObject* jitgen_iternext(PyObject* obj) {
  PyObject* result = nullptr;
  if (jitgen_am_send(obj, nullptr, &result) == PYGEN_RETURN) {
    if (result != Py_None) {
      _PyGen_SetStopIterationValue(result);
    }
    Py_CLEAR(result);
  }
  return result;
}

// Cached methods from base generator filled in by init_jit_genobject_type().
// These are unlikely to be performance sensitive and don't need to run
// particularly fast so we could do dynamic lookups. However, the obvious way
// of doing this is to first deopt and then do a method call. Deopting on throw
// or close isn't too bad, but doing so on __sizeof__() is a bit dubious as the
// generator may end up in the interpreter unnecessarily. So, I made the
// machinery to cache methods anyway and we may as well use it. This does all
// make the assumption that the methods on PyGen_Type don't change.
typedef PyObject* (
    *GenThrowMeth)(PyObject* obj, PyObject* const* args, Py_ssize_t nargs);
GenThrowMeth gen_throw_meth;
PyCFunction gen_close_meth;
PyCFunction gen___sizeof___meth;

PyObject* jitgen_throw(PyObject* obj, PyObject* const* args, Py_ssize_t nargs) {
  // Always deopt as an exception being raised internally would cause a JIT
  // generator to deopt anyway.
  if (!deopt_jit_gen(obj)) {
    raise_already_running_exception(reinterpret_cast<JitGenObject*>(obj));
    return nullptr;
  }
  return gen_throw_meth(obj, args, nargs);
}

PyObject* jitgen_close(PyObject* obj, PyObject*) {
  // Always deopt as closing either raises an exception in the generator which
  // would cause a deopt anyway or if the generator is already done then deopt
  // is cheap and won't rexecute in the interpreter.
  if (!deopt_jit_gen(obj)) {
    raise_already_running_exception(reinterpret_cast<JitGenObject*>(obj));
    return nullptr;
  }
  return gen_close_meth(obj, nullptr);
}

PyObject* jitgen_sizeof(PyObject* obj, PyObject*) {
  Ref<> base_size = Ref<>::steal(gen___sizeof___meth(obj, nullptr));
  if (base_size == nullptr) {
    return nullptr;
  }
  JitGenObject* jit_gen = JitGenObject::cast(obj);
  if (jit_gen == nullptr) {
    return base_size.release();
  }
  Py_ssize_t base_size_int = PyLong_AsSsize_t(base_size);
  if (base_size_int == -1 && PyErr_Occurred()) {
    return nullptr;
  }
  // +1 word for storing the GenDataFooter pointer
  // +size of the GenDataFooter
  // +the size of the JIT register spill area.
  return PyLong_FromSsize_t(
      base_size_int + sizeof(GenDataFooter*) + sizeof(GenDataFooter) +
      jit_gen->genDataFooter()->spillWords * sizeof(uint64_t));
}

PyObject* jitgen_getyieldfrom(PyObject* obj, void*) {
  JitGenObject* jit_gen = JitGenObject::cast(obj);
  if (jit_gen == nullptr) {
    return PyObject_GetAttrString(obj, "gi_yieldfrom");
  }
  PyObject* yield_from = jit_gen->yieldFrom();
  if (yield_from == nullptr) {
    Py_RETURN_NONE;
  }
  return yield_from;
}

PyObject* jitgen_getclass(PyObject*, void*) {
  return Py_NewRef(&PyGen_Type);
}

PyObject* jitcoro_getclass(PyObject*, void*) {
  return Py_NewRef(&PyCoro_Type);
}

void jitgen_finalize(PyObject* obj) {
  PyGenObject* gen = reinterpret_cast<PyGenObject*>(obj);

  // Fast-path: generator has completed so there's nothing to do.
  if (gen->gi_frame_state >= FRAME_COMPLETED) {
    return;
  }

  // Slow-path: generator is still running, so we deopt and defer to runtime
  // logic for raising errors/warnings and possibly closing the generator (which
  // would require a deopt anyway).
  JIT_CHECK(deopt_jit_gen(obj), "Tried to finalize a running JIT generator");
  PyGen_Type.tp_finalize(obj);
}

typedef struct {
  PyObject_HEAD
  PyObject* cw_coroutine;
} JitCoroWrapper;

void jitcoro_wrapper_dealloc(JitCoroWrapper* cw) {
  PyObject_GC_UnTrack(reinterpret_cast<PyObject*>(cw));
  Py_CLEAR(cw->cw_coroutine);
  PyObject_GC_Del(cw);
}

PyObject* jitcoro_wrapper_iternext(JitCoroWrapper* cw) {
  return jitgen_iternext(cw->cw_coroutine);
}

PyObject* jitcoro_wrapper_send(JitCoroWrapper* cw, PyObject* arg) {
  return jitgen_send(cw->cw_coroutine, arg);
}

PyObject* jitcoro_wrapper_throw(
    JitCoroWrapper* cw,
    PyObject* const* args,
    Py_ssize_t nargs) {
  return jitgen_throw(cw->cw_coroutine, args, nargs);
}

PyObject* jitcoro_wrapper_close(JitCoroWrapper* cw, PyObject* args) {
  return jitgen_close(cw->cw_coroutine, args);
}

int jitcoro_wrapper_traverse(JitCoroWrapper* cw, visitproc visit, void* arg) {
  Py_VISIT(cw->cw_coroutine);
  return 0;
}

static PyMethodDef jitcoro_wrapper_methods[] = {
    {"send",
     reinterpret_cast<PyCFunction>(jitcoro_wrapper_send),
     METH_O,
     nullptr},
    {"throw", _PyCFunction_CAST(jitcoro_wrapper_throw), METH_FASTCALL, nullptr},
    {"close",
     reinterpret_cast<PyCFunction>(jitcoro_wrapper_close),
     METH_NOARGS,
     nullptr},
    {} /* Sentinel */
};

#ifdef ENABLE_GENERATOR_AWAITER

static void jitocorowrapper_set_awaiter(
    JitCoroWrapper* self,
    PyObject* awaiter) {
  Ci_PyAwaitable_SetAwaiter(self->cw_coroutine, awaiter);
}

static Ci_AsyncMethodsWithExtra jitcorowrapper_as_async = {
    .ame_async_methods = {},
    // Filled in by init_jit_genobject_type()
    .ame_setawaiter =
        reinterpret_cast<setawaiterfunc>(jitocorowrapper_set_awaiter),
};

static PyAsyncMethods* jitcorowrapper_async_methods =
    &jitcorowrapper_as_async.ame_async_methods;

#else

#ifdef Ci_TPFLAGS_HAVE_AM_EXTRA
#undef Ci_TPFLAGS_HAVE_AM_EXTRA
#endif
#define Ci_TPFLAGS_HAVE_AM_EXTRA 0

static PyAsyncMethods* jitcorowrapper_async_methods = nullptr;

#endif
PyTypeObject _JitCoroWrapper_Type = {
    PyVarObject_HEAD_INIT(&PyType_Type, 0) "coroutine_wrapper",
    sizeof(JitCoroWrapper), /* tp_basicsize */
    0, /* tp_itemsize */
    reinterpret_cast<destructor>(
        jitcoro_wrapper_dealloc), /* destructor tp_dealloc */
    0, /* tp_vectorcall_offset */
    nullptr, /* tp_getattr */
    nullptr, /* tp_setattr */
    jitcorowrapper_async_methods, /* tp_as_async */
    nullptr, /* tp_repr */
    nullptr, /* tp_as_number */
    nullptr, /* tp_as_sequence */
    nullptr, /* tp_as_mapping */
    nullptr, /* tp_hash */
    nullptr, /* tp_call */
    nullptr, /* tp_str */
    PyObject_GenericGetAttr, /* tp_getattro */
    nullptr, /* tp_setattro */
    nullptr, /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Ci_TPFLAGS_HAVE_AM_EXTRA, /* tp_flags */
    "A wrapper object implementing __await__ for coroutines.",
    reinterpret_cast<traverseproc>(jitcoro_wrapper_traverse), /* tp_traverse */
    nullptr, /* tp_clear */
    nullptr, /* tp_richcompare */
    0, /* tp_weaklistoffset */
    PyObject_SelfIter, /* tp_iter */
    reinterpret_cast<iternextfunc>(jitcoro_wrapper_iternext), /* tp_iternext */
    jitcoro_wrapper_methods, /* tp_methods */
    nullptr, /* tp_members */
    nullptr, /* tp_getset */
    nullptr, /* tp_base */
    nullptr, /* tp_dict */
    nullptr, /* tp_descr_get */
    nullptr, /* tp_descr_set */
    0, /* tp_dictoffset */
    nullptr, /* tp_init */
    nullptr, /* tp_alloc */
    nullptr, /* tp_new */
    nullptr, /* tp_free */
    nullptr, /* tp_is_gc */
    nullptr, /* tp_bases */
    nullptr, /* tp_mro */
    nullptr, /* tp_cache */
    nullptr, /* tp_subclasses */
    nullptr, /* tp_weaklist */
    nullptr, /* tp_del */
    0, /* tp_version_tag */
    nullptr, /* tp_finalize */
};

PyObject* jitcoro_await(PyCoroObject* coro) {
  JitCoroWrapper* cw = PyObject_GC_New(JitCoroWrapper, &_JitCoroWrapper_Type);
  if (cw == nullptr) {
    return nullptr;
  }
  cw->cw_coroutine = Py_NewRef(coro);
  PyObject_GC_Track(cw);
  return reinterpret_cast<PyObject*>(cw);
}

// Unfortunately we need to fork this because it uses _PyCoroObject_CAST which
// includes an assert on the exact type of coroutine.
PyObject* jitcoro_repr(PyObject* self) {
  PyCoroObject* coro = reinterpret_cast<PyCoroObject*>(self);
  return PyUnicode_FromFormat(
      "<coroutine object %S at %p>", coro->cr_qualname, coro);
}

static PyMethodDef jitgen_methods[] = {
    {"send", reinterpret_cast<PyCFunction>(jitgen_send), METH_O, nullptr},
    {"throw",
     reinterpret_cast<PyCFunction>(jitgen_throw),
     METH_FASTCALL,
     nullptr},
    {"close",
     reinterpret_cast<PyCFunction>(jitgen_close),
     METH_NOARGS,
     nullptr},
    {"__sizeof__",
     reinterpret_cast<PyCFunction>(jitgen_sizeof),
     METH_NOARGS,
     nullptr},
#if PY_VERSION_HEX >= 0x030E0000
    {"__class_getitem__", nullptr, METH_O | METH_CLASS, nullptr},
#endif
    {} /* Sentinel */
};

// Null fields are copied from the base generator type by
// init_jit_genobject_type(). The order must match the one in genobject.c
static PyGetSetDef jitgen_getsetlist[] = {
    {"__name__", nullptr, nullptr, nullptr},
    {"__qualname__", nullptr, nullptr, nullptr},
    {"gi_yieldfrom", (getter)jitgen_getyieldfrom, nullptr, nullptr},
    {"gi_running", nullptr, nullptr, nullptr},
    {"gi_frame", nullptr, nullptr, nullptr},
    {"gi_suspended", nullptr, nullptr, nullptr},
    {"gi_code", nullptr, nullptr, nullptr},
    {"__class__", jitgen_getclass, nullptr, nullptr},
    {} /* Sentinel */
};

// The fully null entries are filled in by init_jit_genobject_type()
static PyGetSetDef jitcoro_getsetlist[] = {
    {"__name__", nullptr, nullptr, nullptr},
    {"__qualname__", nullptr, nullptr, nullptr},
    {"cr_await", (getter)jitgen_getyieldfrom, nullptr, nullptr},
    {"cr_running", nullptr, nullptr, nullptr},
    {"cr_frame", nullptr, nullptr, nullptr},
    {"cr_code", nullptr, nullptr, nullptr},
    {"cr_suspended", nullptr, nullptr, nullptr},
#ifdef ENABLE_GENERATOR_AWAITER
    {"cr_ci_awaiter", nullptr, nullptr, nullptr},
    {"cr_awaiter", nullptr, nullptr, nullptr},
#endif
    {"__class__", jitcoro_getclass, nullptr, nullptr},
    {} /* Sentinel */
};

static PyMemberDef jitcoro_memberlist[] = {
    {"cr_origin",
     T_OBJECT,
     offsetof(PyCoroObject, cr_origin_or_finalizer),
     READONLY},
    {} /* Sentinel */
};

static PyMethodDef jitcoro_methods[] = {
    {"send", (PyCFunction)jitgen_send, METH_O, nullptr},
    {"throw", _PyCFunction_CAST(jitgen_throw), METH_FASTCALL, nullptr},
    {"close", (PyCFunction)jitgen_close, METH_NOARGS, nullptr},
    {"__sizeof__", (PyCFunction)jitgen_sizeof, METH_NOARGS, nullptr},
#if PY_VERSION_HEX >= 0x030E0000
    {"__class_getitem__", nullptr, METH_O | METH_CLASS, nullptr},
#endif
#ifdef ENABLE_GENERATOR_AWAITER
    {"__set_awaiter__", nullptr, METH_O, nullptr},
#endif
    {} /* Sentinel */
};

#ifdef ENABLE_GENERATOR_AWAITER

static Ci_AsyncMethodsWithExtra jitcoro_as_async = {
    .ame_async_methods =
        {
            .am_await = reinterpret_cast<unaryfunc>(jitcoro_await),
            .am_aiter = nullptr,
            .am_anext = nullptr,
            .am_send = (sendfunc)jitgen_am_send,
        },
    // Filled in by init_jit_genobject_type()
    .ame_setawaiter = nullptr,
};

#else

#ifdef Ci_TPFLAGS_HAVE_AM_EXTRA
#undef Ci_TPFLAGS_HAVE_AM_EXTRA
#endif
#define Ci_TPFLAGS_HAVE_AM_EXTRA 0

static PyAsyncMethods jitcoro_as_async = {
    .am_await = reinterpret_cast<unaryfunc>(jitcoro_await),
    .am_aiter = NULL,
    .am_anext = NULL,
    .am_send = (sendfunc)jitgen_am_send,
};

#endif

} // namespace

PyType_Slot gen_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(jitgen_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(jitgen_traverse)},
    {Py_tp_finalize, reinterpret_cast<void*>(jitgen_finalize)},
    {Py_tp_iter, reinterpret_cast<void*>(PyObject_SelfIter)},
    {Py_tp_iternext, reinterpret_cast<void*>(jitgen_iternext)},
    {Py_tp_methods, jitgen_methods},
    {Py_tp_getset, jitgen_getsetlist},
    {Py_am_send, reinterpret_cast<void*>(jitgen_am_send)},
    // gi_weakreflist
    {0, nullptr},
};

constexpr size_t kGenObjectSize =
#if PY_VERSION_HEX >= 0x030E0000
    sizeof(PyGenObject)
#else
    // For 3.12 and 3.13 generator objects are variable-sized, so we use
    // offsetof(). This is inherited from genobject.c.
    offsetof(PyGenObject, gi_iframe) + offsetof(_PyInterpreterFrame, localsplus)
#endif
    ;

PyType_Spec JitGen_Spec = {
    .name = "builtins.generator",
    // We store our pointer to JIT data in an additional
    // variable slot at the end of the object.
    .basicsize = kGenObjectSize + sizeof(GenDataFooter*),
    .itemsize = sizeof(PyObject*),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = gen_slots,
};

static_assert(sizeof(PyGenObject) == sizeof(PyCoroObject));

PyType_Slot coro_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(jitgen_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(jitgen_traverse)},
    {Py_tp_finalize, reinterpret_cast<void*>(jitgen_finalize)},
    {Py_tp_methods, jitcoro_methods},
    {Py_tp_members, jitcoro_memberlist},
    {Py_tp_getset, jitcoro_getsetlist},
    {Py_am_await, reinterpret_cast<void*>(jitcoro_await)},
    {Py_am_send, reinterpret_cast<void*>(jitgen_am_send)},
    {Py_tp_repr, reinterpret_cast<void*>(jitcoro_repr)},
    {0, nullptr},
};

PyType_Spec JitCoro_Spec = {
    .name = "builtins.coroutine",
    // We store our pointer to JIT data in an additional variable slot at the
    // end of the object.
    .basicsize = kGenObjectSize + sizeof(GenDataFooter*),
    .itemsize = sizeof(PyObject*),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Ci_TPFLAGS_HAVE_AM_EXTRA | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = coro_slots,
};

void deopt_jit_gen_object_only(JitGenObject* gen) {
  PyTypeObject* old_type = Py_TYPE(gen);

  PyTypeObject* type = Py_TYPE(gen) == cinderx::getModuleState()->genType()
      ? &PyGen_Type
      : &PyCoro_Type;
  Py_DECREF(old_type);
  Py_SET_TYPE(reinterpret_cast<PyObject*>(gen), type);
  if (getConfig().frame_mode == FrameMode::kLightweight) {
    auto frame = generatorFrame(gen);
    jitFrameRemoveReifier(frame);
  }
}

bool deopt_jit_gen(PyObject* obj) {
  JitGenObject* jit_gen = JitGenObject::cast(obj);
  if (jit_gen == nullptr) {
    return true;
  }
  if (jit_gen->gi_frame_state == FRAME_EXECUTING) {
    return false;
  }
  GenDataFooter* gen_footer = jit_gen->genDataFooter();

  if (gen_footer->yieldPoint) {
    // TODO: This "deopting" mechanism should be better shared with the
    // similar machinery for general JIT deopting. Among other things we're
    // missing deopt logging here. Although if we used the existing stuff
    // for this it might be misleading as the "cause" will not be an
    // executed instruction.
    const DeoptMetadata& deopt_meta = gen_footer->code_rt->getDeoptMetadata(
        gen_footer->yieldPoint->deoptIdx());
    JIT_CHECK(
        deopt_meta.inline_depth() == 0,
        "inline functions not supported for generators");
    auto frame = generatorFrame(jit_gen);
    if (getConfig().frame_mode == FrameMode::kLightweight) {
      jitFramePopulateFrame(frame);
    }
    reifyGeneratorFrame(
        frame, deopt_meta, deopt_meta.innermostFrame(), gen_footer);
    // Ownership of references has been transferred from JIT to interpreter.
    releaseRefs(deopt_meta, gen_footer);
  } else {
    JIT_CHECK(
        jit_gen->gi_frame_state >= FRAME_COMPLETED,
        "JIT generator has no yield point and is not running or completed");
  }

  deopt_jit_gen_object_only(jit_gen);

  return true;
}

// Cache/copy features of PyGen_Type so we don't need to reimplement them.
void init_jit_genobject_type() {
  using namespace std::literals;
  // Copy base type functions
  BorrowedRef<PyTypeObject> gen_type = cinderx::getModuleState()->genType();
  BorrowedRef<PyTypeObject> coro_type = cinderx::getModuleState()->coroType();

  gen_type->tp_repr = PyGen_Type.tp_repr;

  // Fields that can't be set via PyType_Spec
  gen_type->tp_dictoffset = 0;
  gen_type->tp_weaklistoffset = offsetof(PyGenObject, gi_weakreflist);

  coro_type->tp_dictoffset = 0;
  coro_type->tp_weaklistoffset = offsetof(PyGenObject, gi_weakreflist);
  coro_type->tp_as_async = reinterpret_cast<PyAsyncMethods*>(&jitcoro_as_async);

  // Copy globally cached methods
  for (PyMethodDef* gen_methods = PyGen_Type.tp_methods;
       gen_methods->ml_name != nullptr;
       ++gen_methods) {
    if (gen_methods->ml_name == "throw"sv) {
      gen_throw_meth = reinterpret_cast<GenThrowMeth>(gen_methods->ml_meth);
    } else if (gen_methods->ml_name == "close"sv) {
      gen_close_meth = gen_methods->ml_meth;
    } else if (gen_methods->ml_name == "__sizeof__"sv) {
      gen___sizeof___meth = gen_methods->ml_meth;
    }
  }
  JIT_CHECK(
      gen_throw_meth != nullptr && gen_close_meth != nullptr &&
          gen___sizeof___meth != nullptr,
      "Could not find all needed methods in PyGen_Type");

  // Copy get/setters
  auto copy_getset = [](PyGetSetDef* src, PyGetSetDef* target) {
    int i;
    for (i = 0; src[i].name != nullptr; ++i) {
      JIT_CHECK(
          target[i].name != nullptr,
          "Missing getter/setter on JIT generator: {}",
          src[i].name);
      JIT_CHECK(
          std::string_view(target[i].name) == src[i].name,
          "Name mismatch: {} != {}",
          target[i].name,
          src[i].name);
      if (target[i].get == nullptr) {
        target[i].get = src[i].get;
      }
      if (target[i].set == nullptr) {
        target[i].set = src[i].set;
      }
      if (target[i].doc == nullptr) {
        target[i].doc = src[i].doc;
      }
    }
    if (target[i].name != nullptr && strcmp(target[i].name, "__class__") == 0) {
      // Allow extra class which overrides the type to be the CPython type
      // for the purpose of isinstance checks.
      i++;
    }
    JIT_CHECK(target[i].name == nullptr, "Extra name: {}", target[i].name);
  };
  copy_getset(PyGen_Type.tp_getset, gen_type->tp_getset);
  copy_getset(PyCoro_Type.tp_getset, coro_type->tp_getset);

  // Copy methods
  auto copy_methods = [](PyMethodDef* src, PyMethodDef* target) {
    int i;
    for (i = 0; src[i].ml_name != nullptr; ++i) {
      JIT_CHECK(
          target[i].ml_name != nullptr,
          "Missing method on JIT generator: {}",
          src[i].ml_name);
      JIT_CHECK(
          std::string_view(target[i].ml_name) == src[i].ml_name,
          "Name mismatch: {} != {}",
          target[i].ml_name,
          src[i].ml_name);
      if (target[i].ml_meth == nullptr) {
        target[i].ml_meth = src[i].ml_meth;
      }
      JIT_CHECK(
          target[i].ml_flags == src[i].ml_flags,
          "Flags mismatch for {}",
          target[i].ml_name);
      if (target[i].ml_doc == nullptr) {
        target[i].ml_doc = src[i].ml_doc;
      }
    }
    JIT_CHECK(
        target[i].ml_name == nullptr, "Extra name: {}", target[i].ml_name);
  };
  copy_methods(PyGen_Type.tp_methods, gen_type->tp_methods);
  copy_methods(PyCoro_Type.tp_methods, coro_type->tp_methods);

  cinderx::getModuleState()->setJitGenFreeList(new JitGenFreeList());

  // Override dealloc so we can use a "free-list" for our objects.
  JIT_CHECK(
      PyGen_Type.tp_dealloc == original_gen_dealloc &&
          PyCoro_Type.tp_dealloc == original_coro_dealloc,
      "PyGen/Coro_Type already overridden");
  PyGen_Type.tp_dealloc = reinterpret_cast<destructor>(jitgen_dealloc);
  PyCoro_Type.tp_dealloc = reinterpret_cast<destructor>(jitgen_dealloc);

#ifdef ENABLE_GENERATOR_AWAITER
  JIT_CHECK(
      PyCoro_Type.tp_flags & Ci_TPFLAGS_HAVE_AM_EXTRA,
      "No AM_EXTRA on coro type");
  auto coro_ame =
      reinterpret_cast<Ci_AsyncMethodsWithExtra*>(PyCoro_Type.tp_as_async);
  jitcoro_as_async.ame_setawaiter = coro_ame->ame_setawaiter;
#endif
}

void shutdown_jit_genobject_type() {
  PyGen_Type.tp_dealloc = original_gen_dealloc;
  PyCoro_Type.tp_dealloc = original_coro_dealloc;
}

static PyMethodDef anextawaitable_methods[] = {
    {"send", (PyCFunction)Ci_anextawaitable_send, METH_O, ""},
    {"throw", (PyCFunction)Ci_anextawaitable_throw, METH_VARARGS, ""},
#if PY_VERSION_HEX >= 0x030E0000
    {"close", (PyCFunction)Ci_anextawaitable_close, METH_NOARGS, ""},
#else
    {"close", (PyCFunction)Ci_anextawaitable_close, METH_VARARGS, ""},
#endif
    {nullptr, nullptr} /* Sentinel */
};

PyType_Slot anext_awaitable_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(Ci_anextawaitable_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(Ci_anextawaitable_traverse)},
    {Py_tp_methods, anextawaitable_methods},
    {Py_tp_iternext, reinterpret_cast<void*>(Ci_anextawaitable_iternext)},
    {Py_tp_iter, reinterpret_cast<void*>(PyObject_SelfIter)},
    {Py_am_await, reinterpret_cast<void*>(PyObject_SelfIter)},
    {0, nullptr},
};

PyType_Spec JitAnextAwaitable_Spec = {
    .name = "builtins.anext_awaitable",
    .basicsize = sizeof(anextawaitableobject),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = anext_awaitable_slots,
};

PyObject* JitGen_AnextAwaitable_New(
    cinderx::ModuleState* moduleState,
    PyObject* awaitable,
    PyObject* defaultValue) {
  anextawaitableobject* anext =
      PyObject_GC_New(anextawaitableobject, moduleState->anextAwaitableType());
  if (anext == nullptr) {
    return nullptr;
  }
  anext->wrapped = Py_NewRef(awaitable);
  anext->default_value = Py_NewRef(defaultValue);
  PyObject_GC_Track(anext);
  return (PyObject*)anext;
}

} // namespace jit

PyObject* JitGen_yf(PyGenObject* gen) {
  jit::JitGenObject* jit_gen = jit::JitGenObject::cast(gen);
  return jit_gen == nullptr ? _PyGen_yf(gen) : jit_gen->yieldFrom();
}

#endif // PY_VERSION_HEX >= 0x030C0000
