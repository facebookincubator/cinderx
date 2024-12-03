# Copyright (c) Meta Platforms, Inc. and affiliates.

import unittest

HAVE_CINDER: bool = True

try:
    from cinderjit import (
        disable as disable_jit,
        enable as enable_jit,
        force_compile,
        is_jit_compiled,
        jit_suppress,
    )
except ImportError:
    HAVE_CINDER = False

    def force_compile(func):
        return False

    def jit_suppress(func):
        return func

    def is_jit_compiled(func):
        return False


@unittest.skipIf(not HAVE_CINDER, "Tests functionality on cinderjit module")
class DisableEnableTests(unittest.TestCase):
    """
    These tests are finicky as they will disable the JIT for the entire
    process.  They must re-enable the JIT on exit otherwise things will get
    weird.
    """

    def test_deopts(self):
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(compile_all=False, deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

    def test_suppress_and_reopt(self):
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(compile_all=False, deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        jit_suppress(foo)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()
        self.assertFalse(is_jit_compiled(foo))

    def test_disable_then_deopt(self):
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(compile_all=False)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(compile_all=False, deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

    def test_already_disabled(self):
        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(compile_all=False, deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        disable_jit(compile_all=False, deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

    def test_already_enabled(self):
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

    def test_compile_new_after_reenable(self):
        disable_jit(compile_all=False, deopt_all=True)

        def foo(a, b):
            return a + b

        force_compile(foo)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))


if __name__ == "__main__":
    unittest.main()
