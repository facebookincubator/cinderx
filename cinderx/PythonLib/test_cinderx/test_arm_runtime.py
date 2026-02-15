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
        self.assertTrue(cinderx.jit.is_enabled())

    def test_jit_force_compile_smoke(self) -> None:
        def f(x: int) -> int:
            return x + 1

        self.assertTrue(cinderx.jit.force_compile(f))
        self.assertTrue(cinderx.jit.is_jit_compiled(f))
        self.assertEqual(f(41), 42)


if __name__ == "__main__":
    unittest.main()

