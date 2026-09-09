# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from cinderx.jit import (
    disable as disable_jit,
    enable as enable_jit,
    force_compile,
    force_uncompile,
    get_compiled_function,
    is_enabled as is_jit_enabled,
    is_jit_compiled,
    jit_suppress,
    jit_unsuppress,
    lazy_compile,
    pause as pause_jit,
)
from cinderx.test_support import passUnless, subprocess_env


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
        def foo(a: int, b: int) -> int:
            return a + b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        enable_jit()
        self.assertTrue(is_jit_compiled(foo))

    def test_get_compiled_function_while_deopted(self) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        force_compile(foo)
        self.assertIsNotNone(get_compiled_function(foo))

        disable_jit(deopt_all=True)
        # The function is off its compiled entry point, so it reports no
        # compile even while one is being held for it.
        self.assertIsNone(get_compiled_function(foo))
        self.assertEqual(foo(3, 4), 7)

        enable_jit()
        self.assertIsNotNone(get_compiled_function(foo))

    def test_deopt_all_preserves_compile(self) -> None:
        """
        Disabling the JIT takes every function off its compiled code, but the
        compiled code itself has to survive the disabled window: re-enabling
        must put the same compile back rather than build it again.
        """
        with tempfile.TemporaryDirectory() as tmp_dir:
            code = textwrap.dedent("""
            import cinderx.jit

            def foo(a, b):
                return a + b

            cinderx.jit.force_compile(foo)
            compiled = cinderx.jit.get_compiled_function(foo)
            assert compiled is not None
            # Nothing outside the JIT may hold the compile, or the test would
            # pass no matter what the JIT did with it.
            compiled_id = id(compiled)
            del compiled

            cinderx.jit.disable(deopt_all=True)
            assert not cinderx.jit.is_jit_compiled(foo)
            assert foo(3, 4) == 7

            cinderx.jit.enable()
            assert cinderx.jit.is_jit_compiled(foo)
            assert foo(3, 4) == 7
            assert id(cinderx.jit.get_compiled_function(foo)) == compiled_id
            print("OK")
            """)

            test_file = Path(tmp_dir) / "mod.py"
            test_file.write_text(code)

            # Counting the compiles is what makes this airtight: an identical
            # object address could in principle be a fresh compile that landed
            # in the freed one's memory.
            env = {**subprocess_env(), "PYTHONJITDEBUG": "1"}
            proc = subprocess.run(
                [sys.executable, str(test_file)],
                check=True,
                capture_output=True,
                encoding="utf-8",
                env=env,
            )
            self.assertIn("OK", proc.stdout, proc)
            compiles = sum(
                1
                for line in proc.stderr.splitlines()
                if line.startswith("JIT:") and line.endswith("Compiling __main__:foo")
            )
            self.assertEqual(compiles, 1, proc.stderr)

    def test_code_change_while_deopted(self) -> None:
        def foo(a: int, b: int) -> int:
            return a + b

        def bar(a: int, b: int) -> int:
            return a * b

        force_compile(foo)
        self.assertTrue(is_jit_compiled(foo))

        disable_jit(deopt_all=True)
        self.assertFalse(is_jit_compiled(foo))

        # A function deopted by disabling the JIT is still holding the compile
        # it came off, and giving that up is what stops it going back on
        # machine code built for a code object it no longer has.
        foo.__code__ = bar.__code__
        self.assertEqual(foo(3, 4), 12)

        enable_jit()
        self.assertEqual(foo(3, 4), 12)

    def test_suppress_and_reopt(self) -> None:
        def foo(a: int, b: int) -> int:
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
        def foo(a: int, b: int) -> int:
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
        def foo(a: int, b: int) -> int:
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
        def foo(a: int, b: int) -> int:
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

        def foo(a: int, b: int) -> int:
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
        def foo(a: int, b: int) -> int:
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
        def foo(a: int, b: int) -> int:
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
        def foo(a: int, b: int) -> int:
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

    def test_compile_no_config(self) -> None:
        """
        Test how code behaves when it forces compilation without any other
        configuration or options enabled.
        """

        with tempfile.TemporaryDirectory() as tmp_dir:
            code = textwrap.dedent("""
            import cinderx.jit

            def inc(x):
                return x + 1

            assert not cinderx.jit.is_jit_compiled(inc)
            cinderx.jit.force_compile(inc)
            assert cinderx.jit.is_jit_compiled(inc)
            """)

            test_file = Path(tmp_dir) / "mod.py"
            test_file.write_text(code)

            subprocess.run(
                [sys.executable, str(test_file)],
                check=True,
                env=subprocess_env(),
            )

    def test_auto(self) -> None:
        """
        Basic test for cinderx.jit.auto().
        """

        with tempfile.TemporaryDirectory() as tmp_dir:
            code = textwrap.dedent("""
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
            """)

            test_file = Path(tmp_dir) / "mod.py"
            test_file.write_text(code)

            subprocess.run(
                [sys.executable, str(test_file)],
                check=True,
                env={"CINDERX_JIT_BACKGROUND_COMPILE": "0", **subprocess_env()},
            )

    def test_auto_predefined(self) -> None:
        """
        Test that cinderx.jit.auto() works for functions that were defined
        before it was called.
        """

        with tempfile.TemporaryDirectory() as tmp_dir:
            code = textwrap.dedent("""
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
            """)

            test_file = Path(tmp_dir) / "mod.py"
            test_file.write_text(code)

            subprocess.run(
                [sys.executable, str(test_file)],
                check=True,
                env={"CINDERX_JIT_BACKGROUND_COMPILE": "0", **subprocess_env()},
            )

    def test_compile_after_n_calls(self) -> None:
        """
        Basic test for cinderx.jit.compile_after_n_calls().
        """

        with tempfile.TemporaryDirectory() as tmp_dir:
            code = textwrap.dedent("""
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
            """)

            test_file = Path(tmp_dir) / "mod.py"
            test_file.write_text(code)

            subprocess.run(
                [sys.executable, str(test_file)],
                check=True,
                env={"CINDERX_JIT_BACKGROUND_COMPILE": "0", **subprocess_env()},
            )

    def test_compile_after_n_calls_predefined(self) -> None:
        """
        Test that cinderx.jit.compile_after_n_calls() works for functions that
        were defined before it was called.
        """

        with tempfile.TemporaryDirectory() as tmp_dir:
            code = textwrap.dedent("""
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
            """)

            test_file = Path(tmp_dir) / "mod.py"
            test_file.write_text(code)

            subprocess.run(
                [sys.executable, str(test_file)],
                check=True,
                env={"CINDERX_JIT_BACKGROUND_COMPILE": "0", **subprocess_env()},
            )


if __name__ == "__main__":
    unittest.main()
