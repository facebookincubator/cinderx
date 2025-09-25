# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

import sys
import unittest
from types import CodeType
from typing import Callable

import cinderx.jit


TOOL_ID: int = 2
TOOL_NAME: str = "fake_tool"


def compiled_func(a: int, b: int) -> int:
    return a + b


@unittest.skipUnless(
    hasattr(sys, "monitoring"), f"sys.monitoring not supported by Python {sys.version}"
)
class JitSysMonitoringTests(unittest.TestCase):
    def setUp(self) -> None:
        cinderx.jit.auto()
        cinderx.jit.force_compile(compiled_func)

        sys.monitoring.use_tool_id(TOOL_ID, TOOL_NAME)
        sys.monitoring.set_events(TOOL_ID, sys.monitoring.events.CALL)

    def tearDown(self) -> None:
        cinderx.jit.force_uncompile(compiled_func)

        sys.monitoring.set_events(TOOL_ID, 0)
        sys.monitoring.free_tool_id(TOOL_ID)

    def test_register_then_unregister(self) -> None:
        events = 0

        def call_event_callback(
            code: CodeType, instr_offset: int, func: Callable[..., object], arg0: object
        ) -> None:
            nonlocal events
            events += 1

        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        # This should deopt all functions and disable the JIT.
        sys.monitoring.register_callback(
            TOOL_ID, sys.monitoring.events.CALL, call_event_callback
        )
        self.assertFalse(cinderx.jit.is_enabled())
        self.assertFalse(cinderx.jit.is_jit_compiled(compiled_func))

        # At least 2 calls for assertFalse() and another 2 for the cinderx.jit
        # functions.
        self.assertGreaterEqual(events, 4)

        events_before_call = events
        compiled_func(1, 2)
        events_after_call = events

        self.assertEqual(events_before_call, events_after_call - 1)

        # This should reopt all functions and re-enable the JIT.
        sys.monitoring.register_callback(TOOL_ID, sys.monitoring.events.CALL, None)
        events_after_stop = events

        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))
        self.assertEqual(events_after_stop, events)

    def test_unregister_only(self) -> None:
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        sys.monitoring.set_events(TOOL_ID, sys.monitoring.events.CALL)

        # Should have no effect.
        sys.monitoring.register_callback(TOOL_ID, sys.monitoring.events.CALL, None)
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        # Still nothing.
        sys.monitoring.register_callback(TOOL_ID, sys.monitoring.events.CALL, None)
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

    def test_register_twice_then_unregister_once(self) -> None:
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        def call_event_callback(
            code: CodeType, instr_offset: int, func: Callable[..., object], arg0: object
        ) -> None:
            pass

        sys.monitoring.register_callback(
            TOOL_ID, sys.monitoring.events.CALL, call_event_callback
        )
        sys.monitoring.register_callback(
            TOOL_ID + 1, sys.monitoring.events.CALL, call_event_callback
        )
        self.assertFalse(cinderx.jit.is_enabled())
        self.assertFalse(cinderx.jit.is_jit_compiled(compiled_func))

        # Still have one more tool registered.
        sys.monitoring.register_callback(TOOL_ID, sys.monitoring.events.CALL, None)
        self.assertFalse(cinderx.jit.is_enabled())
        self.assertFalse(cinderx.jit.is_jit_compiled(compiled_func))

        # Clean up the outstanding callback.
        sys.monitoring.register_callback(TOOL_ID + 1, sys.monitoring.events.CALL, None)

    def test_register_then_unregister_wrong(self) -> None:
        self.assertTrue(cinderx.jit.is_enabled())
        self.assertTrue(cinderx.jit.is_jit_compiled(compiled_func))

        def call_event_callback(
            code: CodeType, instr_offset: int, func: Callable[..., object], arg0: object
        ) -> None:
            pass

        sys.monitoring.register_callback(
            TOOL_ID, sys.monitoring.events.CALL, call_event_callback
        )
        self.assertFalse(cinderx.jit.is_enabled())
        self.assertFalse(cinderx.jit.is_jit_compiled(compiled_func))

        # Should do nothing as the tool ID doesn't match.
        sys.monitoring.register_callback(TOOL_ID + 1, sys.monitoring.events.CALL, None)
        self.assertFalse(cinderx.jit.is_enabled())
        self.assertFalse(cinderx.jit.is_jit_compiled(compiled_func))

        # Clean up the outstanding callback.
        sys.monitoring.register_callback(TOOL_ID, sys.monitoring.events.CALL, None)
