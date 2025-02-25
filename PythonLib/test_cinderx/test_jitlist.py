# Copyright (c) Meta Platforms, Inc. and affiliates.

# This is in its own file as it modifies the global JIT-list.

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest

from pathlib import Path

# pyre-ignore[21]: Pyre doesn't know about cpython/Lib/test.
from test.support.script_helper import assert_python_ok

from cinderx import test_support as cinder_support

try:
    import cinderjit
except:
    cinderjit = None


@cinder_support.skipUnlessJITEnabled("No JIT-list if no JIT")
class JitListTest(unittest.TestCase):
    def setUp(self) -> None:
        # Force the JIT list to exist.
        cinderjit.jit_list_append("foobar:baz")

    def test_comments(self) -> None:
        cinderjit.jit_list_append("")
        initial_jit_list = cinderjit.get_jit_list()
        cinderjit.jit_list_append("# asdfasdfasd")
        cinderjit.jit_list_append("# x:y.z")
        cinderjit.jit_list_append("# x@y:1")
        self.assertEqual(initial_jit_list, cinderjit.get_jit_list())

    def test_py_func(self) -> None:
        def func():
            pass

        def func_nojit():
            pass

        # Re-trigger lazy compilation, in case the JIT tried to compile the functions in
        # between them being defined and them being added to the JIT list.
        cinderjit.jit_list_append(f"{func.__module__}:{func.__qualname__}")
        cinderjit.lazy_compile(func)
        cinderjit.lazy_compile(func_nojit)
        py_funcs = cinderjit.get_jit_list()[0]
        self.assertIn(func.__qualname__, py_funcs[__name__])
        self.assertNotIn(func_nojit.__qualname__, py_funcs[__name__])

        func()
        if cinderjit.auto_jit_threshold() <= 1:
            self.assertTrue(cinderjit.is_jit_compiled(func))
        func_nojit()
        self.assertFalse(cinderjit.is_jit_compiled(func_nojit))

    def test_py_meth(self) -> None:
        class JitClass:
            def meth(self):
                pass

            def meth_nojit(self):
                pass

        meth = JitClass.meth
        meth_nojit = JitClass.meth_nojit

        cinderjit.jit_list_append(f"{meth.__module__}:{meth.__qualname__}")
        cinderjit.lazy_compile(meth)
        cinderjit.lazy_compile(meth_nojit)
        py_funcs = cinderjit.get_jit_list()[0]
        self.assertIn(meth.__qualname__, py_funcs[__name__])
        self.assertNotIn(meth_nojit.__qualname__, py_funcs[__name__])

        meth(JitClass())
        if cinderjit.auto_jit_threshold() <= 1:
            self.assertTrue(cinderjit.is_jit_compiled(meth))
        meth_nojit(JitClass())
        self.assertFalse(cinderjit.is_jit_compiled(meth_nojit))

    def test_py_code(self) -> None:
        def code_func():
            pass

        def code_func_nojit():
            pass

        # pyre-ignore[16]: Pyre doesn't know about __code__.
        code_obj = code_func.__code__

        # Cheating a little here, because we don't have a code.co_qualname in 3.10.
        code_name = code_func.__qualname__
        cinderjit.jit_list_append(
            f"{code_name}@{code_obj.co_filename}:{code_obj.co_firstlineno}"
        )
        cinderjit.lazy_compile(code_func)
        cinderjit.lazy_compile(code_func_nojit)

        py_code_objs = cinderjit.get_jit_list()[1]
        thisfile = os.path.basename(__file__)
        self.assertIn(code_obj.co_firstlineno, py_code_objs[code_name][thisfile])
        code_func()
        if cinderjit.auto_jit_threshold() <= 1:
            self.assertTrue(cinderjit.is_jit_compiled(code_func))
        code_func_nojit()
        self.assertFalse(cinderjit.is_jit_compiled(code_func_nojit))

    def test_change_func_qualname(self) -> None:
        def inner_func():
            return 24

        cinderjit.jit_list_append(
            f"{inner_func.__module__}:{inner_func.__qualname__}_foo"
        )

        self.assertEqual(inner_func(), 24)
        self.assertFalse(cinderjit.is_jit_compiled(inner_func))
        inner_func.__qualname__ += "_foo"
        self.assertEqual(inner_func(), 24)

        if cinderjit.auto_jit_threshold() <= 1:
            self.assertTrue(cinderjit.is_jit_compiled(inner_func))

    def test_precompile_all(self) -> None:
        # Has to be run under a separate process because precompile_all will mess up the
        # other JIT-related tests.
        code = """if 1:
            import cinderjit

            def func():
                return 24

            assert not cinderjit.is_jit_compiled(func)

            # This has to use multiple threads otherwise this will take many minutes to run.
            # It'll be compiling all functions that were loaded and not-yet-run in JitAll
            # mode.
            assert cinderjit.precompile_all(workers=10)
            assert cinderjit.is_jit_compiled(func)

            print(func())
        """
        rc, out, err = assert_python_ok("-X", "jit", "-c", code)
        self.assertEqual(out.strip(), b"24")

    def test_precompile_all_bad_args(self) -> None:
        with self.assertRaises(ValueError):
            cinderjit.precompile_all(workers=-1)
        with self.assertRaises(ValueError):
            cinderjit.precompile_all(workers=200000)

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
                    "jit",
                    "-X",
                    "jit-list-file=jitlist.txt",
                    "mod.py",
                ],
                capture_output=True,
                cwd=tmp,
                encoding=sys.stdout.encoding,
            )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertIn("Continuing on with the JIT disabled", proc.stderr)

    def test_default_parse_error_behavior_append(self) -> None:
        code = textwrap.dedent(
            """
        import cinderjit
        cinderjit.jit_list_append("OH NO")
        """
        )
        jitlist = ""
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
                    "jit",
                    "-X",
                    "jit-list-file=jitlist.txt",
                    "mod.py",
                ],
                capture_output=True,
                cwd=tmp,
                encoding=sys.stdout.encoding,
            )
        self.assertEqual(proc.returncode, 0, proc)

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
                    "jit",
                    "-X",
                    "jit-list-file=jitlist.txt",
                    "-X",
                    "jit-list-fail-on-parse-error",
                    "mod.py",
                ],
                capture_output=True,
                cwd=tmp,
                encoding=sys.stdout.encoding,
            )
        self.assertNotEqual(proc.returncode, 0, proc)
        self.assertIn("Error while parsing line", proc.stderr)
        self.assertIn("in JIT list file", proc.stderr)

    def test_fail_on_parse_error_append(self) -> None:
        code = textwrap.dedent(
            """
        import cinderjit
        cinderjit.jit_list_append("OH NO")
        """
        )
        jitlist = ""
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
                    "jit",
                    "-X",
                    "jit-list-file=jitlist.txt",
                    "-X",
                    "jit-list-fail-on-parse-error",
                    "mod.py",
                ],
                capture_output=True,
                cwd=tmp,
                encoding=sys.stdout.encoding,
            )
        self.assertNotEqual(proc.returncode, 0, proc)
        self.assertIn("Failed to parse new JIT list line", proc.stderr)


if __name__ == "__main__":
    unittest.main()
