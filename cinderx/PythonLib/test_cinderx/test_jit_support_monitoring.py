# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import sys
import unittest

from cinderx.jit import force_compile, is_jit_compiled
from cinderx.test_support import passUnless, skip_unless_jit


# sys.monitoring is only available in Python 3.12+
AT_LEAST_312 = sys.version_info[:2] >= (3, 12)


def dummy_callback(code, offset, callable, arg):
    return None


@passUnless(AT_LEAST_312, "sys.monitoring requires Python 3.12+")
@skip_unless_jit("Tests functionality on the JIT")
class JitMonitoringIntegrationTest(unittest.TestCase):
    """
    Test that the JIT behaves correctly in debugger/profiler workflows. These
    workflows all go through `sys.monitoring.register_callback`, so this is used
    to test the JIT's behavior.

    When a debugger/profiler attaches via `sys.monitoring.register_callback`,
    the JIT should:
    1. Stop compiling new functions
    2. Deoptimize previously compiled functions
    3. Re-optimize when all callbacks are removed
    4. Not interfere with callback invocation
    """

    def test_new_functions_not_compiled_while_callback_registered(self) -> None:
        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "test_debugger")
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )

        def new_function(a, b):
            return a + b

        force_compile(new_function)

        self.assertFalse(
            is_jit_compiled(new_function),
            "New functions should not be JIT compiled while callback is registered",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_compiled_functions_deoptimized_on_callback_registration(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "test_debugger")
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )

        self.assertFalse(
            is_jit_compiled(foo),
            "Compiled function should be deoptimized when callback is registered",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_functions_reoptimized_after_callback_removed(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "test_debugger")
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )
        self.assertFalse(is_jit_compiled(foo))

        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            None,
        )

        self.assertTrue(
            is_jit_compiled(foo),
            "Function should be re-JIT compiled after callback is removed",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_all_callbacks_must_be_removed_for_reoptimization(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "test_debugger")
        sys.monitoring.use_tool_id(sys.monitoring.PROFILER_ID, "test_profiler")

        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )
        sys.monitoring.register_callback(
            sys.monitoring.PROFILER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )
        self.assertFalse(is_jit_compiled(foo))

        # Remove first callback - should still be deoptimized
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            None,
        )
        self.assertFalse(
            is_jit_compiled(foo),
            "Function should remain deoptimized while any callback is active",
        )

        # Remove second callback - now should reoptimize
        sys.monitoring.register_callback(
            sys.monitoring.PROFILER_ID,
            sys.monitoring.events.CALL,
            None,
        )
        self.assertTrue(
            is_jit_compiled(foo),
            "Function should be re-JIT compiled after all callbacks removed",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)
        sys.monitoring.free_tool_id(sys.monitoring.PROFILER_ID)

    def test_reregistering_same_callback_only_needs_one_removal(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "test_debugger")

        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )
        self.assertFalse(is_jit_compiled(foo))

        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            None,
        )
        self.assertTrue(
            is_jit_compiled(foo),
            "Clearing tool id's callback should re-enable JIT regardless of how many times it was registered",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_removing_callback_twice_is_harmless(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "test_debugger")
        sys.monitoring.use_tool_id(sys.monitoring.PROFILER_ID, "test_profiler")

        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )
        sys.monitoring.register_callback(
            sys.monitoring.PROFILER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )
        self.assertFalse(is_jit_compiled(foo))

        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            None,
        )
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            None,
        )
        self.assertFalse(
            is_jit_compiled(foo),
            "JIT should still be disabled since the profiler callback is still registered",
        )

        sys.monitoring.register_callback(
            sys.monitoring.PROFILER_ID,
            sys.monitoring.events.CALL,
            None,
        )
        self.assertTrue(
            is_jit_compiled(foo),
            "Should re-enable JIT after all callbacks removed",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)
        sys.monitoring.free_tool_id(sys.monitoring.PROFILER_ID)

    def test_callbacks_invoked_for_deoptimized_functions(self) -> None:
        calls_seen = []

        def call_callback(code, offset, callable, arg):
            # Guard against callables without __name__; the callback may still
            # be invoked after free_tool_id during interpreter cleanup.
            if hasattr(callable, "__name__"):
                calls_seen.append(callable.__name__)
            return None

        def helper():
            return 42

        def traced_function():
            return helper()

        force_compile(traced_function)
        self.assertTrue(is_jit_compiled(traced_function))

        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "test_debugger")
        sys.monitoring.set_events(
            sys.monitoring.DEBUGGER_ID, sys.monitoring.events.CALL
        )
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            call_callback,
        )
        self.assertFalse(is_jit_compiled(traced_function))

        result = traced_function()

        self.assertEqual(result, 42)
        self.assertIn(
            "helper",
            calls_seen,
            "CALL callback should have seen the helper function call",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_function_results_correct_across_jit_state_transitions(self) -> None:
        def compute(n):
            result = 0
            for i in range(n):
                result += i * i
            return result

        expected = sum(i * i for i in range(100))

        # Test while JIT compiled
        force_compile(compute)
        self.assertTrue(is_jit_compiled(compute))
        self.assertEqual(compute(100), expected)

        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "test_debugger")
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )

        # Test while deoptimized
        self.assertFalse(is_jit_compiled(compute))
        self.assertEqual(compute(100), expected)

        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            None,
        )

        # Test after re-optimization
        self.assertTrue(is_jit_compiled(compute))
        self.assertEqual(compute(100), expected)

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)
