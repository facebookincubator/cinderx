# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import unittest

import cinderx
import cinderx.jit
from cinderx.jit import count_interpreted_calls
from cinderx.test_support import passIf, passUnless


# Defined here because pyre is limited to typing constants in decorators.
INIT: bool = cinderx.is_initialized()
JIT_ENABLED: bool = cinderx.jit.is_enabled()
JIT_COMPILE_AFTER_N_CALLS: int | None = cinderx.jit.get_compile_after_n_calls()
JIT_AUTO: bool = (
    JIT_ENABLED
    and JIT_COMPILE_AFTER_N_CALLS is not None
    and JIT_COMPILE_AFTER_N_CALLS > 0
)
JIT_ALL: bool = JIT_ENABLED and JIT_COMPILE_AFTER_N_CALLS == 0


class CountCallsTest(unittest.TestCase):
    @passIf(INIT, "Testing the no-init case")
    def test_no_init(self) -> None:
        def func() -> int:
            return 13

        self.assertEqual(count_interpreted_calls(func), 0)
        func()
        func()
        func()
        self.assertEqual(count_interpreted_calls(func), 0)

    @passUnless(JIT_ENABLED, "Stubs don't check argument types")
    def test_bad_args(self) -> None:
        with self.assertRaises(TypeError):
            # pyre-ignore[6]: Intentionally checking runtime behavior.
            count_interpreted_calls(None)
        with self.assertRaises(TypeError):
            # pyre-ignore[6]: Intentionally checking runtime behavior.
            count_interpreted_calls(5)
        with self.assertRaises(TypeError):
            # pyre-ignore[6]: Intentionally checking runtime behavior.
            count_interpreted_calls("huh")
        with self.assertRaises(TypeError):
            count_interpreted_calls(count_interpreted_calls)

    @passUnless(JIT_AUTO, "Testing JitAuto functionality")
    def test_basic_auto(self) -> None:
        def func() -> int:
            return 15

        # Loop many times to check that the shadowcode threshold (50) isn't affecting
        # call tracking.
        for i in range(2000):
            # Stops counting after it gets compiled.
            call_limit = cinderx.jit.get_compile_after_n_calls()
            self.assertIsNotNone(call_limit)
            expected = min(i, call_limit)
            self.assertEqual(count_interpreted_calls(func), expected)
            func()

    @passUnless(JIT_ALL, "Testing JitAll functionality")
    def test_basic_all(self) -> None:
        def func() -> int:
            return 15

        # Loop many times to check that the shadowcode threshold (50) isn't affecting
        # call tracking.
        for _ in range(2000):
            # Will never count any calls as the function gets compiled immediately.
            self.assertEqual(count_interpreted_calls(func), 0)
            func()
