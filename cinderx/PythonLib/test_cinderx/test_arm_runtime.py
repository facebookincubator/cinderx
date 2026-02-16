# Copyright (c) Meta Platforms, Inc. and affiliates.

import platform
import unittest

import cinderx
import cinderx.jit


def is_arm_linux() -> bool:
    machine = platform.machine().lower()
    return platform.system() == "Linux" and machine in ("aarch64", "arm64")


@unittest.skipUnless(is_arm_linux(), "ARM Linux specific runtime checks")
class ArmRuntimeTests(unittest.TestCase):
    def test_runtime_initializes(self) -> None:
        self.assertTrue(cinderx.is_initialized())
        self.assertIsNone(cinderx.get_import_error())

    def test_jit_is_enabled(self) -> None:
        cinderx.jit.enable()
        self.assertTrue(cinderx.jit.is_enabled())

    def test_jit_force_compile_smoke(self) -> None:
        cinderx.jit.enable()
        # Ensure auto-jit doesn't kick in during the interpreted phase below.
        cinderx.jit.compile_after_n_calls(1000000)

        def f(n: int) -> int:
            s = 0
            for i in range(n):
                s += i
            return s

        # Prove we start interpreted: call count should increase.
        cinderx.jit.force_uncompile(f)
        self.assertFalse(cinderx.jit.is_jit_compiled(f))

        before = cinderx.jit.count_interpreted_calls(f)
        for _ in range(10):
            self.assertEqual(f(10), 45)
        after = cinderx.jit.count_interpreted_calls(f)
        self.assertGreater(after, before)

        # Force compilation and verify that subsequent calls don't bump the
        # interpreted call counter (i.e., compiled code is actually executing).
        self.assertTrue(cinderx.jit.force_compile(f))
        self.assertTrue(cinderx.jit.is_jit_compiled(f))
        self.assertGreater(cinderx.jit.get_compiled_size(f), 0)

        interp0 = cinderx.jit.count_interpreted_calls(f)
        for _ in range(2000):
            self.assertEqual(f(10), 45)
        interp1 = cinderx.jit.count_interpreted_calls(f)
        self.assertEqual(interp1, interp0)


if __name__ == "__main__":
    unittest.main()

