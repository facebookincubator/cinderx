// Copyright (c) Meta Platforms, Inc. and affiliates.

// clang-format off

#define CINDERX_INTERPRETER
#define NEED_OPCODE_TABLES

#define _PyOpcode_Deopt _Ci_Opcode_Deopt
#define _PyOpcode_Caches _Ci_Opcode_Caches
#define _PyOpcode_Jump _Ci_Opcode_Jump

#include "cinderx/Interpreter/Includes/ceval.c"

/* _PyEval_EvalFrameDefault() is a *big* function,
 * so consume 3 units of C stack */
#define PY_EVAL_C_STACK_UNITS 2

PyObject* _Py_HOT_FUNCTION
Ci_EvalFrame(PyThreadState *tstate, _PyInterpreterFrame *frame, int throwflag)
{
    _Py_EnsureTstateNotNULL(tstate);
    CALL_STAT_INC(pyeval_calls);

#if USE_COMPUTED_GOTOS
/* Import the static jump table */
#include "cinderx/Interpreter/cinderx_opcode_targets.h"
#endif

#ifdef Py_STATS
    int lastopcode = 0;
#endif
    // opcode is an 8-bit value to improve the code generated by MSVC
    // for the big switch below (in combination with the EXTRA_CASES macro).
    uint8_t opcode;        /* Current opcode */
    int oparg;         /* Current opcode argument, if any */
#ifdef LLTRACE
    int lltrace = 0;
#endif

    _PyCFrame cframe;
    _PyInterpreterFrame  entry_frame;
    PyObject *kwnames = NULL; // Borrowed reference. Reset by CALL instructions.

    /* WARNING: Because the _PyCFrame lives on the C stack,
     * but can be accessed from a heap allocated object (tstate)
     * strict stack discipline must be maintained.
     */
    _PyCFrame *prev_cframe = tstate->cframe;
    cframe.previous = prev_cframe;
    tstate->cframe = &cframe;

    assert(tstate->interp->interpreter_trampoline != NULL);
#ifdef Py_DEBUG
    /* Set these to invalid but identifiable values for debugging. */
    entry_frame.f_funcobj = (PyObject*)0xaaa0;
    entry_frame.f_locals = (PyObject*)0xaaa1;
    entry_frame.frame_obj = (PyFrameObject*)0xaaa2;
    entry_frame.f_globals = (PyObject*)0xaaa3;
    entry_frame.f_builtins = (PyObject*)0xaaa4;
#endif
    entry_frame.f_code = tstate->interp->interpreter_trampoline;
    entry_frame.prev_instr =
        _PyCode_CODE(tstate->interp->interpreter_trampoline);
    entry_frame.stacktop = 0;
    entry_frame.owner = FRAME_OWNED_BY_CSTACK;
    entry_frame.return_offset = 0;
    /* Push frame */
    entry_frame.previous = prev_cframe->current_frame;
    frame->previous = &entry_frame;
    cframe.current_frame = frame;

    tstate->c_recursion_remaining -= (PY_EVAL_C_STACK_UNITS - 1);
    if (_Py_EnterRecursiveCallTstate(tstate, "")) {
        tstate->c_recursion_remaining--;
        tstate->py_recursion_remaining--;
        goto exit_unwind;
    }

    /* support for generator.throw() */
    if (throwflag) {
        if (_Py_EnterRecursivePy(tstate)) {
            goto exit_unwind;
        }
        /* Because this avoids the RESUME,
         * we need to update instrumentation */
        _Py_Instrument(frame->f_code, tstate->interp);
        monitor_throw(tstate, frame, frame->prev_instr);
        /* TO DO -- Monitor throw entry. */
        goto resume_with_error;
    }

    /* Local "register" variables.
     * These are cached values from the frame and code object.  */

    _Py_CODEUNIT *next_instr;
    PyObject **stack_pointer;

/* Sets the above local variables from the frame */
#define SET_LOCALS_FROM_FRAME() \
    assert(_PyInterpreterFrame_LASTI(frame) >= -1); \
    /* Jump back to the last instruction executed... */ \
    next_instr = frame->prev_instr + 1; \
    stack_pointer = _PyFrame_GetStackPointer(frame);

start_frame:
    if (_Py_EnterRecursivePy(tstate)) {
        goto exit_unwind;
    }

resume_frame:
    SET_LOCALS_FROM_FRAME();

#ifdef LLTRACE
    {
        if (frame != &entry_frame) {
            int r = PyDict_Contains(GLOBALS(), &_Py_ID(__lltrace__));
            if (r < 0) {
                goto exit_unwind;
            }
            lltrace = r;
        }
        if (lltrace) {
            lltrace_resume_frame(frame);
        }
    }
#endif

#ifdef Py_DEBUG
    /* _PyEval_EvalFrameDefault() must not be called with an exception set,
       because it can clear it (directly or indirectly) and so the
       caller loses its exception */
    assert(!_PyErr_Occurred(tstate));
#endif

    DISPATCH();

handle_eval_breaker:

    /* Do periodic things, like check for signals and async I/0.
     * We need to do reasonably frequently, but not too frequently.
     * All loops should include a check of the eval breaker.
     * We also check on return from any builtin function.
     *
     * ## More Details ###
     *
     * The eval loop (this function) normally executes the instructions
     * of a code object sequentially.  However, the runtime supports a
     * number of out-of-band execution scenarios that may pause that
     * sequential execution long enough to do that out-of-band work
     * in the current thread using the current PyThreadState.
     *
     * The scenarios include:
     *
     *  - cyclic garbage collection
     *  - GIL drop requests
     *  - "async" exceptions
     *  - "pending calls"  (some only in the main thread)
     *  - signal handling (only in the main thread)
     *
     * When the need for one of the above is detected, the eval loop
     * pauses long enough to handle the detected case.  Then, if doing
     * so didn't trigger an exception, the eval loop resumes executing
     * the sequential instructions.
     *
     * To make this work, the eval loop periodically checks if any
     * of the above needs to happen.  The individual checks can be
     * expensive if computed each time, so a while back we switched
     * to using pre-computed, per-interpreter variables for the checks,
     * and later consolidated that to a single "eval breaker" variable
     * (now a PyInterpreterState field).
     *
     * For the longest time, the eval breaker check would happen
     * frequently, every 5 or so times through the loop, regardless
     * of what instruction ran last or what would run next.  Then, in
     * early 2021 (gh-18334, commit 4958f5d), we switched to checking
     * the eval breaker less frequently, by hard-coding the check to
     * specific places in the eval loop (e.g. certain instructions).
     * The intent then was to check after returning from calls
     * and on the back edges of loops.
     *
     * In addition to being more efficient, that approach keeps
     * the eval loop from running arbitrary code between instructions
     * that don't handle that well.  (See gh-74174.)
     *
     * Currently, the eval breaker check happens here at the
     * "handle_eval_breaker" label.  Some instructions come here
     * explicitly (goto) and some indirectly.  Notably, the check
     * happens on back edges in the control flow graph, which
     * pretty much applies to all loops and most calls.
     * (See bytecodes.c for exact information.)
     *
     * One consequence of this approach is that it might not be obvious
     * how to force any specific thread to pick up the eval breaker,
     * or for any specific thread to not pick it up.  Mostly this
     * involves judicious uses of locks and careful ordering of code,
     * while avoiding code that might trigger the eval breaker
     * until so desired.
     */
    if (_Py_HandlePending(tstate) != 0) {
        goto error;
    }
    DISPATCH();

    {
    /* Start instructions */
#if !USE_COMPUTED_GOTOS
    dispatch_opcode:
        switch (opcode)
#endif
        {

#include "cinderx/Interpreter/Includes/generated_cases.c.h"

    /* INSTRUMENTED_LINE has to be here, rather than in bytecodes.c,
     * because it needs to capture frame->prev_instr before it is updated,
     * as happens in the standard instruction prologue.
     */
#if USE_COMPUTED_GOTOS
        TARGET_INSTRUMENTED_LINE:
#else
        case INSTRUMENTED_LINE:
#endif
    {
        _Py_CODEUNIT *prev = frame->prev_instr;
        _Py_CODEUNIT *here = frame->prev_instr = next_instr;
        _PyFrame_SetStackPointer(frame, stack_pointer);
        int original_opcode = _Py_call_instrumentation_line(
                tstate, frame, here, prev);
        stack_pointer = _PyFrame_GetStackPointer(frame);
        if (original_opcode < 0) {
            next_instr = here+1;
            goto error;
        }
        next_instr = frame->prev_instr;
        if (next_instr != here) {
            DISPATCH();
        }
        if (_PyOpcode_Caches[original_opcode]) {
            _PyBinaryOpCache *cache = (_PyBinaryOpCache *)(next_instr+1);
            /* Prevent the underlying instruction from specializing
             * and overwriting the instrumentation. */
            INCREMENT_ADAPTIVE_COUNTER(cache->counter);
        }
        opcode = original_opcode;
        DISPATCH_GOTO();
    }


#if USE_COMPUTED_GOTOS
        _unknown_opcode:
#else
        EXTRA_CASES  // From opcode.h, a 'case' for each unused opcode
#endif
            /* Tell C compilers not to hold the opcode variable in the loop.
               next_instr points the current instruction without TARGET(). */
            opcode = next_instr->op.code;
            _PyErr_Format(tstate, PyExc_SystemError,
                          "%U:%d: unknown opcode %d",
                          frame->f_code->co_filename,
                          PyUnstable_InterpreterFrame_GetLine(frame),
                          opcode);
            goto error;

        } /* End instructions */

        /* This should never be reached. Every opcode should end with DISPATCH()
           or goto error. */
        Py_UNREACHABLE();

unbound_local_error:
        {
            format_exc_check_arg(tstate, PyExc_UnboundLocalError,
                UNBOUNDLOCAL_ERROR_MSG,
                PyTuple_GetItem(frame->f_code->co_localsplusnames, oparg)
            );
            goto error;
        }

pop_4_error:
    STACK_SHRINK(1);
pop_3_error:
    STACK_SHRINK(1);
pop_2_error:
    STACK_SHRINK(1);
pop_1_error:
    STACK_SHRINK(1);
error:
        kwnames = NULL;
        /* Double-check exception status. */
#ifdef NDEBUG
        if (!_PyErr_Occurred(tstate)) {
            _PyErr_SetString(tstate, PyExc_SystemError,
                             "error return without exception set");
        }
#else
        assert(_PyErr_Occurred(tstate));
#endif

        /* Log traceback info. */
        assert(frame != &entry_frame);
        if (!_PyFrame_IsIncomplete(frame)) {
            PyFrameObject *f = _PyFrame_GetFrameObject(frame);
            if (f != NULL) {
                PyTraceBack_Here(f);
            }
        }
        monitor_raise(tstate, frame, next_instr-1);
exception_unwind:
        {
            /* We can't use frame->f_lasti here, as RERAISE may have set it */
            int offset = INSTR_OFFSET()-1;
            int level, handler, lasti;
            if (get_exception_handler(frame->f_code, offset, &level, &handler, &lasti) == 0) {
                // No handlers, so exit.
                assert(_PyErr_Occurred(tstate));

                /* Pop remaining stack entries. */
                PyObject **stackbase = _PyFrame_Stackbase(frame);
                while (stack_pointer > stackbase) {
                    PyObject *o = POP();
                    Py_XDECREF(o);
                }
                assert(STACK_LEVEL() == 0);
                _PyFrame_SetStackPointer(frame, stack_pointer);
                monitor_unwind(tstate, frame, next_instr-1);
                goto exit_unwind;
            }

            assert(STACK_LEVEL() >= level);
            PyObject **new_top = _PyFrame_Stackbase(frame) + level;
            while (stack_pointer > new_top) {
                PyObject *v = POP();
                Py_XDECREF(v);
            }
            if (lasti) {
                int frame_lasti = _PyInterpreterFrame_LASTI(frame);
                PyObject *lasti = PyLong_FromLong(frame_lasti);
                if (lasti == NULL) {
                    goto exception_unwind;
                }
                PUSH(lasti);
            }

            /* Make the raw exception data
                available to the handler,
                so a program can emulate the
                Python main loop. */
            PyObject *exc = _PyErr_GetRaisedException(tstate);
            PUSH(exc);
            JUMPTO(handler);
            if (monitor_handled(tstate, frame, next_instr, exc) < 0) {
                goto exception_unwind;
            }
            /* Resume normal execution */
            DISPATCH();
        }
    }

exit_unwind:
    assert(_PyErr_Occurred(tstate));
    _Py_LeaveRecursiveCallPy(tstate);
    assert(frame != &entry_frame);
    // GH-99729: We need to unlink the frame *before* clearing it:
    _PyInterpreterFrame *dying = frame;
    frame = cframe.current_frame = dying->previous;
    _PyEvalFrameClearAndPop(tstate, dying);
    frame->return_offset = 0;
    if (frame == &entry_frame) {
        /* Restore previous cframe and exit */
        tstate->cframe = cframe.previous;
        assert(tstate->cframe->current_frame == frame->previous);
        tstate->c_recursion_remaining += PY_EVAL_C_STACK_UNITS;
        return NULL;
    }

resume_with_error:
    SET_LOCALS_FROM_FRAME();
    goto error;

}

// clang-format on
