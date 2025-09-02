# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

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
import cinderx.jit

from cinderx.test_support import CINDERX_PATH, ENCODING, skip_unless_jit


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
        def func() -> None:
            pass

        def func_nojit() -> None:
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
            def meth(self) -> None:
                pass

            def meth_nojit(self) -> None:
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
        def code_func() -> None:
            pass

        def code_func_nojit() -> None:
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
        def inner_func() -> int:
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

    def test_batch_compile_nested_func(self) -> None:
        root = Path(
            os.path.join(os.path.dirname(__file__), "data/batch_compile_nested_func")
        )
        cmd = [
            sys.executable,
            "-X",
            f"jit-list-file={root / 'jitlist.txt'}",
            "-X",
            "jit-batch-compile-workers=2",
            str(root / "main.py"),
        ]
        proc = subprocess.run(
            cmd,
            cwd=root,
            capture_output=True,
            env={"PYTHONPATH": CINDERX_PATH},
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(b"42\n", proc.stdout, proc.stdout)

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
            assert not cinderx.jit.is_jit_compiled(func)

            assert cinderx.jit.precompile_all(workers=2)
            assert cinderx.jit.is_jit_compiled(func)

            print(func())
        """
        )

        with tempfile.TemporaryDirectory() as tmp:
            dirpath = Path(tmp)
            codepath = dirpath / "mod.py"
            codepath.write_text(code)
            proc = subprocess.run(
                [sys.executable, "mod.py"],
                stdout=subprocess.PIPE,
                cwd=tmp,
                encoding=ENCODING,
                env={"PYTHONPATH": CINDERX_PATH},
            )
        self.assertEqual(proc.returncode, 0, proc)
        self.assertEqual(proc.stdout.strip(), "24")

    def test_read_jit_list(self) -> None:
        def func() -> int:
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
                env={"PYTHONPATH": CINDERX_PATH},
            )
        self.assertNotEqual(proc.returncode, 0, proc)
        self.assertIn("Error while parsing line", proc.stderr)
        self.assertIn("in JIT list file", proc.stderr)


if __name__ == "__main__":
    unittest.main()
