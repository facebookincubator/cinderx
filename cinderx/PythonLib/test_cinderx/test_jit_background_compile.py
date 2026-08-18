# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import collections
import gc
import os
import signal
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
import weakref
from pathlib import Path
from types import FunctionType
from typing import Any, Callable, cast

import cinderx.jit
from cinderx.test_support import (
    ENCODING,
    failUnlessJITCompiled,
    is_sanitizer_build,
    passUnless,
    skip_if_ft,
    skip_if_prefork,
    skip_unless_jit,
    subprocess_env,
)


class AttrLoadType:
    """Module-level type used to give the JIT an exact receiver type so
    LoadAttr on an instance goes through simplifyLoadAttrInstanceReceiver."""

    cls_attr = 123

    def __init__(self) -> None:
        self.inst_attr = 7


from .test_jit_pipeline_stress import make_function


def _wait_for_jit_compile(func: Callable[..., object], timeout: float = 5.0) -> bool:
    """Poll is_jit_compiled until true or timeout. Returns True if compiled."""
    start = time.time()
    while time.time() - start < timeout:
        if cinderx.jit.is_jit_compiled(func):
            return True
        time.sleep(0.01)
    failUnlessJITCompiled(func)
    return cinderx.jit.is_jit_compiled(func)


class BackgroundCompileTest(unittest.TestCase):
    def setUp(self) -> None:
        # Enable background compilation for testing
        self._old_background_compile = cinderx.jit.get_background_compile()
        cinderx.jit.background_compile(True)
        # If we were running w/ background compiles enabled we may have a queue
        # before the test starts, go ahead and drain it.
        cinderx.jit.wait_for_background_compiles()

    def tearDown(self) -> None:
        cinderx.jit.background_compile(self._old_background_compile)
        gc.collect()

    def test_background_compile_basic(self) -> None:
        """Basic smoke test: function gets JIT compiled in background."""

        def test_func(x: int) -> int:
            return x * 2 + 1

        # First call triggers background compile, runs interpreted
        result1 = test_func(5)
        self.assertEqual(result1, 11)

        # Wait for background compile to finish
        _wait_for_jit_compile(test_func, timeout=2.0)

        # Function should now be JIT compiled and still work correctly
        self.assertTrue(cinderx.jit.is_jit_compiled(test_func))
        result2 = test_func(10)
        self.assertEqual(result2, 21)

    @passUnless(hasattr(os, "fork"), "requires os.fork()")
    def test_fork_after_background_compile(self) -> None:
        """Forking once the background compile worker thread exists must leave
        a usable child.

        The worker does not survive the fork, but the child still inherits its
        std::thread handle in a joinable state and the registry mutex locked by
        the pthread_atfork prepare handler.  Running the registry's destructor
        in the child would abort in ~thread(); the child has to reset the
        registry without destroying the inherited one.

        Runs out-of-process so the auto-compile threshold can be set low enough
        to guarantee the compile goes through the background worker rather than
        being forced onto this thread.
        """

        with tempfile.TemporaryDirectory() as tmp_dir:
            code = textwrap.dedent("""
            import os
            import sys
            import time

            import cinderx.jit

            def parent_func(x):
                return x * 3 + 1

            for i in range(2000):
                parent_func(i)

            deadline = time.time() + 10
            while time.time() < deadline and not cinderx.jit.is_jit_compiled(
                parent_func
            ):
                time.sleep(0.01)
            # The worker thread only exists if a background compile ran.
            assert cinderx.jit.is_jit_compiled(parent_func)

            pid = os.fork()
            if pid == 0:
                # Compile in the child too: it should start its own worker off
                # the freshly reset registry, not the dead inherited one.
                status = 0
                try:
                    def child_func(y):
                        return y + 4

                    for i in range(2000):
                        child_func(i)
                    if child_func(1) != 5:
                        status = 1
                except Exception:
                    status = 2
                os._exit(status)

            _, status = os.waitpid(pid, 0)
            if not os.WIFEXITED(status):
                sys.exit(f"child died from signal {os.WTERMSIG(status)}")
            sys.exit(os.WEXITSTATUS(status))
            """)

            test_file = Path(tmp_dir) / "mod.py"
            test_file.write_text(code)

            subprocess.run(
                [sys.executable, str(test_file)],
                check=True,
                env={
                    **subprocess_env(),
                    "CINDERX_JIT_AUTO": "200",
                    "CINDERX_JIT_BACKGROUND_COMPILE": "1",
                },
            )

    def test_delete_function_during_background_compile(self) -> None:
        """Test that deleting a function object during background compilation
        doesn't crash.  The BackgroundCompileTask holds a Ref to the
        function, keeping it alive across the GIL-free compile, even if Python
        code drops all references.

        This exercises:
        - BackgroundCompileTask func_ref keeping PyFunctionObject alive
        - Preloader code/builtins/globals Ref<> keeping code objects alive
        - Function::~Function resetting code/builtins/globals/reifier under
          ThreadedCompileGILHolder guard
        """
        # Create function via exec so we can delete it from globals easily
        PARAMS = 8
        target_func = make_function(
            "target_func", segments=8, num_vars=PARAMS, nest_depth=4
        )
        assert isinstance(target_func, FunctionType)

        func_args = ([1, 2.0, "s", b"b", [1], (1,), {1: 1}, {1}] * 4, *([0] * PARAMS))
        # Trigger background compile
        result1 = target_func(*func_args)
        self.assertEqual(result1, 19167)

        # Immediately delete all Python references to the function
        # The background compile task should keep it alive via its Ref
        func_weak = weakref.ref(target_func)
        del target_func
        gc.collect()

        # Give background compile time to run with no Python references held
        # (if background_compile is disabled, this is a no-op)
        time.sleep(0.1)
        gc.collect()

        # Function object may still be alive due to BackgroundCompileTask holding
        # a Ref, or it may be dead if compilation finished quickly and
        # no other references exist - either is fine, we just must not crash.
        # The important thing is that the background compile didn't crash
        # accessing freed memory.
        #
        # We can't call the function anymore (we deleted our reference),
        # but we can check that the process is still alive and no crash occurred.
        # If the weakref is alive, verify the function still works.
        func_obj = func_weak()
        if func_obj is not None:
            # Function survived, check if it got JIT compiled
            if cinderx.jit.is_jit_compiled(func_obj):
                result2 = func_obj(*func_args)
                self.assertEqual(result2, result1)

    @skip_if_prefork(
        "prefork immortalizes compiled functions, so exec'ing a fresh code "
        "object each refleak iteration leaks the pinned type/code/runtime"
    )
    def test_delete_type_during_background_compile(self) -> None:
        """Test deleting a type object referenced by a function being
        background-compiled.  Preloader::types_ holds OwnedType strong references
        keeping PyTypeObjects alive during compilation, and GuardType lowering
        pins type objects in CodeRuntime for the lifetime of compiled code.

        This exercises:
        - Preloader::types_ OwnedType keeping PyTypeObject alive during compile
        - GuardType CodeRuntime::addReference keeping type alive at runtime
        - TypedArgument pytype kept alive by the Preloader across the compile
        """
        namespace: dict[str, Any] = {}
        exec(
            """
class MyCustomType:
    value = 42

def target_func(obj):
    # Type guard will be emitted for MyCustomType attribute access
    # Preloader will preload MyCustomType
    return obj.value + 1
""",
            namespace,
        )
        target_func = cast(FunctionType, namespace["target_func"])
        my_type = cast(Any, namespace["MyCustomType"])
        assert isinstance(target_func, FunctionType)

        # Create instance and call once to trigger background compile
        result1 = target_func(my_type())
        self.assertEqual(result1, 43)

        # Delete the type object while background compile is likely running
        # Preloader should keep it alive via OwnedType during compile,
        # and GuardType should pin it in CodeRuntime for runtime
        del namespace["MyCustomType"]
        del my_type
        gc.collect()

        # Wait for background compile to finish - otherwise calling target_func
        # would run interpreted code which needs MyCustomType global for
        # isinstance checks / attribute access type guards that haven't been
        # compiled yet, causing NameError / crash
        _wait_for_jit_compile(target_func, timeout=2.0)

        gc.collect()

        # Function should still work with JIT-compiled code, even though the
        # type object was deleted from Python globals.  CodeRuntime keeps the
        # PyTypeObject alive, preventing UAF / type confusion.
        class MyCustomType:
            value = 42

        result2 = target_func(MyCustomType())
        self.assertEqual(result2, 43)

    @skip_if_prefork(
        "prefork immortalizes compiled functions, so exec'ing a fresh code "
        "object each refleak iteration leaks the pinned callee/code/runtime"
    )
    def test_delete_callee_during_background_compile(self) -> None:
        """Test deleting a callee function while caller is being background-
        compiled.  The inliner keeps callee alive during compilation via
        Environment::addReference, and LIRGenerator pins callee in CodeRuntime
        for the lifetime of the caller compiled code.

        Exercises:
        - Inliner callee pinning (addReference during HIR build)
        - CodeRuntime::addReference(func) in LIRGenerator for InvokeStaticFunction
        - Prevents use-after-free surfacing as code_dealloc from _CiStaticEval_Vector
        """
        namespace: dict[str, Any] = {}
        exec(
            """
def callee_func(x):
    return x * 3 + 7

def caller_func(y):
    # Call callee - should be inlined or statically invoked,
    # triggering the callee pinning logic
    total = 0
    for i in range(50):
        total += callee_func(y + i)
    return total
""",
            namespace,
        )
        caller_func = cast(FunctionType, namespace["caller_func"])
        callee_func = cast(FunctionType, namespace["callee_func"])

        # Call once to trigger background compile of caller_func
        # (callee_func will be preloaded/inlined)
        result1 = caller_func(10)
        self.assertIsInstance(result1, int)

        # Delete callee while caller is background compiling
        # Inliner / CodeRuntime should keep it alive
        del namespace["callee_func"]
        del callee_func
        gc.collect()

        # Wait for background compile to finish before calling again.
        # Otherwise interpreter will try to LOAD_GLOBAL callee_func and fail
        # with NameError since we deleted it from globals.
        # With JIT compiled code, callee is pinned in CodeRuntime, so call
        # succeeds even though global is gone.
        _wait_for_jit_compile(caller_func, timeout=2.0)
        gc.collect()

        # Caller should still work correctly - callee kept alive by CodeRuntime
        try:
            result2 = caller_func(10)
            self.assertEqual(result2, result1)
        except NameError as ex:
            self.assertEqual(ex.name, "callee_func")

    @skip_if_prefork(
        "prefork immortalizes compiled functions, so exec'ing a fresh code "
        "object each refleak iteration leaks the pinned globals/code/runtime"
    )
    def test_delete_global_during_background_compile(self) -> None:
        """Test deleting a global variable referenced by a function during
        background compilation.  Preloader::global_names_ and global_caches_
        capture globals during preload (with GIL held), avoiding races during
        HIR building with GIL released.
        """
        namespace: dict[str, Any] = {}
        exec(
            """
GLOBAL_VALUE = 12345

def target_func(x):
    # Access global - Preloader preloads globals
    return x + GLOBAL_VALUE
""",
            namespace,
        )
        target_func = cast(FunctionType, namespace["target_func"])

        result1 = target_func(5)
        self.assertEqual(result1, 12350)

        # Delete global while background compile is running
        del namespace["GLOBAL_VALUE"]
        gc.collect()

        # Wait for background compile to finish.  Interpreter would fail with
        # NameError if global is missing, but JIT-compiled code with cached
        # globals should still work (global value was captured during preload).
        _wait_for_jit_compile(target_func, timeout=10.0)
        gc.collect()

        # Function should still work with JIT code, even though global was
        # deleted from module namespace.  Must not crash with UAF.
        try:
            target_func(5)
        except NameError as ex:
            self.assertEqual(ex.name, "GLOBAL_VALUE")

    @skip_if_prefork(
        "prefork immortalizes compiled functions, so exec'ing a fresh code "
        "object each refleak iteration leaks the pinned type/code/runtime"
    )
    def test_delete_annotation_type_during_background_compile(self) -> None:
        """Test deleting a type used in function annotations during background
        compilation.  Preloader::return_type_owned_ keeps return-type spec
        PyTypeObject alive across compile, and annotation processing is done
        during preload with GIL held.
        """
        namespace: dict[str, Any] = {}
        exec(
            """
class ReturnType:
    pass

def annotated_func(x: int) -> ReturnType:
    # Function with return type annotation - type is resolved during preload
    # and kept alive via return_type_owned_, even with GIL released
    # Body does NOT reference ReturnType at runtime, so deleting the global
    # won't break interpreter execution - we're testing that the compiler
    # doesn't UAF the type object during background compilation
    return x * 2
""",
            namespace,
        )
        annotated_func = cast(FunctionType, namespace["annotated_func"])
        return_type = cast(Any, namespace["ReturnType"])

        result1 = annotated_func(42)
        self.assertEqual(result1, 84)

        # Delete annotation type during background compile
        del namespace["ReturnType"]
        del return_type
        gc.collect()

        # Wait for background compile to finish
        _wait_for_jit_compile(annotated_func, timeout=2.0)
        gc.collect()

        # Function should still work, no crash from UAF of annotation type
        # during compilation
        result2 = annotated_func(99)
        self.assertEqual(result2, 198)

    @skip_if_prefork(
        "prefork immortalizes compiled functions, so exec'ing a fresh code "
        "object each refleak iteration leaks the pinned type/code/runtime"
    )
    def test_guard_type_target_pinning(self) -> None:
        """Test that PyTypeObjects used in GuardType instructions are kept alive
        for the lifetime of compiled code via CodeRuntime::addReference.
        Without pinning, type object UAF leads to type confusion, e.g.,
        coroutine type being replaced by generator type in memory, causing
        anext() to fail with "'generator' object is not an async iterator".

        This test deletes a type used in a type guard during/after background
        compilation and verifies no type confusion occurs.
        """
        namespace: dict[str, Any] = {}
        exec(
            """
class GuardedType:
    attr = 100

def target_func(obj):
    # Type guard will be emitted - if obj is GuardedType, access attr,
    # else deopt
    # Make it a bit complex to ensure GuardType is emitted
    total = 0
    for i in range(20):
        if isinstance(obj, GuardedType):
            total += obj.attr + i
        else:
            total += i
    return total
""",
            namespace,
        )
        target_func = cast(FunctionType, namespace["target_func"])
        guarded_type = cast(Any, namespace["GuardedType"])

        result1 = target_func(guarded_type())
        self.assertIsInstance(result1, int)

        # Wait for background compile to finish BEFORE deleting type.
        # Otherwise interpreter will try isinstance(obj, GuardedType) with
        # GuardedType global missing -> NameError.
        # With JIT compiled code, GuardType uses constant type pointer pinned
        # in CodeRuntime, no global lookup needed.
        _wait_for_jit_compile(target_func, timeout=2.0)
        gc.collect()

        # Now delete the type object - if GuardType target is NOT pinned in
        # CodeRuntime, this frees the PyTypeObject, leading to UAF when guard
        # runs (type confusion / crash)
        del namespace["GuardedType"]
        del guarded_type
        gc.collect()

        # Call function many times - if type guard UAF exists, we may get
        # type confusion, crash, or wrong results
        # With proper pinning, type stays alive via CodeRuntime reference,
        # guard works correctly
        class GuardedType:
            attr = 100

        for _ in range(10):
            try:
                result = target_func(GuardedType())
                self.assertEqual(result, result1)
            except NameError as ex:
                self.assertEqual(ex.name, "GuardedType")

        # Type may still be alive due to CodeRuntime pinning - that's correct!
        # The important thing is no crash / type confusion occurred

    # --- Racy variants: don't wait for is_jit_compiled, call repeatedly
    # while background compile is likely running, catch/ignore any Python
    # exceptions (NameError from deleted globals, etc.), verify no crash ---

    def test_delete_type_during_background_compile_racy(self) -> None:
        """Racy variant: delete type immediately, then call function repeatedly
        while background compile is in flight, ignoring any Python exceptions.
        Verifies compiler doesn't crash accessing freed type objects.
        """
        namespace: dict[str, Any] = {}
        exec(
            """
class MyCustomType:
    value = 42

def target_func(obj):
    return obj.value + 1
""",
            namespace,
        )
        target_func = cast(FunctionType, namespace["target_func"])
        my_type = cast(Any, namespace["MyCustomType"])

        obj = my_type()
        target_func(obj)

        # Delete type and instance - if Preloader doesn't keep type alive,
        # background compile thread UAF -> crash
        del namespace["MyCustomType"]
        del my_type
        del obj
        gc.collect()

        # Call repeatedly while background compile likely running.
        # Use duck-typed dummy object so attribute access works even
        # if type is gone (interpreter path).
        class Dummy:
            value = 42

        dummy = Dummy()

        # Hammer the function while background compile is in flight.
        # Ignore any Python exceptions (e.g., if somehow attribute lookup fails)
        # - we're testing for crashes / UAF in the compiler thread, not
        # correctness of interpreter execution with deleted globals.
        start = time.time()
        calls = 0
        while time.time() - start < 0.5:
            target_func(dummy)
            calls += 1
            if calls > 100:
                break

        # If we got here without crashing, the background compiler survived
        # having its type objects deleted out from under it - i.e.,
        # Preloader OwnedType / ThreadedCompileGILHolder protection worked.
        self.assertGreaterEqual(calls, 0)  # Dummy assertion, test is no-crash

    def test_delete_callee_during_background_compile_racy(self) -> None:
        """Racy variant of callee deletion test: delete callee immediately,
        then call caller repeatedly, ignoring NameError from interpreter
        global lookup failures.  Verifies background compiler doesn't crash
        accessing freed callee function / code objects.
        """
        namespace: dict[str, Any] = {}
        exec(
            """
def callee_func(x):
    return x * 3 + 7

def caller_func(y):
    total = 0
    for i in range(10):
        total += callee_func(y + i)
    return total
""",
            namespace,
        )
        caller_func = cast(FunctionType, namespace["caller_func"])

        # Trigger background compile
        caller_func(5)

        # Delete callee immediately
        del namespace["callee_func"]
        gc.collect()

        # Call caller repeatedly while background compile is likely running.
        # Interpreter path will raise NameError (callee_func global deleted) -
        # ignore it.  JIT-compiled path (if compile finishes) should still work
        # because callee is pinned in CodeRuntime.
        start = time.time()
        successes = 0
        name_errors = 0
        while time.time() - start < 0.5:
            try:
                caller_func(5)
                successes += 1
            except NameError:
                name_errors += 1
            if successes + name_errors > 50:
                break

        # Test passes if we didn't crash - i.e., background compiler survived
        # callee deletion via Environment::addReference / CodeRuntime pinning
        self.assertTrue(True)

    def test_delete_global_during_background_compile_racy(self) -> None:
        """Racy variant: delete global immediately after triggering background
        compile, then call function repeatedly ignoring NameError.
        Verifies Preloader global_names_ / global_caches_ protection.
        """
        namespace: dict[str, Any] = {}
        exec(
            """
GLOBAL_VALUE = 12345

def target_func(x):
    return x + GLOBAL_VALUE
""",
            namespace,
        )
        target_func = cast(FunctionType, namespace["target_func"])
        target_func(1)

        del namespace["GLOBAL_VALUE"]
        gc.collect()

        start = time.time()
        while time.time() - start < 0.5:
            try:
                target_func(1)
            except NameError as ex:
                self.assertEqual(ex.name, "GLOBAL_VALUE")
            # Don't loop too long
            if time.time() - start > 0.1:
                break

        self.assertTrue(True)

    def test_delete_annotation_type_during_background_compile_racy(self) -> None:
        """Racy variant for annotation type deletion."""
        namespace: dict[str, Any] = {}
        exec(
            """
class ReturnType:
    pass

def annotated_func(x: int) -> ReturnType:
    return x * 2
""",
            namespace,
        )
        annotated_func = cast(FunctionType, namespace["annotated_func"])
        annotated_func(1)

        # Delete annotation type
        del namespace["ReturnType"]
        gc.collect()

        start = time.time()
        while time.time() - start < 0.3:
            annotated_func(1)

        self.assertTrue(True)

    def test_guard_type_target_pinning_racy(self) -> None:
        """Racy variant: delete GuardedType immediately, call target_func
        repeatedly with duck-typed dummy objects, ignoring NameError from
        isinstance global lookup in interpreter mode.
        Verifies CodeRuntime pinning prevents type confusion / UAF crash
        that would manifest as "'generator' object is not an async iterator"
        in builtin_anext.
        """
        namespace: dict[str, Any] = {}
        exec(
            """
class GuardedType:
    attr = 100

def target_func(obj):
    total = 0
    for i in range(5):
        if isinstance(obj, GuardedType):
            total += obj.attr + i
        else:
            total += i
    return total
""",
            namespace,
        )
        target_func = cast(FunctionType, namespace["target_func"])
        guarded_type = cast(Any, namespace["GuardedType"])
        obj = guarded_type()
        target_func(obj)

        # Delete type and instance - force type object to be freed unless
        # compiler/JIT pins it
        del namespace["GuardedType"]
        del guarded_type
        del obj
        gc.collect()

        class Dummy:
            attr = 100

        dummy = Dummy()

        # Hammer with dummy objects while background compile runs.
        # Interpreter path will NameError on isinstance(obj, GuardedType)
        # since global is deleted - ignore.
        # JIT path (if compiled) uses GuardType with pinned PyTypeObject*,
        # should work correctly (isinstance check becomes type guard,
        # dummy is not instance, else branch taken, no crash).
        start = time.time()
        while time.time() - start < 0.5:
            try:
                target_func(dummy)
            except NameError as exc:
                self.assertEqual(exc.name, "GuardedType")

        self.assertTrue(True)

    # --- Constant-folding / preload optimizations that previously punted out
    # of multi-threaded compilation via RETURN_MULTITHREADED_COMPILE (or a
    # canAccessSharedData JIT_CHECK) and now run on the background worker under
    # a ThreadedCompileGILHolder.  Each test forces the optimization to fire
    # during a background compile and asserts (a) no crash, (b) correct
    # results, and (c) via the HIR opcode histogram, that the fold actually
    # happened (the pre-fold opcode is gone from the final HIR). ---

    def _assert_folded_in_background(
        self,
        func: Any,
        absent_opcodes: tuple[str, ...],
        *args: Any,
    ) -> Any:
        """Trigger a background compile of ``func``, wait for it, and assert it
        JIT-compiled with every opcode in ``absent_opcodes`` folded away.
        Returns the interpreted result from the first (triggering) call."""
        # First call runs interpreted and enqueues the background compile.
        result = func(*args)
        _wait_for_jit_compile(func, timeout=5.0)
        self.assertTrue(cinderx.jit.is_jit_compiled(func))
        counts = cinderx.jit.get_function_hir_opcode_counts(func)
        self.assertIsNotNone(counts)
        for opcode in absent_opcodes:
            self.assertNotIn(opcode, cast(dict[str, int], counts))
        return result

    def test_int_constant_binary_op_background_compile(self) -> None:
        """Integer binary ops on compile-time-constant operands are constant-
        folded during background compilation.  simplifyLongBinaryOp no longer
        punts via RETURN_MULTITHREADED_COMPILE; it takes a
        ThreadedCompileGILHolder to allocate the resulting PyLong off the main
        thread, so the LongBinaryOp is gone from the compiled HIR.
        """

        def test_func() -> tuple[int, ...]:
            # CPython does not fold these across statements, but the JIT const-
            # propagates the locals into LongBinaryOp operands with object
            # specs, so simplifyLongBinaryOp folds each result at compile time.
            a = 7
            b = 13
            c = 1000
            return (a**b, c // a, c % a, a << b, c & a, c | a, c ^ a)

        result = self._assert_folded_in_background(test_func, ("LongBinaryOp",))
        self.assertEqual(test_func(), result)

    def test_float_constant_binary_op_background_compile(self) -> None:
        """Float binary ops on constant operands are constant-folded during
        background compilation.  The object-spec fold path in
        simplifyFloatBinaryOp (power / floor-divide / modulo) previously
        RETURN_MULTITHREADED_COMPILE'd; it now boxes the folded PyFloat under a
        ThreadedCompileGILHolder, so the FloatBinaryOp is gone.
        """

        def test_func() -> tuple[float, ...]:
            a = 3.5
            b = 4.0
            # Power (non-0.5 exponent), floor-divide, and modulo all reach the
            # object-spec constant-fold fallback rather than the unboxed
            # DoubleBinaryOp lowering used for +/-/*/(/).
            return (b**a, a // b, a % b)

        result = self._assert_folded_in_background(test_func, ("FloatBinaryOp",))
        self.assertEqual(test_func(), result)

    def test_unicode_subscript_constant_fold_background_compile(self) -> None:
        """Subscripting a constant string with a constant index folds during
        background compilation.  The unicode-subscript fold in simplifySubscript
        creates a new interned substring and now does so under a
        ThreadedCompileGILHolder, replacing the runtime UnicodeSubscr.
        """

        def test_func() -> tuple[str, ...]:
            s = "background-compile"
            i = 3
            j = -1
            return (s[i], s[j], s[0])

        result = self._assert_folded_in_background(test_func, ("UnicodeSubscr",))
        self.assertEqual(test_func(), result)

    @skip_if_ft(
        "simplifyLoadAttr instance fast path is disabled on free-threaded builds"
    )
    def test_load_attr_instance_background_compile(self) -> None:
        """Loading an attribute off an instance of a known exact type goes
        through simplifyLoadAttrInstanceReceiver -> ensureVersionTag.
        ensureVersionTag previously required the shared-data lock (a JIT_CHECK
        that would abort during a GIL-free background compile); it now assigns
        the type version tag under a ThreadedCompileGILHolder.  Constructing the
        instance in-function gives the receiver an exact type so the fast path
        (LoadField / split-dict) is taken instead of a generic LoadAttr.
        """

        def test_func() -> int:
            obj = AttrLoadType()
            return obj.inst_attr

        result = self._assert_folded_in_background(test_func, ("LoadAttr",))
        self.assertEqual(result, 7)
        self.assertEqual(test_func(), result)

    @skip_if_ft("loadModuleAttrSafe returns nullptr on free-threaded builds")
    def test_module_attr_load_background_compile(self) -> None:
        """Resolving a module attribute at compile time (loadModuleAttrSafe) now
        runs during background compilation instead of punting via
        RETURN_MULTITHREADED_COMPILE.  On 3.14 it reads the module dict with
        _PyDict_GetItemRefKeepLazy under a ThreadedCompileGILHolder (lazy import
        support).  Attributes that resolve to a type or submodule are exactly
        the values pinModuleAttr keeps alive with a GuardIs, so this exercises
        that off-GIL dict read + pin path (the runtime LoadModuleAttrCached is
        retained for correctness, so the fold is a pin, not a removal).
        """

        # `collections` / `os` are module-level globals so the JIT resolves the
        # module receiver to an object spec, which is what loadModuleAttrSafe
        # needs to read the attribute at compile time.
        def test_func() -> tuple[Any, ...]:
            # OrderedDict/defaultdict resolve to types; os.path is a submodule -
            # loadModuleAttrSafe resolves each and pinModuleAttr pins them.
            return (
                collections.OrderedDict,
                collections.defaultdict,
                os.path.sep,
            )

        result = self._assert_folded_in_background(test_func, ())
        self.assertEqual(test_func(), result)


# Workload for BackgroundCompilePoolForkTest, run out-of-process so the
# auto-JIT threshold and background_compile can be set from the environment.
#
# Shaped after the mezql fragment serializers (the binaries that wedged in
# S695342): a `jit = True` python_binary that farms work out to a
# multiprocessing.Pool created with the *fork* start method and
# maxtasksperchild=1, while the parent keeps running JIT-eligible Python.
_POOL_FORK_SCRIPT = '''
import faulthandler
import multiprocessing
import os
import signal
import sys
import time

START = time.time()

# The parent and every forked worker dump all thread stacks on SIGUSR1, so a
# timeout in the test can show where the processes are wedged.
faulthandler.register(signal.SIGUSR1, all_threads=True, chain=False)

import cinderx.jit

# Sized so the parent's worker is compiling essentially all of the time: a
# batch takes well under a second to queue up but much longer to compile, so
# the queue never empties and every fork lands mid-compile.  The function size
# and the fork count are handed down by the parent test because how long a
# compile takes is build-dependent; see SEGMENTS/TASKS there.
SEGMENTS = int(os.environ["POOL_FORK_SEGMENTS"])
TASKS = int(os.environ["POOL_FORK_TASKS"])
FUNCS_PER_BATCH = 2
SMALL_FUNCS = 100
CALLS_PER_FUNC = 4
WORKERS = 8


def progress(message):
    """Report progress with a raw write().

    Never print() while the pool is forking: buffered stdio takes a lock, and a
    worker forked while the main thread holds it inherits it locked and then
    wedges flushing its streams on the way out.  That hazard is Python\'s, not
    the JIT\'s, and it would mask the deadlock this test is looking for.
    """
    os.write(2, ("[%7.2fs] %s\\n" % (time.time() - START, message)).encode())


def source(name):
    lines = ["def " + name + "(a, b):", "    t = 0"]
    for i in range(SEGMENTS):
        lines.append("    if isinstance(a, int) and a > %d:" % i)
        lines.append("        t += a * %d + b" % i)
        lines.append("    else:")
        lines.append("        t -= b // %d" % (i + 1))
    lines.append("    return t")
    return "\\n".join(lines)


def churn(tag):
    """exec fresh functions and call them past the auto-JIT threshold.

    Every function is a new code object, so each one schedules its own
    background compile.  We deliberately never wait for them here: the point is
    to keep the worker thread compiling across the fork.
    """
    total = 0
    for i in range(FUNCS_PER_BATCH):
        name = "f_%s_%d" % (tag, i)
        namespace = {}
        exec(source(name), namespace)
        func = namespace[name]
        for _ in range(CALLS_PER_FUNC):
            total += func(i, 3)
    return total


def small_churn(n):
    """Create and drop JIT-tracked functions, which is what drives the
    funcDestroyed() bookkeeping that takes the JIT compilation lock."""
    total = 0
    for i in range(n):

        def inner(x=i):
            return x + 1

        total += inner()
    return total


def task(n):
    # Runs in a freshly forked pool worker.  Everything in here needs the JIT
    # state the child inherited through fork(), including whatever lock the
    # parent's background compile worker happened to be holding at the time.
    #
    # The wait at the end is what makes a wedged child *observable*: without
    # it the child would return before its own background compile got far
    # enough to touch the inherited locks, and a deadlocked compile thread
    # would go unnoticed because pool workers exit with os._exit().
    total = churn("child%d_%d" % (n, os.getpid()))
    total += small_churn(SMALL_FUNCS)
    cinderx.jit.wait_for_background_compiles()
    return total


def main():
    # Prime the parent so the background compile worker thread exists, and is
    # busy, before the first fork.
    churn("warmup")
    progress("bg compile %s" % cinderx.jit.get_background_compile())

    ctx = multiprocessing.get_context("fork")
    total = 0
    # maxtasksperchild=1 makes the pool fork a replacement worker after every
    # single task, so forks keep coming from the pool's non-main
    # _handle_workers thread while the main thread queues more compiles.
    pool = ctx.Pool(processes=WORKERS, maxtasksperchild=1)
    try:
        for i, result in enumerate(pool.imap_unordered(task, range(TASKS))):
            total += result
            if i % max(1, TASKS // 4) == 0:
                progress("task %d done" % i)
            churn("parent%d" % i)
        # close()/join() rather than terminate(): joining is where a child that
        # wedged after fork never lets the parent go, which is how this showed
        # up in the fragment serializers.
        pool.close()
        pool.join()
    except BaseException:
        pool.terminate()
        pool.join()
        raise

    print("ok", total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
'''


# macOS deprecated fork() in a threaded process, and this workload is nothing
# but that: the pool wedges in multiprocessing's own queue lock rather than in
# anything the JIT holds, so a failure there says nothing about the locks below.
@passUnless(sys.platform == "linux", "JIT fork safety is a Linux-only concern")
@passUnless(hasattr(os, "fork"), "requires fork()")
class BackgroundCompilePoolForkTest(unittest.TestCase):
    """Forking a multiprocessing.Pool while background compiles are in flight.

    Regression test for the CI hang in S695342, where `jit = True` build tools
    that fan work out over a fork-start-method Pool started wedging silently
    once background compile became the default.

    The hazard: a fork only carries over the forking thread, so any lock the
    background compile worker held at that instant stays locked forever in the
    child.  A child that blocks on one of them goes quiet with no output, and
    since pool workers are what produce results, the parent blocks in join()
    and the whole tree wedges with nothing on stdout.

    jitAtForkPrepare/Parent/Child in pyjit.cpp now cover every lock a compile
    thread can hold: the background compile registry mutex, jitCompilationMutex,
    the code allocator (including asmjit's own lock, behind runtime_mutex_),
    every live SlabArena::mutex_, ModuleState::mutex_, and
    HugePageArena::mutex_.

    HugePageArena is the worst of these because cinderjit.after_fork_child
    re-takes its mutex in the child before any user code runs.  It is only
    wired up on aarch64 (ALLOCATE_HUGE_PAGES in Common/slab_arena.cpp), which
    is also where the parent's hold window is widest -- allocateChunk() does a
    2MB-aligned malloc plus madvise() under the lock.

    The workload was never observed to deadlock on x86-64, where the windows on
    the remaining locks are only a few instructions wide.  It stays as a
    stress/regression guard: it forks TASKS times with the compile queue
    permanently backlogged, and reports a wedge as a failure with stacks rather
    than hanging the suite.

    Both tests run the same workload; the only difference is
    CINDERX_JIT_BACKGROUND_COMPILE.  The one with it off is the control: if it
    also times out, the workload (not background compile) is at fault.
    """

    # Healthy runs take well under a minute, but they are dominated by fork and
    # compile throughput and stretch badly on a loaded host.  A hang is
    # unbounded, so a wide margin costs nothing but keeps contention from
    # looking like a deadlock.
    TIMEOUT: float = 600.0

    # A debug + sanitizer build compiles roughly twenty times slower than an
    # optimized one, so the workload has to be sized for the build it runs in.
    # At the optimized sizes it grinds for ~8.5 minutes per test there with no
    # output at all -- indistinguishable from the hang this test exists to catch,
    # and close enough to TIMEOUT that a loaded host reports a deadlock that
    # isn't there.  The control run is the worse of the two: with background
    # compile off, the parent compiles inline, so its per-task churn is serial.
    #
    # gettotalrefcount only exists on a Py_DEBUG build.
    _SLOW_BUILD: bool = is_sanitizer_build() or hasattr(sys, "gettotalrefcount")

    # How many if/else segments each churned function gets.  Large enough that
    # compiling one takes far longer than queueing it -- that is what keeps the
    # compile queue permanently backlogged so every fork lands mid-compile -- and
    # under the JIT's size ceiling, above which functions are refused outright.
    # Shrinking it on a slow build costs the test nothing: a slower compiler
    # backs the queue up more easily, not less.
    SEGMENTS: int = 20 if _SLOW_BUILD else 120

    # How many pool tasks, and so how many forks, the workload drives.  This is
    # the one knob a slow build really gives up coverage on, so it is cut only
    # as far as the runtime budget demands.
    TASKS: int = 24 if _SLOW_BUILD else 80

    def _run_pool_workload(
        self, background_compile: str
    ) -> tuple[bool, int | None, str, str]:
        """Run the fork+pool workload out-of-process.

        Returns (timed_out, returncode, stdout, stderr).  On timeout the whole
        process group is asked for a traceback and then killed, so a hang is
        reported as a test failure instead of hanging the suite.
        """
        with tempfile.TemporaryDirectory() as tmp_dir:
            script = Path(tmp_dir) / "pool_fork.py"
            script.write_text(_POOL_FORK_SCRIPT)

            # start_new_session so the pool's workers land in their own process
            # group: killing just the direct child would leave them holding the
            # pipes open and communicate() would block anyway.
            proc = subprocess.Popen(
                [sys.executable, str(script)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding=ENCODING,
                env={
                    **subprocess_env(),
                    "CINDERX_JIT_AUTO": "2",
                    "CINDERX_JIT_BACKGROUND_COMPILE": background_compile,
                    "POOL_FORK_SEGMENTS": str(self.SEGMENTS),
                    "POOL_FORK_TASKS": str(self.TASKS),
                },
                start_new_session=True,
            )
            try:
                stdout, stderr = proc.communicate(timeout=self.TIMEOUT)
                return False, proc.returncode, stdout, stderr
            except subprocess.TimeoutExpired:
                self._dump_and_kill(proc)
                stdout, stderr = proc.communicate()
                return True, proc.returncode, stdout, stderr

    def _dump_and_kill(self, proc: "subprocess.Popen[str]") -> None:
        """Ask every process in the group for a traceback, then kill them."""
        try:
            os.killpg(proc.pid, signal.SIGUSR1)
            time.sleep(2)
        except OSError:
            pass
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except OSError:
            proc.kill()

    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_pool_fork_during_background_compile(self) -> None:
        timed_out, returncode, stdout, stderr = self._run_pool_workload("1")
        self.assertFalse(
            timed_out,
            f"multiprocessing.Pool(fork) deadlocked with background compile "
            f"enabled\nstdout:\n{stdout}\nstderr:\n{stderr}",
        )
        self.assertEqual(returncode, 0, stderr)
        self.assertIn("ok ", stdout, stderr)

    @skip_unless_jit("Runs a subprocess with the JIT enabled")
    def test_pool_fork_without_background_compile(self) -> None:
        """Control: the same workload must be fine without background compile."""
        timed_out, returncode, stdout, stderr = self._run_pool_workload("0")
        self.assertFalse(
            timed_out,
            f"multiprocessing.Pool(fork) deadlocked with background compile "
            f"disabled, so the workload itself is broken\nstdout:\n{stdout}\n"
            f"stderr:\n{stderr}",
        )
        self.assertEqual(returncode, 0, stderr)
        self.assertIn("ok ", stdout, stderr)


if __name__ == "__main__":
    unittest.main()
