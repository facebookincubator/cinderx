// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include "cinderx/python.h"

#include "cinderx/Common/util.h"
#include "cinderx/StaticPython/typed-args-info.h"

#include <utility>

namespace cinderx::jit {
class CodeRuntime;
struct GenDataFooter;
struct JitGenObject;
} // namespace cinderx::jit

namespace cinderx::jit::rt {

// static->static call convention for primitive returns is to return error flag
// in rdx (null means error occurred); for C helpers that need to implement this
// convention, returning this struct will fill the right registers
struct StaticCallReturn {
  void* rax;
  void* rdx;
};

struct StaticCallFPReturn {
  double xmm0;
  double xmm1;
};

PyThreadState* allocateAndLinkInterpreterFrame_Debug(
    PyFunctionObject* func,
    PyCodeObject* jit_code_object);

PyThreadState* allocateAndLinkInterpreterFrame_Release(PyFunctionObject* func);

std::pair<PyThreadState*, GenDataFooter*> allocateAndLinkGenAndInterpreterFrame(
    PyFunctionObject* func,
    CodeRuntime* code_rt,
    GenResumeFunc resume_entry,
    uint64_t original_frame_pointer);

void initFrameCellVars(
    PyFunctionObject* func,
    int nvars,
    PyThreadState* tstate);

std::pair<JitGenObject*, GenDataFooter*> unlinkGenFrameAndReturnGenDataFooter(
    PyThreadState* tstate);

/*
 * Helper function to decref a frame.
 *
 * Used by unlinkFrame, and designed to only be used separately if
 * something else has already unlinked the frame.
 */
void decrefFrame(PyFrameObject* frame);

/*
 * Helper function to unlink a frame.
 *
 * Designed to be used in tandem with allocateAndLinkFrame. This checks
 * if the frame has escaped (> 1 refcount) and tracks it if so.
 */
void unlinkFrame(PyThreadState* tstate);

/*
 * Handles a call that includes kw arguments where the target function has
 * *args, **kwargs, or keyword only args.
 */
PyObject* callWithKeywordArgs(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames);

/*
 * Handles a call that includes kw arguments where the target function doesn't
 * have *args, **kwargs, or keyword only args.
 */
PyObject* callWithKeywordArgsSimple(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames);

StaticCallReturn callWithIncorrectArgcount(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    int argcount);

StaticCallFPReturn callWithIncorrectArgcountFPReturn(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    int argcount);

/* Helper function to report an error when the arguments aren't correct for
 * a static function call.  Dispatches to the eval loop to let the normal
 * argument checking prologue run and then report the error */
PyObject* reportStaticArgTypecheckErrors(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames);

/* Variation of reportStaticArgTypecheckErrors but also sets the primitive
   return value error in addition to returning the normal NULL error indicator
 */
StaticCallReturn reportStaticArgTypecheckErrorsWithPrimitiveReturn(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames);

/* Variation of reportStaticArgTypecheckErrors but also sets the double
   return value error in addition to returning the normal NULL error indicator
 */
StaticCallFPReturn reportStaticArgTypecheckErrorsWithDoubleReturn(
    PyObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames);

/*
 * Mimics the behavior of _PyDict_LoadGlobal except that it raises an error
 * when the name does not exist.
 */
PyObject* loadGlobal(PyObject* globals, PyObject* builtins, PyObject* name);

/*
 * Load a global value given a Python thread state.
 */
PyObject* loadGlobalFromThreadState(PyThreadState* tstate, PyObject* name);

/*
 * Load the globals dict from a Python thread state.
 */
PyObject* loadGlobalsDict(PyThreadState* tstate);

/*
 * Helper to perform a Python call with dynamically determined arguments.
 *
 * pargs will be a possibly empty tuple of positional arguments, kwargs will be
 * null or a dictionary of keyword arguments.
 */
PyObject* callFunctionEx(PyObject* func, PyObject* pargs, PyObject* kwargs);

/*
 * Perform a function or method call.
 *
 * If it's a method call, then `args[0]` will be the receiver of the method
 * lookup (e.g. `self`). The rest of `args` will be the positional and keyword
 * arguments to the call.
 *
 * If it's a function call, then `callable` will be Py_None and the actual
 * callable will be stored in `args[0]`.  The rest of `args` is then the same as
 * the method case.
 *
 * Note: Technically for the function call case, `callable` should be NULL and
 * not Py_None, but we use NULL return values in HIR to determine where to
 * deopt.
 */
PyObject* call(
    PyThreadState* tstate,
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

/*
 * Performs a function call with a vectorcall. Will check and handle any
 * eval breaker events after the call.
 */
PyObject* vectorcallTstate(
    PyThreadState* tstate,
    PyObject* callable,
    PyObject* const* args,
    size_t nargsf,
    PyObject* kwnames);

/*
 * Perform a method lookup on an object.
 */
LoadMethodResult getMethod(PyObject* obj, PyObject* name);

/*
 * Perform an attribute lookup in a super class
 *
 * This is used to avoid bound method creation for attribute lookups that
 * correspond to method calls (e.g. `self.foo()`).
 */
LoadMethodResult getMethodFromSuper(
    PyObject* global_super,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call);

/*
 * Perform an attribute lookup in a super class
 */
PyObject* getAttrFromSuper(
    PyObject* super_globals,
    PyTypeObject* type,
    PyObject* self,
    PyObject* name,
    bool no_args_in_super_call);

/*
 * Mimics the behavior of the UNARY_NOT opcode.
 *
 * Checks if value is truthy, and returns Py_False if it is, or Py_True if
 * it's not.  Returns NULL if the object doesn't support truthyness.
 */
PyObject* unaryNot(PyObject* value);

/*
 * Invokes a function stored within the method table for the object.
 * The method table lives off tp_cache in the type object
 */
PyObject* invokeMethod(
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargs,
    PyObject* kwnames);
/*
 * Invokes a function stored within the method table for the object.
 * The method table lives off tp_cache of self.
 */
PyObject* invokeClassMethod(
    Py_ssize_t slot,
    PyObject** args,
    Py_ssize_t nargs,
    PyObject* kwnames);

/*
 * Loads an indirect function, optionally loading it from the descriptor
 * if the indirect cache fails.
 */
PyObject* loadFunctionIndirect(PyObject** func, PyObject* descr);

/*
 * Performs a type check on an object, raising an error if the object is
 * not an instance of the specified type.  The type check is a real type
 * check which doesn't support dynamic behaviors against the type or
 * proxy behaviors against obj.__class__
 */
PyObject* cast(PyObject* obj, PyTypeObject* type);

/*
 * cast() when target type is float. This case requires extra work because
 * Python typing pretends int is a subtype of float, so CAST needs to coerce int
 * to float.
 */
PyObject* castToFloat(PyObject* obj);

/*
 * castToFloat() but with None allowed.
 */
PyObject* castToFloatOptional(PyObject* obj);

/*
 * Performs a type check on an object, raising an error if the object is
 * not an instance of the specified type or None.  The type check is a
 * real type check which doesn't support dynamic behaviors against the
 * type or proxy behaviors against obj.__class__.
 */
PyObject* castOptional(PyObject* obj, PyTypeObject* type);

/*
 * Performs a type check on obj, but does not allow passing a subclass of type.
 */
PyObject* castExact(PyObject* obj, PyTypeObject* type);
PyObject* castOptionalExact(PyObject* obj, PyTypeObject* type);

/* Helper methods to implement left shift, which wants its operand in cl */
int64_t shiftLeft64(int64_t x, int64_t y);
int32_t shiftLeft32(int32_t x, int32_t y);

/* Helper methods to implement right shift, which wants its operand in cl */
int64_t shiftRight64(int64_t x, int64_t y);
int32_t shiftRight32(int32_t x, int32_t y);

/* Helper methods to implement unsigned right shift, which wants its operand in
 * cl
 */
uint64_t shiftRightUnsigned64(uint64_t x, uint64_t y);
uint32_t shiftRightUnsigned32(uint32_t x, uint32_t y);

/* Helper methods to implement signed modulus */
int64_t mod64(int64_t x, int64_t y);
int32_t mod32(int32_t x, int32_t y);

/* Helper methods to implement unsigned modulus */
uint64_t modUnsigned64(uint64_t x, uint64_t y);
uint32_t modUnsigned32(uint32_t x, uint32_t y);

PyObject* boxI32(int32_t i);
PyObject* boxU32(uint32_t i);
PyObject* boxBool(uint32_t i);
PyObject* boxI64(int64_t i);
PyObject* boxU64(uint64_t i);
PyObject* boxDouble(double_t d);

double powerDouble(double x, double y);
double sqrtDouble(double x);
double power32(int32_t x, int32_t y);
double powerUnsigned32(uint32_t x, uint32_t y);
double power64(int64_t x, int64_t y);
double powerUnsigned64(uint64_t x, uint64_t y);

uint64_t unboxU64(PyObject* obj);
uint32_t unboxU32(PyObject* obj);
uint16_t unboxU16(PyObject* obj);
uint8_t unboxU8(PyObject* obj);
int64_t unboxI64(PyObject* obj);
int32_t unboxI32(PyObject* obj);
int16_t unboxI16(PyObject* obj);
int8_t unboxI8(PyObject* obj);

/*
 * Calls __builtins__.__import__(), with a fast-path if this hasn't been
 * overridden.
 *
 * This is a near verbatim copy of import_name() from ceval.c with minor
 * tweaks. We copy rather than expose to avoid making changes to ceval.c.
 */
PyObject* importName(
    PyThreadState* tstate,
    PyObject* name,
    PyObject* fromlist,
    PyObject* level);

#if PY_VERSION_HEX >= 0x030F0000
/*
 * Implements IMPORT_FROM for 3.15+. Mirrors the interpreter: if `from` is an
 * unresolved lazy import, the attribute load goes through the lazy-import path
 * (which may create a chained lazy import); otherwise it behaves like a normal
 * import-from. `frame` is only used to record the import location for any
 * chained lazy import that gets created.
 */
PyObject* importFrom(
    PyThreadState* tstate,
    struct _PyInterpreterFrame* frame,
    PyObject* from,
    PyObject* name);
#endif

/*
 * Formats a f-string value
 */
PyObject* formatValue(
    PyThreadState* tstate,
    PyObject* fmt_spec,
    PyObject* value,
    int conversion);
/*
 * Concatenate strings from args
 */
PyObject* buildString(
    PyThreadState* /*tstate*/,
    void* /*unused*/,
    PyObject** args,
    size_t nargsf,
    void* /*unused*/);

// Set the awaiter of the given awaitable to be the coroutine at the top of
// `ts`.
void setCurrentAwaiter(PyObject* awaitable, PyThreadState* ts);

// Mostly the same implementation as YIELD_FROM in ceval.c with slight tweaks to
// make it stand alone. The argument 'v' is stolen.
//
// The arguments 'gen', 'v', 'finish_yield_from' must match positions with JIT
// resume entry function (GenResumeFunc) so registers with their values pass
// straight through.
struct GenSendRes {
  PyObject* retval;
  uint64_t done;
};
GenSendRes genSend(
    PyObject* gen,
    PyObject* v,
    uint64_t finish_yield_from,
    _PyInterpreterFrame* frame);

// Used for the `YIELD_FROM` that appears in the bytecode of the header for
// an `async for` loop.
//
// This is identical to genSend() with the addition that it detects when
// PyExc_StopAsyncIteration has been raised. In such cases the function clears
// the error and returns a sentinel value indicating that iteration has
// finished.
GenSendRes genSendHandleStopAsyncIteration(
    PyObject* gen,
    PyObject* v,
    uint64_t finish_yield_from,
    _PyInterpreterFrame* frame);

/* Unpack a sequence as in unpack_iterable(), and save the
 * results in a tuple.
 */
PyObject* unpackExToTuple(
    PyThreadState* tstate,
    PyObject* iterable,
    int before,
    int after);

/* Unpack a sequence of exactly 'count' items via the iterator protocol.
 * Used as the slow path for UNPACK_SEQUENCE when the sequence is not a
 * list or tuple.
 *
 * On success, fills items[0..count-1] with new references and returns 0.
 * On error, sets a Python exception and returns -1. Any partially-filled
 * items are cleaned up before returning.
 */
int unpackSequence(
    PyThreadState* tstate,
    PyObject* iterable,
    PyObject** items,
    int count);

/* When compiling a fully-typed JIT static -> static call we sometimes
 * optimistically assume the target will be JIT compiled too. If the target
 * fails to compile we point the call to this function which converts the static
 * arguments into a form suitable for a regular Python vector call. Much of the
 * work in this function would have to be done anyway if we were initially
 * making a JIT static -> non-JIT static function anyway, so there is not too
 * much overhead.
 *
 * The function object is obtained from args[0] since in the static calling
 * convention the function is always the first argument.
 */
StaticCallReturn failedDeferredCompileShim(PyObject** args);

StaticCallReturn callStaticallyWithPrimitiveSignature(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info);

StaticCallFPReturn callStaticallyWithPrimitiveSignatureFP(
    PyFunctionObject* func,
    PyObject** args,
    size_t nargsf,
    PyObject* kwnames,
    _PyTypedArgsInfo* arg_info);

/* Compares if one unicode object is equal to another object.
 * At least one of the objects has to be exactly a unicode
 * object.
 */
int unicodeEquals(PyObject* s1, PyObject* s2, int equals);

/* Return Py_True if needle is in haystack else return Py_False. Return nullptr
 * with exception raised on error. */
PyObject* sequenceContains(PyObject* haystack, PyObject* needle);

/* Return Py_True if needle is not in haystack else return Py_False. Return
 * nullptr with exception raised on error. */
PyObject* sequenceNotContains(PyObject* haystack, PyObject* needle);

/* Inverse form of PySequence_Contains for "not in"
 */
int notContainsBool(PyObject* w, PyObject* v);

/* Perform a rich comparison with integer result.  This wraps
   PyObject_RichCompare(), returning -1 for error, 0 for false, 1 for true.
   Unlike PyObject_RichCompareBool this doesn't perform an object equality
   check, which is incompatible w/ float comparisons. */

int richCompareBool(PyObject* v, PyObject* w, int op);

/* perform a batch decref to the objects in args */
void batchDecref(TaggedPyObject* args, int nargs);

/* Check that `i` is within the bounds of `seq`.
 *
 * A negative value of `i` is an index relative to the end of the
 * sequence (e.g. -1 refers to the last element in the sequence).
 *
 * Returns 0-based index that `i` refers to on success.
 * Returns -1 and raises IndexError on error.
 **/
Py_ssize_t checkSequenceBounds(PyObject* seq, Py_ssize_t i);

/* Call obj.__len__(). Return LongExact on success or NULL with an exception
 * set if there was an error. */
PyObject* getLength(PyObject* obj);

/* Call match_keys() in ceval.c
 * NOTE: This function is here as a wrapper around the private match_keys
 * function and should be removed when match_keys becomes public.
 */
PyObject* matchKeys(PyThreadState* tstate, PyObject* subject, PyObject* keys);

/* Used by DICT_UPDATE and DICT_MERGE implementations. */
int dictUpdate(PyThreadState* tstate, PyObject* dict, PyObject* update);
int dictMerge(
    PyThreadState* tstate,
    PyObject* dict,
    PyObject* update,
    PyObject* func);

/* Returns nullptr on error and an exact dict otherwise. Used by
 * COPY_DICT_WITHOUT_KEYS implementation. */
PyObject* copyDictWithoutKeys(PyObject* subject, PyObject* keys);

/*
 * Load a name from a Python thread's code object.
 */
PyObject* loadName(PyThreadState* tstate, int name_idx);

/*
 * Reimplements the format_awaitable_error() function from the CPython
 * interpreter loop. */
void formatAwaitableError(
    PyThreadState* tstate,
    PyTypeObject* type,
    bool is_aenter);

void incRefTotal();
void decRefTotal();

PyObject*
lookupAttrSpecial(PyObject* obj, PyObject* attr, const char* failure_fmt_str);

LoadMethodResult loadSpecial(PyObject* self, int special_idx);

/*
 * Non-inline wrapper for _Py_qsbr_quiescent_state(). This is only meaningful
 * in free-threaded builds; GIL builds compile a no-op so callers can branch on
 * kFreeThreadedBuild instead of #ifdefs.
 */
void atQuiescentState(PyThreadState* tstate);

/*
 * Atomically increment the shared refcount. Called from the inline incref
 * slow path when ob_tid doesn't match (object not owned by current thread).
 *
 * Used by FT builds only.
 */
void incRefShared(PyObject* obj);

/*
 * If the object uses deferred reference counting, return a value with the
 * deferred stack-ref tag without touching the refcount. Otherwise return an
 * untagged pointer value. Used for values loaded from caches where the
 * reference kind isn't statically known.
 *
 * Used by FT builds only.
 */
TaggedPyObject tagIfDeferred(PyObject* obj);

/*
 * A PyObject that is used to indicate that an iterator has finished
 * normally. This must never escape into managed code.
 */
extern PyObject iterDoneSentinel;

/*
 * Invoke __next__ on iterator.
 * Returns the next value, or iterDoneSentinel if the iterator is done.
 */
PyObject* invokeIterNext(PyObject* iterator);

} // namespace cinderx::jit::rt

#if PY_VERSION_HEX >= 0x030D0000

// Implemented in Jit/cell_helpers.c, so these need C linkage. See that file for
// why they can't live in jit_rt.cpp.
extern "C" {

/*
 * Atomically load cell value with new reference (for LOAD_DEREF).
 */
PyObject* cx_load_cell_item(PyCellObject* cell);

/*
 * Atomically swap cell value, returns old value for decref (for STORE_DEREF).
 */
PyObject* cx_swap_cell_item(PyCellObject* cell, PyObject* new_value);

} // extern "C"

#endif
