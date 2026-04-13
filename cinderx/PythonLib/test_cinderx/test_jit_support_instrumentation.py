# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import sys
import threading
import unittest
from types import CodeType, FrameType
from typing import Generator

from cinderx.jit import force_compile, is_jit_compiled
from cinderx.test_support import passUnless, skip_unless_jit



# sys.monitoring is only available in Python 3.12+
AT_LEAST_312: bool = sys.version_info[:2] >= (3, 12)


def dummy_callback(
    code: CodeType, offset: int, callable: object, arg: object
) -> object:
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

        def new_function(a: int, b: int) -> int:
            return a + b

        force_compile(new_function)

        self.assertFalse(
            is_jit_compiled(new_function),
            "New functions should not be JIT compiled while callback is registered",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_compiled_functions_deoptimized_on_callback_registration(self) -> None:
        def foo(a: int, b: int) -> int:
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
        def foo(a: int, b: int) -> int:
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

    def test_functions_reoptimized_after_free_tool_id(self) -> None:
        def foo(a: int, b: int) -> int:
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

        # Free the tool directly instead of clearing the callback
        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

        self.assertTrue(
            is_jit_compiled(foo),
            "Function should be re-JIT compiled after free_tool_id is called",
        )

    def test_free_tool_id_with_multiple_tools(self) -> None:
        def foo(a: int, b: int) -> int:
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

        # Free first tool - should still be deoptimized
        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)
        self.assertFalse(
            is_jit_compiled(foo),
            "JIT should remain disabled while profiler callback is still active",
        )

        # Free second tool - now should reoptimize
        sys.monitoring.free_tool_id(sys.monitoring.PROFILER_ID)
        self.assertTrue(
            is_jit_compiled(foo),
            "JIT should re-enable after all tools are freed via free_tool_id",
        )

    def test_all_callbacks_must_be_removed_for_reoptimization(self) -> None:
        def foo(a: int, b: int) -> int:
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
        def foo(a: int, b: int) -> int:
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
        def foo(a: int, b: int) -> int:
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
        calls_seen: list[str] = []

        def call_callback(
            code: CodeType, offset: int, callable: object, arg: object
        ) -> object:
            # Guard against callables without __name__; the callback may still be
            # invoked after free_tool_id during interpreter cleanup.
            if name := getattr(callable, "__name__", None):
                calls_seen.append(name)

        def helper() -> int:
            return 42

        def traced_function() -> int:
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
        def compute(n: int) -> int:
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


@skip_unless_jit("Tests functionality on the JIT")
class JitSetProfileIntegrationTest(unittest.TestCase):
    """
    Test that the JIT behaves correctly when sys.setprofile is used.

    When a profiler attaches via sys.setprofile, the JIT should:
    1. Stop compiling new functions
    2. Deoptimize previously compiled functions
    3. Re-optimize when the profiler is removed
    """

    def tearDown(self) -> None:
        sys.setprofile(None)

    def test_compiled_functions_deoptimized_on_setprofile(self) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        calls_seen: list[str] = []

        def profile_func(frame: FrameType, event: str, arg: object) -> None:
            if event == "call" and frame.f_code.co_name == "foo":
                calls_seen.append(frame.f_code.co_name)

        sys.setprofile(profile_func)

        self.assertFalse(
            is_jit_compiled(foo),
            "Compiled function should be deoptimized when profiler is set",
        )

        foo(1, 2)
        self.assertIn("foo", calls_seen, "Profile callback should have been invoked")

    def test_functions_reoptimized_after_setprofile_cleared(self) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        calls_seen: list[str] = []

        def profile_func(frame: FrameType, event: str, arg: object) -> None:
            if event == "call" and frame.f_code.co_name == "foo":
                calls_seen.append(frame.f_code.co_name)

        sys.setprofile(profile_func)
        self.assertFalse(is_jit_compiled(foo))

        foo(1, 2)
        self.assertIn("foo", calls_seen, "Profile callback should have been invoked")

        sys.setprofile(None)

        self.assertTrue(
            is_jit_compiled(foo),
            "Function should be re-JIT compiled after profiler is cleared",
        )

        calls_seen.clear()
        foo(1, 2)
        self.assertEqual(
            calls_seen, [], "Profile callback should not be invoked after clearing"
        )

    def test_new_functions_not_compiled_while_profiler_active(self) -> None:
        calls_seen: list[str] = []

        def profile_func(frame: FrameType, event: str, arg: object) -> None:
            if event == "call" and frame.f_code.co_name == "new_function":
                calls_seen.append(frame.f_code.co_name)

        sys.setprofile(profile_func)

        def new_function(a: int, b: int) -> int:
            return a + b

        force_compile(new_function)

        self.assertFalse(
            is_jit_compiled(new_function),
            "New functions should not be JIT compiled while profiler is active",
        )

        new_function(1, 2)
        self.assertIn(
            "new_function", calls_seen, "Profile callback should have been invoked"
        )


@skip_unless_jit("Tests functionality on the JIT")
class JitSetTraceIntegrationTest(unittest.TestCase):
    """
    Test that the JIT behaves correctly when sys.settrace is used.

    When a debugger attaches via sys.settrace, the JIT should:
    1. Stop compiling new functions
    2. Deoptimize previously compiled functions
    3. Re-optimize when the debugger is removed
    """

    def tearDown(self) -> None:
        sys.settrace(None)

    def test_looping_thread_deopted_on_instrumentation(self) -> None:
        # A worker thread in a tight JIT loop should have its topmost frame
        # deopted when instrumentation activates from another thread.
        loop_line_events: list[str] = []
        thread_in_loop: threading.Event = threading.Event()
        stop_flag: threading.Event = threading.Event()

        def line_callback(code: CodeType, line: int) -> object:
            if code.co_name == "worker_loop":
                loop_line_events.append(code.co_name)
            return sys.monitoring.DISABLE

        # Use a top-level-style function that avoids closure variables in the
        # hot loop.
        def worker_loop(flag: threading.Event, started: threading.Event) -> int:
            total = 0
            while not flag.is_set():
                total += 1
                if total == 1:
                    started.set()
            x = 999  # LINE event expected here after deopt
            return total + x

        force_compile(worker_loop)
        self.assertTrue(is_jit_compiled(worker_loop))

        thread_done: threading.Event = threading.Event()
        results: list[int] = []

        def thread_target() -> None:
            result = worker_loop(stop_flag, thread_in_loop)
            results.append(result)
            thread_done.set()

        worker_thread = threading.Thread(target=thread_target)
        worker_thread.start()

        try:
            # Wait for worker to be inside the JIT-compiled loop
            self.assertTrue(
                thread_in_loop.wait(timeout=5.0),
                "Worker thread didn't enter JIT loop in time",
            )

            # Attach instrumentation from main thread while worker is looping.
            # The worker's topmost frame (worker_loop) should be patched by
            # deoptAllJitFramesOnStack and transition to interpreter mode.
            sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "loop_debugger")
            sys.monitoring.set_events(
                sys.monitoring.DEBUGGER_ID, sys.monitoring.events.LINE
            )
            sys.monitoring.register_callback(
                sys.monitoring.DEBUGGER_ID,
                sys.monitoring.events.LINE,
                line_callback,
            )

            # Signal loop to stop
            stop_flag.set()

            self.assertTrue(
                thread_done.wait(timeout=10.0),
                "Worker thread didn't complete in time",
            )

            self.assertEqual(len(results), 1)

            # worker_loop should receive LINE events after its topmost frame
            # was deopted via return-address patching at the eval breaker.
            self.assertIn(
                "worker_loop",
                loop_line_events,
                "worker_loop should receive LINE events after its topmost frame "
                "is deopted when instrumentation activates while it is looping",
            )
        finally:
            stop_flag.set()
            worker_thread.join(timeout=5.0)
            if worker_thread.is_alive():
                self.fail("Worker thread didn't terminate")
            try:
                sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)
            except ValueError:
                pass

    def test_compiled_functions_deoptimized_on_settrace(self) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        calls_seen: list[str] = []

        # pyre-ignore[3]: Return type is recursive.
        def trace_func(frame: FrameType, event: str, arg: object):
            if event == "call" and frame.f_code.co_name == "foo":
                calls_seen.append(frame.f_code.co_name)
            return trace_func

        sys.settrace(trace_func)

        self.assertFalse(
            is_jit_compiled(foo),
            "Compiled function should be deoptimized when tracer is set",
        )

        foo(1, 2)
        self.assertIn("foo", calls_seen, "Trace callback should have been invoked")

    def test_functions_reoptimized_after_settrace_cleared(self) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        calls_seen: list[str] = []

        # pyre-ignore[3]: Return type is recursive.
        def trace_func(frame: FrameType, event: str, arg: object):
            if event == "call" and frame.f_code.co_name == "foo":
                calls_seen.append(frame.f_code.co_name)
            return trace_func

        sys.settrace(trace_func)
        self.assertFalse(is_jit_compiled(foo))

        foo(1, 2)
        self.assertIn("foo", calls_seen, "Trace callback should have been invoked")

        sys.settrace(None)

        self.assertTrue(
            is_jit_compiled(foo),
            "Function should be re-JIT compiled after tracer is cleared",
        )

        calls_seen.clear()
        foo(1, 2)
        self.assertEqual(
            calls_seen, [], "Trace callback should not be invoked after clearing"
        )

    def test_new_functions_not_compiled_while_tracer_active(self) -> None:
        calls_seen: list[str] = []

        # pyre-ignore[3]: Return type is recursive.
        def trace_func(frame: FrameType, event: str, arg: object):
            if event == "call" and frame.f_code.co_name == "new_function":
                calls_seen.append(frame.f_code.co_name)
            return trace_func

        sys.settrace(trace_func)

        def new_function(a: int, b: int) -> int:
            return a + b

        force_compile(new_function)

        self.assertFalse(
            is_jit_compiled(new_function),
            "New functions should not be JIT compiled while tracer is active",
        )

        new_function(1, 2)
        self.assertIn(
            "new_function", calls_seen, "Trace callback should have been invoked"
        )


@passUnless(
    AT_LEAST_312, "Combined instrumentation requires Python 3.12+ for JIT integration"
)
@skip_unless_jit("Tests functionality on the JIT")
class JitCombinedTracingIntegrationTest(unittest.TestCase):
    """
    Test that the JIT behaves correctly when multiple instrumentation mechanisms are used.

    The JIT should only re-enable when ALL instrumentation mechanisms are cleared.
    """

    def test_profiling_and_tracing_callbacks_must_be_cleared_for_reoptimization(
        self,
    ) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        def profile_func(frame: FrameType, event: str, arg: object) -> None:
            return None

        # pyre-ignore[3]: Return type is recursive.
        def trace_func(frame: FrameType, event: str, arg: object):
            return trace_func

        sys.setprofile(profile_func)
        sys.settrace(trace_func)

        self.assertFalse(is_jit_compiled(foo))

        sys.setprofile(None)
        self.assertFalse(
            is_jit_compiled(foo),
            "JIT should remain disabled since trace callback is still active",
        )

        sys.settrace(None)
        self.assertTrue(
            is_jit_compiled(foo),
            "JIT should re-enable after all callbacks are cleared",
        )

    def test_monitoring_and_setprofile_combined(self) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "test_debugger")
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            dummy_callback,
        )

        def profile_func(frame: FrameType, event: str, arg: object) -> None:
            return None

        sys.setprofile(profile_func)

        self.assertFalse(is_jit_compiled(foo))

        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.CALL,
            None,
        )
        self.assertFalse(
            is_jit_compiled(foo),
            "JIT should remain disabled while sys.setprofile callback is active",
        )

        sys.setprofile(None)
        self.assertTrue(
            is_jit_compiled(foo),
            "JIT should re-enable after all callbacks are cleared",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)


@passUnless(AT_LEAST_312, "sys.monitoring requires Python 3.12+")
@skip_unless_jit("Tests functionality on the JIT")
class JitStackFrameDeoptTest(unittest.TestCase):
    """
    Test that JIT frames on the stack are deopted when instrumentation attaches.

    When instrumentation activates mid-execution, the JIT deopts frames currently
    on the stack so they transition to interpreter mode and receive monitoring
    events (LINE, PY_RETURN, etc.). This covers on-stack frames reached via
    return-address patching (deoptAllJitFramesOnStack), suspended generators
    deopted via function-object deopt, and cross-thread frame patching.
    """

    def test_grandparent_frame_receives_line_events_after_parent_returns(self) -> None:
        # The grandparent frame should receive LINE events after the parent
        # (which called register_callback) returns through deopt trampoline
        line_events: list[tuple[str, int]] = []

        def line_callback(code: CodeType, line: int) -> object:
            line_events.append((code.co_name, line))
            return sys.monitoring.DISABLE

        def register_monitoring() -> int:
            sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "line_debugger")
            sys.monitoring.set_events(
                sys.monitoring.DEBUGGER_ID, sys.monitoring.events.LINE
            )
            sys.monitoring.register_callback(
                sys.monitoring.DEBUGGER_ID,
                sys.monitoring.events.LINE,
                line_callback,
            )
            return 1

        def parent() -> int:
            # This frame's return address is patched; when register_monitoring
            # returns, parent transitions to interpreter mode
            a = register_monitoring()
            b = 10  # LINE event should fire here (interpreter mode)
            return a + b

        def grandparent() -> int:
            # This frame's return address is also patched; when parent returns,
            # grandparent transitions to interpreter mode
            x = 100
            y = parent()
            z = 200  # LINE event should fire here (interpreter mode)
            w = 300  # LINE event should fire here too
            return x + y + z + w

        force_compile(register_monitoring)
        force_compile(parent)
        force_compile(grandparent)
        self.assertTrue(is_jit_compiled(grandparent))
        self.assertTrue(is_jit_compiled(parent))

        result = grandparent()

        self.assertEqual(result, 611)

        # Both parent and grandparent should receive LINE events after deopt.
        # parent is the direct caller of register_monitoring — its return
        # address is patched, so it deopts when register_monitoring returns.
        # grandparent is patched similarly for the call to parent.
        parent_lines = [name for name, _ in line_events if name == "parent"]
        self.assertGreater(
            len(parent_lines),
            0,
            "LINE events should fire for parent after register_monitoring returns "
            "through deopt trampoline, proving parent transitioned to interpreter",
        )

        grandparent_lines = [name for name, _ in line_events if name == "grandparent"]
        self.assertGreater(
            len(grandparent_lines),
            0,
            "LINE events should fire for grandparent after parent returns through "
            "deopt trampoline, proving grandparent transitioned to interpreter",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_deeply_nested_stack_grandparent_receives_events(self) -> None:
        # With deeply nested calls, grandparent frames should receive LINE events
        line_events: list[str] = []

        def line_callback(code: CodeType, line: int) -> object:
            if code.co_name in ("depth_1", "depth_2", "depth_3"):
                line_events.append(code.co_name)
            return sys.monitoring.DISABLE

        def register_monitoring() -> int:
            sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "deep_debugger")
            sys.monitoring.set_events(
                sys.monitoring.DEBUGGER_ID, sys.monitoring.events.LINE
            )
            sys.monitoring.register_callback(
                sys.monitoring.DEBUGGER_ID,
                sys.monitoring.events.LINE,
                line_callback,
            )
            return 1

        def depth_3() -> int:
            # Suspended at call to register_monitoring — deopts when it returns
            x = register_monitoring()
            y = 3  # LINE event fires here (interpreter mode after deopt)
            return x + y

        def depth_2() -> int:
            # Grandparent of register_monitoring - should get LINE events
            a = depth_3()
            b = 20  # LINE event expected here
            return a + b

        def depth_1() -> int:
            # Great-grandparent - should also get LINE events
            c = depth_2()
            d = 100  # LINE event expected here
            return c + d

        # register_monitoring is deliberately NOT force_compiled — it runs as
        # an interpreter frame. This doesn't affect the deopt behavior: depth_3's
        # callsite_deopt_exits_ entry for the call to register_monitoring is
        # determined by depth_3's codegen, not the callee's compile status.
        force_compile(depth_1)
        force_compile(depth_2)
        force_compile(depth_3)

        result = depth_1()

        self.assertEqual(result, 124)

        # All frames in the chain should receive LINE events after deopt.
        self.assertIn(
            "depth_3",
            line_events,
            "depth_3 should receive LINE events (direct caller, deopted)",
        )
        self.assertIn(
            "depth_2",
            line_events,
            "depth_2 should receive LINE events (grandparent, deopted "
            "via multi-frame chaining)",
        )
        self.assertIn(
            "depth_1",
            line_events,
            "depth_1 should receive LINE events (great-grandparent, deopted "
            "via multi-frame chaining)",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_return_event_fires_for_parent_frame(self) -> None:
        # PY_RETURN event should fire when the parent frame returns through deopt
        return_events: list[str] = []

        def py_return_callback(code: CodeType, offset: int, retval: object) -> object:
            return_events.append(code.co_name)
            return None

        def register_return_monitoring() -> int:
            sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "return_debugger")
            sys.monitoring.set_events(
                sys.monitoring.DEBUGGER_ID, sys.monitoring.events.PY_RETURN
            )
            sys.monitoring.register_callback(
                sys.monitoring.DEBUGGER_ID,
                sys.monitoring.events.PY_RETURN,
                py_return_callback,
            )
            return 1

        def parent() -> int:
            x = register_return_monitoring()
            return x + 42

        def grandparent() -> int:
            return parent() + 100

        force_compile(parent)
        force_compile(grandparent)

        result = grandparent()

        self.assertEqual(result, 143)

        # Both parent and grandparent should trigger PY_RETURN when they
        # return through the deopt trampoline via multi-frame chaining.
        self.assertIn(
            "parent",
            return_events,
            "PY_RETURN should fire for parent when it returns through deopt",
        )
        self.assertIn(
            "grandparent",
            return_events,
            "PY_RETURN should fire for grandparent when it returns",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_suspended_generator_deopted_on_instrumentation_attach(self) -> None:
        # Suspended generators should be deopted when instrumentation attaches
        line_events: list[str] = []

        def line_callback(code: CodeType, line: int) -> object:
            if code.co_name == "my_generator":
                line_events.append(code.co_name)
            return sys.monitoring.DISABLE

        def my_generator() -> Generator[int, None, None]:
            yield 1
            yield 2  # LINE event should fire here after deopt
            yield 3

        force_compile(my_generator)
        self.assertTrue(is_jit_compiled(my_generator))

        gen = my_generator()
        first_value = next(gen)
        self.assertEqual(first_value, 1)

        # Generator is now suspended - attach instrumentation
        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "gen_debugger")
        sys.monitoring.set_events(
            sys.monitoring.DEBUGGER_ID, sys.monitoring.events.LINE
        )
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.LINE,
            line_callback,
        )

        self.assertFalse(
            is_jit_compiled(my_generator),
            "Generator function should be deoptimized after instrumentation attached",
        )

        # Resume generator - LINE events should fire proving generator was deopted
        second_value = next(gen)
        third_value = next(gen)
        self.assertEqual(second_value, 2)
        self.assertEqual(third_value, 3)

        self.assertIn(
            "my_generator",
            line_events,
            "LINE events should fire for resumed generator, proving it was deopted",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_multiple_suspended_generators_all_deopted(self) -> None:
        # Multiple suspended generators should all be deopted
        line_events: list[str] = []

        def line_callback(code: CodeType, line: int) -> object:
            line_events.append(code.co_name)
            return sys.monitoring.DISABLE

        def gen_a() -> Generator[str, None, None]:
            yield "a1"
            yield "a2"  # LINE event proves gen_a deopted

        def gen_b() -> Generator[str, None, None]:
            yield "b1"
            yield "b2"  # LINE event proves gen_b deopted

        force_compile(gen_a)
        force_compile(gen_b)
        self.assertTrue(is_jit_compiled(gen_a))
        self.assertTrue(is_jit_compiled(gen_b))

        # Create and partially consume generators
        iter_a = gen_a()
        iter_b = gen_b()
        self.assertEqual(next(iter_a), "a1")
        self.assertEqual(next(iter_b), "b1")

        # Attach instrumentation while both generators are suspended
        sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "multi_gen_debugger")
        sys.monitoring.set_events(
            sys.monitoring.DEBUGGER_ID, sys.monitoring.events.LINE
        )
        sys.monitoring.register_callback(
            sys.monitoring.DEBUGGER_ID,
            sys.monitoring.events.LINE,
            line_callback,
        )

        self.assertFalse(is_jit_compiled(gen_a))
        self.assertFalse(is_jit_compiled(gen_b))

        # Resume both generators
        self.assertEqual(next(iter_a), "a2")
        self.assertEqual(next(iter_b), "b2")

        self.assertIn("gen_a", line_events, "gen_a should receive LINE events")
        self.assertIn("gen_b", line_events, "gen_b should receive LINE events")

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_compound_expression_frame_deopted(self) -> None:
        """
        Test that frames with compound expressions (non-empty operand stack)
        are correctly deopted when instrumentation activates.

        In foo() + bar(), foo()'s result is on the operand stack when bar()
        is called. The post-call guard's DeoptMetadata does not include bar()'s
        return value as a stack entry (it captures the error-case state).
        prepareForDeopt pushes RAX (bar()'s return value) onto the operand
        stack after reifyStack reconstructs the pre-call values (foo()'s result)
        from stable locations (callee-saved regs / spill slots).
        """
        line_events: list[tuple[str, int]] = []

        def line_callback(code: CodeType, line: int) -> object:
            line_events.append((code.co_name, line))
            return sys.monitoring.DISABLE

        def register_monitoring() -> int:
            sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "compound_debugger")
            sys.monitoring.set_events(
                sys.monitoring.DEBUGGER_ID, sys.monitoring.events.LINE
            )
            sys.monitoring.register_callback(
                sys.monitoring.DEBUGGER_ID,
                sys.monitoring.events.LINE,
                line_callback,
            )
            return 1

        def helper_a() -> int:
            return 100

        def helper_b() -> int:
            a = register_monitoring()
            return a + 50

        def parent_compound() -> int:
            # Compound expression: helper_a()'s result is on the operand stack
            # when helper_b() is called. This frame's return address is patched
            # by deoptAllJitFramesOnStack, so it transitions to interpreter mode
            # when helper_b() returns.
            result = helper_a() + helper_b()
            x = 999  # LINE event should fire here (interpreter mode)
            return result + x

        force_compile(register_monitoring)
        force_compile(helper_a)
        force_compile(helper_b)
        force_compile(parent_compound)

        self.assertTrue(is_jit_compiled(parent_compound))

        result = parent_compound()

        self.assertEqual(result, 1150)

        parent_lines = [name for name, _ in line_events if name == "parent_compound"]
        self.assertGreater(
            len(parent_lines),
            0,
            "parent_compound should receive LINE events after helper_b() "
            "returns through deopt trampoline, proving the compound expression "
            "frame was correctly deopted.",
        )

        sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)

    def test_multithread_grandparent_frame_deopted(self) -> None:
        # Worker thread's grandparent frame should receive LINE events
        worker_line_events: list[str] = []
        thread_started: threading.Event = threading.Event()
        thread_continue: threading.Event = threading.Event()
        thread_done: threading.Event = threading.Event()

        def line_callback(code: CodeType, line: int) -> object:
            if code.co_name == "worker_grandparent":
                worker_line_events.append(code.co_name)
            return sys.monitoring.DISABLE

        def worker_parent() -> int:
            # This frame is suspended at wait() when instrumentation attaches.
            # Its return address is patched, so it deopts when wait() returns.
            thread_started.set()
            thread_continue.wait(timeout=5.0)
            return 10

        def worker_grandparent() -> int:
            # This frame's return address is also patched; when worker_parent
            # returns through the deopt trampoline, worker_grandparent
            # transitions to interpreter mode.
            x = worker_parent()
            y = 20  # LINE event expected here (interpreter mode)
            z = 30  # LINE event expected here (interpreter mode)
            return x + y + z

        force_compile(worker_grandparent)
        force_compile(worker_parent)
        self.assertTrue(is_jit_compiled(worker_grandparent))
        self.assertTrue(is_jit_compiled(worker_parent))

        results: list[int] = []

        def thread_target() -> None:
            result = worker_grandparent()
            results.append(result)
            thread_done.set()

        worker_thread = threading.Thread(target=thread_target)
        worker_thread.start()

        try:
            # Wait for worker to start and pause inside worker_parent
            self.assertTrue(
                thread_started.wait(timeout=5.0), "Worker thread didn't start in time"
            )

            # Attach instrumentation from main thread while worker is paused.
            # Both worker_parent and worker_grandparent have their return
            # addresses patched and will deopt when their callees return.
            sys.monitoring.use_tool_id(sys.monitoring.DEBUGGER_ID, "thread_debugger")
            sys.monitoring.set_events(
                sys.monitoring.DEBUGGER_ID, sys.monitoring.events.LINE
            )
            sys.monitoring.register_callback(
                sys.monitoring.DEBUGGER_ID,
                sys.monitoring.events.LINE,
                line_callback,
            )

            # Signal worker to continue
            thread_continue.set()

            self.assertTrue(
                thread_done.wait(timeout=10.0), "Worker thread didn't complete in time"
            )

            self.assertEqual(len(results), 1)
            self.assertEqual(results[0], 60)

            # Verify worker_grandparent received LINE events after
            # worker_parent returned through deopt trampoline via
            # multi-frame deopt chaining.
            self.assertGreater(
                len(worker_line_events),
                0,
                "worker_grandparent should receive LINE events after worker_parent "
                "returns through deopt trampoline",
            )
        finally:
            # Ensure thread isn't stuck waiting
            thread_continue.set()
            worker_thread.join(timeout=5.0)
            if worker_thread.is_alive():
                self.fail("Worker thread didn't terminate")
            try:
                sys.monitoring.free_tool_id(sys.monitoring.DEBUGGER_ID)
            except ValueError:
                pass

    def test_settrace_grandparent_receives_line_events(self) -> None:
        # sys.settrace should also cause grandparent frames to receive LINE events
        line_events: list[tuple[str, str]] = []

        # pyre-ignore[3]: Return type is recursive.
        def trace_func(frame: FrameType, event: str, arg: object):
            if frame.f_code.co_name == "grandparent" and event == "line":
                line_events.append((frame.f_code.co_name, event))
            return trace_func

        def activate_tracing() -> int:
            sys.settrace(trace_func)
            return 1

        def parent() -> int:
            # This frame's return address is patched; when activate_tracing
            # returns, parent transitions to interpreter mode
            a = activate_tracing()
            b = 10  # LINE event should fire here (interpreter mode)
            return a + b

        def grandparent() -> int:
            # This frame's return address is also patched; when parent returns,
            # grandparent transitions to interpreter mode
            x = 100
            y = parent()
            z = 200  # LINE event expected here
            return x + y + z

        force_compile(parent)
        force_compile(grandparent)
        self.assertTrue(is_jit_compiled(grandparent))
        self.assertTrue(is_jit_compiled(parent))

        result = grandparent()

        self.assertEqual(result, 311)

        # Grandparent should receive LINE events after parent returns
        # through the deopt trampoline via multi-frame deopt chaining.
        grandparent_line_events = [e for e in line_events if e[0] == "grandparent"]
        self.assertGreater(
            len(grandparent_line_events),
            0,
            "settrace should cause LINE events to fire for grandparent after "
            "parent returns through deopt trampoline",
        )

        sys.settrace(None)
