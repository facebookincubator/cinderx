# Copyright (c) Meta Platforms, Inc. and affiliates.

import unittest

from cinderx.jit import (
    INSTALLED,
    disable as disable_jit,
    enable as enable_jit,
    force_compile,
    lazy_compile,
    is_enabled as is_jit_enabled,
    is_jit_compiled,
    jit_suppress,
    pause as pause_jit,
)


@unittest.skipIf(not INSTALLED, "Tests functionality on the JIT")
class DisableEnableTests(unittest.TestCase):
    """
    These tests are finicky as they will disable the JIT for the entire
    process.  They must re-enable the JIT on exit otherwise things will get
    weird.
    """

    def test_is_enabled(self) -> None:
        self.assertTrue(is_jit_enabled())
        disable_jit()
        self.assertFalse(is_jit_enabled())
        enable_jit()
        self.assertTrue(is_jit_enabled())

    def test_deopts(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

    def test_suppress_and_reopt(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        jit_suppress(foo)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()
        self.assertFalse(is_jit_compiled(foo))

    def test_disable_then_deopt(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit()
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

    def test_already_disabled(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        disable_jit(deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

    def test_already_enabled(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

    def test_compile_new_after_reenable(self) -> None:
        disable_jit(deopt_all=True)

        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

    def test_pause(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        with pause_jit(deopt_all=False):
            self.assertFalse(is_jit_enabled())
            self.assertTrue(is_jit_compiled(foo))

        self.assertTrue(is_jit_enabled())
        self.assertTrue(is_jit_compiled(foo))

        with pause_jit(deopt_all=True):
            self.assertFalse(is_jit_enabled())

            self.assertFalse(is_jit_compiled(foo))
            self.assertFalse(force_compile(foo))
            self.assertFalse(lazy_compile(foo))
            self.assertFalse(is_jit_compiled(foo))

        self.assertTrue(is_jit_enabled())
        self.assertTrue(is_jit_compiled(foo))

    def test_pause_nested(self) -> None:
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        with pause_jit(deopt_all=True):
            self.assertFalse(is_jit_enabled())
            self.assertFalse(is_jit_compiled(foo))

            with pause_jit(deopt_all=True):
                self.assertFalse(is_jit_enabled())
                self.assertFalse(is_jit_compiled(foo))

            self.assertFalse(is_jit_enabled())
            self.assertFalse(is_jit_compiled(foo))

        self.assertTrue(is_jit_enabled())
        self.assertTrue(is_jit_compiled(foo))

    def test_pause_between_lazy_compile(self) -> None:
        def foo(a, b):
            return a + b

        self.assertFalse(is_jit_compiled(foo))
        self.assertTrue(lazy_compile(foo))

        with pause_jit():
            foo(1, 2)
            self.assertFalse(is_jit_compiled(foo))

        self.assertTrue(is_jit_enabled())
        self.assertFalse(is_jit_compiled(foo))
        foo(3, 4)
        self.assertTrue(is_jit_compiled(foo))

if __name__ == "__main__":
    unittest.main()
