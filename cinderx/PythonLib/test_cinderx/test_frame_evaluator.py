# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import unittest

import cinderx
import cinderx.jit
from cinderx.test_support import passUnless, run_in_fresh_process


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


if __name__ == "__main__":
    unittest.main()
