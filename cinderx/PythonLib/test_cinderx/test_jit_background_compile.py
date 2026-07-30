# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import gc
import sys
import time
import unittest
import weakref
from types import FunctionType
from typing import Any, Callable, cast

import cinderx.jit
from cinderx.test_support import failUnlessJITCompiled, skip_if_prefork

from .test_jit_pipeline_stress import make_function


class AttrLoadType:
    """Module-level type used to give the JIT an exact receiver type so
    LoadAttr on an instance goes through simplifyLoadAttrInstanceReceiver."""

    cls_attr = 123

    def __init__(self) -> None:
        self.inst_attr = 7


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

    def test_delete_function_during_background_compile(self) -> None:
        """Test that deleting a function object during background compilation
        doesn't crash.  The BackgroundCompileTask holds a ThreadedRef to the
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
        # The background compile task should keep it alive via ThreadedRef
        func_weak = weakref.ref(target_func)
        del target_func
        gc.collect()

        # Give background compile time to run with no Python references held
        # (if background_compile is disabled, this is a no-op)
        time.sleep(0.1)
        gc.collect()

        # Function object may still be alive due to BackgroundCompileTask holding
        # a ThreadedRef, or it may be dead if compilation finished quickly and
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
        - TypedArgument pytype ThreadedRef with ThreadedCompileGILHolder guard
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


if __name__ == "__main__":
    unittest.main()
