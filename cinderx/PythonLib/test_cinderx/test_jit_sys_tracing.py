# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

# pyre-strict

import sys
import unittest
from types import FrameType

import cinderx.jit


def compiled_func(a: int, b: int) -> int:
    return a + b


class JitSysTracingTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.auto()
        cinderx.jit.force_compile(compiled_func)

    def tearDown(self) -> None:
        sys.setprofile(None)
        sys.settrace(None)

        cinderx.jit.force_uncompile(compiled_func)

    def test_setprofile_register_then_unregister(self) -> None:
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        events = 0

        def profiler(frame: FrameType, event: str, arg: object) -> None:
            nonlocal events
            events += 1

        # This should deopt all functions and disable the JIT.
        sys.setprofile(profiler)

        self.assertFalse(cinderx.jit.is_enabled())
        self.assertFalse(cinderx.jit.is_jit_compiled(compiled_func))
        self.assertGreater(events, 0)

        # Should reopt functions and re-enable the JIT.
        sys.setprofile(None)

        events_after_stop = events

        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))
        self.assertEqual(events_after_stop, events)

    def test_setprofile_unregister_only(self) -> None:
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        # Should have no effect.
        sys.setprofile(None)
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        # Still no effect.
        sys.setprofile(None)
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

    def test_setprofile_register_twice_then_unregister_once(self) -> None:
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        events = 0

        def profiler(frame: FrameType, event: str, arg: object) -> None:
            nonlocal events
            events += 1

        sys.setprofile(profiler)
        sys.setprofile(profiler)
        self.assertFalse(cinderx.jit.is_enabled())
        self.assertFalse(cinderx.jit.is_jit_compiled(compiled_func))
        self.assertGreater(events, 0)

        sys.setprofile(None)

        events_after_stop = events

        # Shouldn't matter that profiler was set twice.
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))
        self.assertEqual(events_after_stop, events)

    def test_settrace_register_then_unregister(self) -> None:
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        events = 0

        def tracer(frame: FrameType, event: str, arg: object) -> None:
            nonlocal events
            events += 1

        # This should deopt all functions and disable the JIT.
        sys.settrace(tracer)
        self.assertFalse(cinderx.jit.is_enabled())
        self.assertFalse(cinderx.jit.is_jit_compiled(compiled_func))
        self.assertGreater(events, 0)

        # Should reopt functions and re-enable the JIT.
        sys.settrace(None)

        events_after_stop = events

        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))
        self.assertEqual(events_after_stop, events)

    def test_settrace_unregister_only(self) -> None:
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        # Should have had no effect.
        sys.settrace(None)
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        # Still no effect.
        sys.settrace(None)
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

    def test_settrace_register_twice_then_unregister_once(self) -> None:
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        events = 0

        def tracer(frame: FrameType, event: str, arg: object) -> None:
            nonlocal events
            events += 1

        sys.settrace(tracer)
        sys.settrace(tracer)
        self.assertFalse(cinderx.jit.is_enabled())
        self.assertFalse(cinderx.jit.is_jit_compiled(compiled_func))
        self.assertGreater(events, 0)

        sys.settrace(None)

        events_after_stop = events

        # Shouldn't matter that tracer was set twice.
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))
        self.assertEqual(events_after_stop, events)
