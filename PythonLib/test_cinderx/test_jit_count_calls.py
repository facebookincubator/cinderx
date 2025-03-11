# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import unittest

import cinderx.jit
from cinderx.jit import count_interpreted_calls


class CountCallsTest(unittest.TestCase):
    @unittest.skipIf(cinderx.jit.is_enabled(), "Testing the no-init case")
    def test_no_init(self) -> None:
        def func():
            return 13

        self.assertEqual(count_interpreted_calls(func), 0)
        func()
        self.assertEqual(count_interpreted_calls(func), 0)

    @unittest.skipUnless(cinderx.jit.is_enabled(), "Stubs don't check argument types")
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
            # pyre-ignore[6]: Intentionally checking runtime behavior.
            count_interpreted_calls(count_interpreted_calls)

    @unittest.skipUnless(
        cinderx.jit.is_enabled() and cinderx.jit.auto_jit_threshold() > 0,
        "Testing JitAuto functionality",
    )
    def test_basic_auto(self) -> None:
        def func():
            return 15

        # Loop many times to check that the shadowcode threshold (50) isn't affecting
        # call tracking.
        for i in range(200):
            self.assertEqual(count_interpreted_calls(func), i)
            func()

    @unittest.skipUnless(
        cinderx.jit.is_enabled() and cinderx.jit.auto_jit_threshold() == 0,
        "Testing JitAll functionality",
    )
    def test_basic_all(self) -> None:
        def func():
            return 15

        # Loop many times to check that the shadowcode threshold (50) isn't affecting
        # call tracking.
        for i in range(200):
            # Will never count any calls as the function gets compiled immediately.
            self.assertEqual(count_interpreted_calls(func), 0)
            func()
