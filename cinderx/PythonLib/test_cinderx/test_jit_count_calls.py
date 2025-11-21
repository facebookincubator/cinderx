# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import unittest

import cinderx

cinderx.init()

import cinderx.jit
from cinderx.jit import count_interpreted_calls
from cinderx.test_support import passIf, passUnless


class CountCallsTest(unittest.TestCase):
    @passIf(cinderx.is_initialized(), "Testing the no-init case")
    def test_no_init(self) -> None:
        def func():
            return 13

        self.assertEqual(count_interpreted_calls(func), 0)
        func()
        func()
        func()
        self.assertEqual(count_interpreted_calls(func), 0)

    @passUnless(cinderx.jit.is_enabled(), "Stubs don't check argument types")
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

    @passUnless(
        cinderx.jit.is_enabled()
        and cinderx.jit.get_compile_after_n_calls() is not None,
        "Testing JitAuto functionality",
    )
    def test_basic_auto(self) -> None:
        def func():
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

    @passUnless(
        cinderx.jit.is_enabled() and cinderx.jit.get_compile_after_n_calls() == 0,
        "Testing JitAll functionality",
    )
    def test_basic_all(self) -> None:
        def func():
            return 15

        # Loop many times to check that the shadowcode threshold (50) isn't affecting
        # call tracking.
        for _ in range(2000):
            # Will never count any calls as the function gets compiled immediately.
            self.assertEqual(count_interpreted_calls(func), 0)
            func()
