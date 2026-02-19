# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

# pyre-strict

import platform
import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passIf, passUnless


CINDERX_INITIALIZED: bool = cinderx.is_initialized()
ALREADY_INSTALLED: bool = cinderx.is_frame_evaluator_installed()
JIT_ENABLED: bool = cinderx.jit.is_enabled()
ARM: bool = "aarch64" in platform.machine() or "arm64" in platform.machine()


@passUnless(CINDERX_INITIALIZED, "Need CinderX initialized to test frame evaluator")
@passIf(ALREADY_INSTALLED, "Can only test frame evaluator with a clean slate")
class FrameEvaluatorTest(unittest.TestCase):
    def test_install_then_remove(self) -> None:
        self.assertFalse(cinderx.is_frame_evaluator_installed())

        cinderx.install_frame_evaluator()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.remove_frame_evaluator()
        self.assertFalse(cinderx.is_frame_evaluator_installed())

    def test_idempotent(self) -> None:
        self.assertFalse(cinderx.is_frame_evaluator_installed())

        cinderx.install_frame_evaluator()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.install_frame_evaluator()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.remove_frame_evaluator()
        self.assertFalse(cinderx.is_frame_evaluator_installed())

        cinderx.remove_frame_evaluator()
        self.assertFalse(cinderx.is_frame_evaluator_installed())

    @passIf(JIT_ENABLED, "Need to check JIT with a clean slate")
    @passIf(ARM, "JIT doesn't work on ARM yet")
    def test_jit_auto(self) -> None:
        self.assertFalse(cinderx.is_frame_evaluator_installed())
        self.assertFalse(cinderx.jit.is_enabled())

        cinderx.jit.auto()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()

    @passIf(JIT_ENABLED, "Need to check JIT with a clean slate")
    @passIf(ARM, "JIT doesn't work on ARM yet")
    def test_jit_compile_after_n_calls(self) -> None:
        self.assertFalse(cinderx.is_frame_evaluator_installed())
        self.assertFalse(cinderx.jit.is_enabled())

        cinderx.jit.compile_after_n_calls(40000)
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()

    @passIf(JIT_ENABLED, "Need to check JIT with a clean slate")
    @passIf(ARM, "JIT doesn't work on ARM yet")
    def test_jit_force_compile(self) -> None:
        self.assertFalse(cinderx.is_frame_evaluator_installed())
        self.assertFalse(cinderx.jit.is_enabled())

        def foo(a: int, b: int) -> int:
            return a + b

        cinderx.jit.force_compile(foo)
        self.assertTrue(cinderx.jit.is_jit_compiled(foo))
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.force_uncompile(foo)
        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()

    @passIf(JIT_ENABLED, "Need to check JIT with a clean slate")
    @passIf(ARM, "JIT doesn't work on ARM yet")
    def test_jit_lazy_compile(self) -> None:
        self.assertFalse(cinderx.is_frame_evaluator_installed())
        self.assertFalse(cinderx.jit.is_enabled())

        def foo(a: int, b: int) -> int:
            return a + b

        cinderx.jit.lazy_compile(foo)
        self.assertFalse(cinderx.jit.is_jit_compiled(foo))
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        self.assertEqual(foo(1, 2), 3)
        self.assertTrue(cinderx.jit.is_jit_compiled(foo))
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.force_uncompile(foo)
        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()


if __name__ == "__main__":
    unittest.main()
