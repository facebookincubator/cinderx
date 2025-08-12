# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

# This is in its own file as it modifies the global JIT-list.

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

from unittest import skipIf

import cinderx

cinderx.init()

import cinderx.jit

from cinderx.test_support import skip_unless_jit


ENCODING: str = sys.stdout.encoding or sys.getdefaultencoding()

# Hack to allow subprocesses to find where cinderx is.
MOD_PATH: str = os.path.dirname(os.path.dirname(cinderx.__file__))


@unittest.skipUnless(
    cinderx.jit.auto_jit_threshold() > 0,
    "Requires the JIT but is incompatible with JitAll mode",
)
class JitListTest(unittest.TestCase):
    def test_comments(self) -> None:
        cinderx.jit.append_jit_list("")
        initial_jit_list = cinderx.jit.get_jit_list()
        cinderx.jit.append_jit_list("# asdfasdfasd")
        cinderx.jit.append_jit_list("# x:y.z")
        cinderx.jit.append_jit_list("# x@y:1")
        self.assertEqual(initial_jit_list, cinderx.jit.get_jit_list())

    def test_py_func(self) -> None:
        def func():
            pass

        def func_nojit():
            pass

        cinderx.jit.append_jit_list(f"{func.__module__}:{func.__qualname__}")

        py_funcs = cinderx.jit.get_jit_list()[0]
        self.assertIn(func.__qualname__, py_funcs[__name__])
        self.assertNotIn(func_nojit.__qualname__, py_funcs[__name__])

        func()
        if cinderx.jit.auto_jit_threshold() <= 1:
            self.assertTrue(cinderx.jit.is_jit_compiled(func))
        func_nojit()
        self.assertFalse(cinderx.jit.is_jit_compiled(func_nojit))

    def test_py_meth(self) -> None:
        class JitClass:
            def meth(self):
                pass

            def meth_nojit(self):
                pass

        meth = JitClass.meth
        meth_nojit = JitClass.meth_nojit

        cinderx.jit.append_jit_list(f"{meth.__module__}:{meth.__qualname__}")

        py_funcs = cinderx.jit.get_jit_list()[0]
        self.assertIn(meth.__qualname__, py_funcs[__name__])
        self.assertNotIn(meth_nojit.__qualname__, py_funcs[__name__])

        meth(JitClass())
        if cinderx.jit.auto_jit_threshold() <= 1:
            self.assertTrue(cinderx.jit.is_jit_compiled(meth))
        meth_nojit(JitClass())
        self.assertFalse(cinderx.jit.is_jit_compiled(meth_nojit))

    def test_py_code(self) -> None:
        def code_func():
            pass

        def code_func_nojit():
            pass

        # pyre-ignore[16]: Pyre doesn't know about __code__.
        code_obj = code_func.__code__

        # Cheating a little here, because we don't have a code.co_qualname in 3.10.
        code_name = code_func.__qualname__
        cinderx.jit.append_jit_list(
            f"{code_name}@{code_obj.co_filename}:{code_obj.co_firstlineno}"
        )

        py_code_objs = cinderx.jit.get_jit_list()[1]
        thisfile = os.path.basename(__file__)
        self.assertIn(code_obj.co_firstlineno, py_code_objs[code_name][thisfile])
        code_func()
        if cinderx.jit.auto_jit_threshold() <= 1:
            self.assertTrue(cinderx.jit.is_jit_compiled(code_func))
        code_func_nojit()
        self.assertFalse(cinderx.jit.is_jit_compiled(code_func_nojit))

    def test_change_func_qualname(self) -> None:
        def inner_func():
            return 24

        cinderx.jit.append_jit_list(
            f"{inner_func.__module__}:{inner_func.__qualname__}_foo"
        )

        self.assertEqual(inner_func(), 24)
        self.assertFalse(cinderx.jit.is_jit_compiled(inner_func))

        inner_func.__qualname__ += "_foo"
        self.assertEqual(inner_func(), 24)
        if cinderx.jit.auto_jit_threshold() <= 1:
            self.assertTrue(cinderx.jit.is_jit_compiled(inner_func))

    def test_precompile_all(self) -> None:
        # Has to be run under a separate process because precompile_all will mess up the
        # other JIT-related tests.
        code = textwrap.dedent(
            """
            import cinderx
            cinderx.init()

            import cinderx.jit

            def func():
                return 24

            assert not cinderx.jit.is_jit_compiled(func)

            cinderx.jit.lazy_compile(func)

            # This has to use multiple threads otherwise this will take many minutes to run.
            # It'll be compiling all functions that were loaded and not-yet-run in JitAll
            # mode.
            assert cinderx.jit.precompile_all(workers=10)
            assert cinderx.jit.is_jit_compiled(func)

            print(func())
        """
        )

        with tempfile.TemporaryDirectory() as tmp:
            dirpath = Path(tmp)
            codepath = dirpath / "mod.py"
            codepath.write_text(code)
            proc = subprocess.run(
                [sys.executable, "-X", "jit", "mod.py"],
                stdout=subprocess.PIPE,
                cwd=tmp,
                encoding=ENCODING,
                env={"PYTHONPATH": MOD_PATH},
            )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertEqual(proc.stdout.strip(), "24")

    def test_read_jit_list(self) -> None:
        def func():
            return 35

        entry = f"{func.__module__}:{func.__qualname__}"

        with tempfile.NamedTemporaryFile("w+") as jit_list_file:
            jit_list_file.write(entry)
            jit_list_file.flush()
            cinderx.jit.read_jit_list(jit_list_file.name)

        entries = cinderx.jit.get_jit_list()[0]
        self.assertIn(func.__module__, entries)
        self.assertIn(func.__qualname__, entries[func.__module__])

        self.assertFalse(cinderx.jit.is_jit_compiled(func))
        self.assertEqual(func(), 35)

        if cinderx.jit.auto_jit_threshold() <= 1:
            self.assertTrue(cinderx.jit.is_jit_compiled(func))

    def test_append_jit_list_parse_error(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "Failed to parse new JIT list line"):
            cinderx.jit.append_jit_list("OH NO")

    def test_read_jit_list_parse_error(self) -> None:
        with self.assertRaisesRegex(
            RuntimeError, r"Error while parsing line \d+ in JIT list file"
        ):
            with tempfile.NamedTemporaryFile("w+") as jit_list_file:
                jit_list_file.write("OH NO")
                jit_list_file.flush()
                cinderx.jit.read_jit_list(jit_list_file.name)

    def test_precompile_all_bad_args(self) -> None:
        with self.assertRaises(ValueError):
            cinderx.jit.precompile_all(workers=-1)
        with self.assertRaises(ValueError):
            cinderx.jit.precompile_all(workers=200000)

    def test_default_parse_error_behavior_startup(self) -> None:
        code = 'print("Hello world!")'
        jitlist = "OH NO"
        with tempfile.TemporaryDirectory() as tmp:
            dirpath = Path(tmp)
            codepath = dirpath / "mod.py"
            jitlistpath = dirpath / "jitlist.txt"
            codepath.write_text(code)
            jitlistpath.write_text(jitlist)
            proc = subprocess.run(
                [
                    sys.executable,
                    "-X",
                    "jit-list-file=jitlist.txt",
                    "mod.py",
                ],
                capture_output=True,
                cwd=tmp,
                encoding=ENCODING,
                env={"PYTHONPATH": MOD_PATH},
            )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn(
            "Error while parsing line 1 in JIT list file jitlist.txt", proc.stderr
        )

    def test_fail_on_parse_error_startup(self) -> None:
        code = 'print("Hello world!")'
        jitlist = "OH NO"
        with tempfile.TemporaryDirectory() as tmp:
            dirpath = Path(tmp)
            codepath = dirpath / "mod.py"
            jitlistpath = dirpath / "jitlist.txt"
            codepath.write_text(code)
            jitlistpath.write_text(jitlist)
            proc = subprocess.run(
                [
                    sys.executable,
                    "-X",
                    "jit-list-file=jitlist.txt",
                    "-X",
                    "jit-list-fail-on-parse-error",
                    "mod.py",
                ],
                capture_output=True,
                cwd=tmp,
                encoding=ENCODING,
                env={"PYTHONPATH": MOD_PATH},
            )
        self.assertNotEqual(proc.returncode, 0, proc)
        self.assertIn("Error while parsing line", proc.stderr)
        self.assertIn("in JIT list file", proc.stderr)


if __name__ == "__main__":
    unittest.main()
