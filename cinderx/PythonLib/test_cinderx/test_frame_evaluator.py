# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

# pyre-strict

import platform
import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passIf, passUnless


CINDERX_INITIALIZED: bool = cinderx.is_initialized()
ARM: bool = "aarch64" in platform.machine() or "arm64" in platform.machine()


@passUnless(CINDERX_INITIALIZED, "Need CinderX initialized to test frame evaluator")
class FrameEvaluatorTest(unittest.TestCase):
    def test_install_then_remove(self) -> None:
        self.skip_if_already_installed()

        cinderx.install_frame_evaluator()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.remove_frame_evaluator()
        self.assertFalse(cinderx.is_frame_evaluator_installed())

    def test_idempotent(self) -> None:
        self.skip_if_already_installed()

        cinderx.install_frame_evaluator()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.install_frame_evaluator()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.remove_frame_evaluator()
        self.assertFalse(cinderx.is_frame_evaluator_installed())

        cinderx.remove_frame_evaluator()
        self.assertFalse(cinderx.is_frame_evaluator_installed())

    @passIf(ARM, "JIT doesn't work on ARM yet")
    def test_jit_auto(self) -> None:
        self.skip_if_already_installed()

        cinderx.jit.auto()
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()

    @passIf(ARM, "JIT doesn't work on ARM yet")
    def test_jit_compile_after_n_calls(self) -> None:
        self.skip_if_already_installed()

        cinderx.jit.compile_after_n_calls(40000)
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()

    @passIf(ARM, "JIT doesn't work on ARM yet")
    def test_jit_force_compile(self) -> None:
        self.skip_if_already_installed()

        def foo(a: int, b: int) -> int:
            return a + b

        cinderx.jit.enable()
        cinderx.jit.force_compile(foo)
        self.assertTrue(cinderx.jit.is_jit_compiled(foo))
        self.assertTrue(cinderx.is_frame_evaluator_installed())

        cinderx.jit.force_uncompile(foo)
        cinderx.jit.disable()
        cinderx.remove_frame_evaluator()

    @passIf(ARM, "JIT doesn't work on ARM yet")
    def test_jit_lazy_compile(self) -> None:
        self.skip_if_already_installed()

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

    def skip_if_already_installed(self) -> None:
        # JIT can be enabled via decorators from other test modules, so this has to
        # be run inside of the test function and not as a decorator itself.
        if cinderx.is_frame_evaluator_installed():
            self.skipTest("Have to test frame evaluator with a clean slate")


if __name__ == "__main__":
    unittest.main()
