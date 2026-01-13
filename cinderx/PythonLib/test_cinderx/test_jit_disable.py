# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import multiprocessing
import sys
import unittest
from pathlib import Path

from cinderx.jit import (
    disable as disable_jit,
    enable as enable_jit,
    force_compile,
    force_uncompile,
    is_enabled as is_jit_enabled,
    is_jit_compiled,
    jit_suppress,
    jit_unsuppress,
    lazy_compile,
    pause as pause_jit,
)
from cinderx.test_support import passUnless


@passUnless(is_jit_enabled(), "Tests functionality on the JIT")
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

        # Code object persists across multiple runs of the test.  Need to reset the
        # suppress flag to support this being run multiple times for refleak detection.
        jit_unsuppress(foo)

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

        # The compiled code for `foo` will stay compiled across test runs by default.
        # We need to evict it to support multiple runs of the test for refleak
        # detection.
        force_uncompile(foo)

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

        # The compiled code for `foo` will stay compiled across test runs by default.
        # We need to evict it to support multiple runs of the test for refleak
        # detection.
        force_uncompile(foo)

    @staticmethod
    def compile_no_config_test() -> None:
        import cinderx.jit

        def inc(x):
            return x + 1

        assert not cinderx.jit.is_jit_compiled(inc)
        cinderx.jit.force_compile(inc)
        assert cinderx.jit.is_jit_compiled(inc)

    def test_compile_no_config(self) -> None:
        """
        Test how code behaves when it forces compilation without any other
        configuration or options enabled.
        """

        p = multiprocessing.Process(target=DisableEnableTests.compile_no_config_test)
        p.start()
        p.join()
        self.assertEqual(p.exitcode, 0)

    @staticmethod
    def auto_test() -> None:
        import cinderx.jit

        def predefined(x):
            return x + x

        cinderx.jit.auto()

        def inc(x):
            return x + 1

        assert not cinderx.jit.is_jit_compiled(inc)
        for i in range(1000):
            inc(i)
        assert not cinderx.jit.is_jit_compiled(inc)

        inc(1001)
        assert cinderx.jit.is_jit_compiled(inc)

    def test_auto(self) -> None:
        """
        Basic test for cinderx.jit.auto().
        """

        p = multiprocessing.Process(target=DisableEnableTests.auto_test)
        p.start()
        p.join()
        self.assertEqual(p.exitcode, 0)

    @staticmethod
    def auto_predefined_test() -> None:
        import cinderx.jit

        def predefined(x):
            return x + x

        cinderx.jit.auto()

        assert not cinderx.jit.is_jit_compiled(predefined)
        for i in range(1000):
            predefined(i)
        assert not cinderx.jit.is_jit_compiled(predefined)

        predefined(1001)
        assert cinderx.jit.is_jit_compiled(predefined)

    def test_auto_predefined(self) -> None:
        """
        Test that cinderx.jit.auto() works for functions that were defined
        before it was called.
        """

        p = multiprocessing.Process(target=DisableEnableTests.auto_predefined_test)
        p.start()
        p.join()
        self.assertEqual(p.exitcode, 0)

    @staticmethod
    def compile_after_n_calls_test() -> None:
        import cinderx.jit

        cinderx.jit.compile_after_n_calls(2)

        def inc(x):
            return x + 1

        assert not cinderx.jit.is_jit_compiled(inc)
        inc(1)
        inc(2)
        assert not cinderx.jit.is_jit_compiled(inc)

        inc(3)
        assert cinderx.jit.is_jit_compiled(inc)

        # Change the setting and see it takes affect.

        cinderx.jit.compile_after_n_calls(5)

        def dec(x):
            return x - 1

        assert not cinderx.jit.is_jit_compiled(dec)
        dec(1)
        dec(2)
        dec(3)
        dec(4)
        dec(5)
        assert not cinderx.jit.is_jit_compiled(dec)

        dec(6)
        assert cinderx.jit.is_jit_compiled(dec)

    def test_compile_after_n_calls(self) -> None:
        """
        Basic test for cinderx.jit.compile_after_n_calls().
        """

        p = multiprocessing.Process(
            target=DisableEnableTests.compile_after_n_calls_test
        )
        p.start()
        p.join()
        self.assertEqual(p.exitcode, 0)

    @staticmethod
    def compile_after_n_calls_predefined_test() -> None:
        import cinderx.jit

        def predefined(x):
            return x + x

        cinderx.jit.compile_after_n_calls(2)

        assert not cinderx.jit.is_jit_compiled(predefined)
        predefined(1)
        predefined(2)
        assert not cinderx.jit.is_jit_compiled(predefined)

        predefined(3)
        assert cinderx.jit.is_jit_compiled(predefined)

    def test_compile_after_n_calls_predefined(self) -> None:
        """
        Test that cinderx.jit.compile_after_n_calls() works for functions that
        were defined before it was called.
        """

        p = multiprocessing.Process(
            target=DisableEnableTests.compile_after_n_calls_predefined_test
        )
        p.start()
        p.join()
        self.assertEqual(p.exitcode, 0)


if __name__ == "__main__":
    unittest.main()
