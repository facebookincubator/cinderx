# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import sys
import threading
import unittest

try:
    import _testinternalcapi
except ImportError:
    _testinternalcapi = None

import cinderx
import cinderx.jit
from cinderx.test_support import run_in_fresh_process


_inline_frames = []


def _foreign_eval_inline_callee(value):
    return value + 1


def _foreign_eval_inline_caller(value):
    _testinternalcapi.set_eval_frame_record(_inline_frames)
    return _foreign_eval_inline_callee(value)


class FrameEvaluatorTest(unittest.TestCase):
    @run_in_fresh_process
    def test_install_then_remove(self) -> None:
        cinderx.install_frame_evaluator()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.remove_frame_evaluator()
        self.assertFalse(cinderx.is_frame_evaluator_installed())

    @run_in_fresh_process
    def test_idempotent(self) -> None:
        cinderx.install_frame_evaluator()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.install_frame_evaluator()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.remove_frame_evaluator()
        self.assertFalse(cinderx.is_frame_evaluator_installed())

        cinderx.remove_frame_evaluator()
        self.assertFalse(cinderx.is_frame_evaluator_installed())

    @run_in_fresh_process
    def test_jit_auto(self) -> None:
        cinderx.jit.auto()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()

    @run_in_fresh_process
    def test_jit_compile_after_n_calls(self) -> None:
        cinderx.jit.compile_after_n_calls(40000)
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()

    @run_in_fresh_process
    def test_jit_force_compile(self) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        cinderx.jit.enable()
        cinderx.jit.force_compile(foo)
        self.assertTrue(cinderx.jit.is_jit_compiled(foo))
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.force_uncompile(foo)
        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()

    @run_in_fresh_process
    def test_jit_lazy_compile(self) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        cinderx.jit.enable()
        cinderx.jit.lazy_compile(foo)
        self.assertFalse(cinderx.jit.is_jit_compiled(foo))
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        self.assertEqual(foo(1, 2), 3)
        self.assertTrue(cinderx.jit.is_jit_compiled(foo))
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.force_uncompile(foo)
        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()


@unittest.skipUnless(
    _testinternalcapi is not None
    and hasattr(_testinternalcapi, "set_eval_frame_record"),
    "requires CPython's PEP 523 test helper",
)
class ForeignFrameEvaluatorTest(unittest.TestCase):
    @run_in_fresh_process
    def test_first_generator_and_coroutine_resume_observes_hook(self) -> None:
        def generator(value):
            yield value + 1

        async def coroutine(value):
            return value + 1

        cinderx.jit.force_compile(generator)
        cinderx.jit.force_compile(coroutine)
        gen = generator(41)
        coro = coroutine(41)
        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            self.assertEqual(next(gen), 42)
            with self.assertRaises(StopIteration) as stopped:
                coro.send(None)
            self.assertEqual(stopped.exception.value, 42)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
            gen.close()
            coro.close()
        self.assertEqual(frames.count("generator"), 1)
        self.assertEqual(frames.count("coroutine"), 1)
        self.assertTrue(cinderx.jit.is_jit_compiled(generator))
        self.assertTrue(cinderx.jit.is_jit_compiled(coroutine))

    @run_in_fresh_process
    def test_fresh_entry_with_tracing(self) -> None:
        def target(value):
            def inner():
                return value + 1

            return inner()

        events = []

        def trace(frame, event, arg):
            if frame.f_code is target.__code__ and event == "call":
                events.append(event)
            return trace

        cinderx.jit.force_compile(target)
        frames = []
        sys.settrace(trace)
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            self.assertEqual(target(41), 42)
        finally:
            _testinternalcapi.set_eval_frame_default()
            sys.settrace(None)
            cinderx.install_frame_evaluator()
        self.assertEqual(frames.count("target"), 1)
        self.assertEqual(events, ["call"])

    @run_in_fresh_process
    def test_arguments_captured_by_inner_function(self) -> None:
        offset = 5

        def target(value, unused, *, scale=2):
            def inner():
                return value * scale + offset

            return inner()

        cinderx.jit.force_compile(target)
        self.assertTrue(cinderx.jit.is_jit_compiled(target))
        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            self.assertEqual(target(7, object(), scale=3), 26)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
        self.assertEqual(frames.count("target"), 1)
        self.assertTrue(cinderx.jit.is_jit_compiled(target))

    @run_in_fresh_process
    def test_hot_function_is_observed_and_remains_compiled(self) -> None:
        effects = []

        def target(value):
            effects.append(value)
            return value + 1

        cinderx.jit.force_compile(target)
        self.assertTrue(cinderx.jit.is_jit_compiled(target))
        self.assertEqual(target(1), 2)
        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            self.assertEqual(target(41), 42)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
        self.assertEqual(frames.count("target"), 1)
        self.assertEqual(effects, [1, 41])
        self.assertTrue(cinderx.jit.is_jit_compiled(target))
        self.assertEqual(target(9), 10)
        self.assertTrue(cinderx.jit.is_jit_compiled(target))

    @run_in_fresh_process
    def test_bound_arguments_and_closure_are_preserved(self) -> None:
        offset = 7

        def target(value, *args, scale=2, **kwargs):
            return (value + sum(args) + kwargs["extra"] + offset) * scale

        cinderx.jit.force_compile(target)
        self.assertTrue(cinderx.jit.is_jit_compiled(target))
        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            self.assertEqual(target(1, 2, 3, scale=3, extra=4), 51)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
        self.assertEqual(frames.count("target"), 1)

    @run_in_fresh_process
    def test_nested_native_calls_are_observed(self) -> None:
        def callee(value):
            return value * 2

        def caller(value):
            return callee(value) + 1

        cinderx.jit.force_compile(callee)
        cinderx.jit.force_compile(caller)
        self.assertTrue(cinderx.jit.is_jit_compiled(callee))
        self.assertTrue(cinderx.jit.is_jit_compiled(caller))
        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            self.assertEqual(caller(20), 41)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
        self.assertEqual(frames.count("caller"), 1)
        self.assertEqual(frames.count("callee"), 1)

    @run_in_fresh_process
    def test_exception_and_hook_restoration(self) -> None:
        effects = []

        def target(value):
            effects.append(value)
            if value < 0:
                raise ValueError("negative input")
            return value

        cinderx.jit.force_compile(target)
        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            with self.assertRaisesRegex(ValueError, "negative input"):
                target(-1)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
        self.assertEqual(frames.count("target"), 1)
        self.assertEqual(effects, [-1])
        self.assertEqual(target(3), 3)
        self.assertTrue(cinderx.jit.is_jit_compiled(target))

    @run_in_fresh_process
    def test_hook_installed_inside_native_caller_observes_callee(self) -> None:
        callee = _foreign_eval_inline_callee
        caller = _foreign_eval_inline_caller
        _inline_frames.clear()
        cinderx.jit.enable_hir_inliner()
        cinderx.jit.force_compile(callee)
        cinderx.jit.force_compile(caller)
        self.assertTrue(cinderx.jit.is_jit_compiled(callee))
        self.assertTrue(cinderx.jit.is_jit_compiled(caller))
        self.assertGreater(cinderx.jit.get_num_inlined_functions(caller), 0)
        try:
            self.assertEqual(caller(41), 42)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
        # The caller already entered native code before installing the hook.
        # The callee's own check must survive actual inlining. Its suspended
        # caller has already entered native code and must resume as an ordinary
        # deopt without re-entering the newly installed hook mid-function.
        self.assertEqual(_inline_frames.count(callee.__name__), 1)
        self.assertEqual(_inline_frames.count(caller.__name__), 0)
        self.assertTrue(cinderx.jit.is_jit_compiled(caller))
        self.assertTrue(cinderx.jit.is_jit_compiled(callee))

    @run_in_fresh_process
    def test_failed_first_installation_preserves_hook_and_can_recover(self) -> None:
        cinderx.remove_frame_evaluator()

        def target(value):
            return value + 1

        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            # A rejected installation must not mark CinderX initialized and
            # cause the next attempt to silently accept the foreign hook.
            for _ in range(2):
                with self.assertRaisesRegex(RuntimeError, "something else"):
                    cinderx.install_frame_evaluator()
                self.assertEqual(target(41), 42)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
        self.assertEqual(frames.count("target"), 2)
        self.assertTrue(cinderx.is_frame_evaluator_installed())
        cinderx.jit.force_compile(target)
        self.assertTrue(cinderx.jit.is_jit_compiled(target))
        self.assertEqual(target(9), 10)

    @run_in_fresh_process
    def test_hook_installed_on_another_thread_is_observed(self) -> None:
        def target(value):
            return value + 1

        cinderx.jit.force_compile(target)
        self.assertTrue(cinderx.jit.is_jit_compiled(target))
        ready = threading.Event()
        proceed = threading.Event()
        results = []

        def worker():
            ready.set()
            if proceed.wait(timeout=10):
                results.append(target(41))

        thread = threading.Thread(target=worker)
        thread.start()
        self.assertTrue(ready.wait(timeout=10))
        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            proceed.set()
            thread.join(timeout=10)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
            proceed.set()
            thread.join(timeout=10)
        self.assertFalse(thread.is_alive())
        self.assertEqual(results, [42])
        self.assertEqual(frames.count("target"), 1)
        self.assertTrue(cinderx.jit.is_jit_compiled(target))
        self.assertEqual(target(9), 10)

    @run_in_fresh_process
    def test_compile_while_foreign_evaluator_is_active(self) -> None:
        cinderx.install_frame_evaluator()

        def target(value):
            return value + 1

        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            cinderx.jit.force_compile(target)
            self.assertTrue(cinderx.jit.is_jit_compiled(target))
            self.assertEqual(target(41), 42)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
        self.assertEqual(frames.count("target"), 1)

    @run_in_fresh_process
    def test_removal_does_not_remove_foreign_evaluator(self) -> None:
        cinderx.install_frame_evaluator()

        def target(value):
            return value + 1

        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            cinderx.remove_frame_evaluator()
            self.assertEqual(target(41), 42)
        finally:
            _testinternalcapi.set_eval_frame_default()
            cinderx.install_frame_evaluator()
        self.assertEqual(frames.count("target"), 1)


if __name__ == "__main__":
    unittest.main()
