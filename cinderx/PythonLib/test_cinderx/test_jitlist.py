# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

# This is in its own file as it modifies the global JIT-list.

import multiprocessing
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import cinderx.jit
from cinderx.test_support import ENCODING, passUnless, skip_unless_jit, subprocess_env


@skip_unless_jit("Tests JIT list behavior")
@passUnless(
    cinderx.jit.get_compile_after_n_calls() is None
    or cinderx.jit.get_compile_after_n_calls() == 0,
    "Expecting functions to compile on first call",
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
        def victim() -> None:
            pass

        # Using `victim` to determine fully-qualified name, before `func` is actually
        # created.  Want to set JIT list before creating function in case functions try
        # to compile on creation.
        cinderx.jit.append_jit_list(
            f"{victim.__module__}:{victim.__qualname__}".replace("victim", "func")
        )

        def func() -> None:
            pass

        def func_nojit() -> None:
            pass

        py_funcs = cinderx.jit.get_jit_list()[0]
        self.assertIn(func.__qualname__, py_funcs[__name__])
        self.assertNotIn(func_nojit.__qualname__, py_funcs[__name__])

        func()
        self.assertTrue(cinderx.jit.is_jit_compiled(func))
        func_nojit()
        self.assertFalse(cinderx.jit.is_jit_compiled(func_nojit))

    def test_py_meth(self) -> None:
        class VictimClass:
            def meth(self) -> None:
                pass

        victim = VictimClass.meth
        cinderx.jit.append_jit_list(
            f"{victim.__module__}:{victim.__qualname__}".replace(
                "VictimClass", "JitClass"
            )
        )

        class JitClass:
            def meth(self) -> None:
                pass

            def meth_nojit(self) -> None:
                pass

        meth = JitClass.meth
        meth_nojit = JitClass.meth_nojit

        py_funcs = cinderx.jit.get_jit_list()[0]
        self.assertIn(meth.__qualname__, py_funcs[__name__])
        self.assertNotIn(meth_nojit.__qualname__, py_funcs[__name__])

        meth(JitClass())
        self.assertTrue(cinderx.jit.is_jit_compiled(meth))
        meth_nojit(JitClass())
        self.assertFalse(cinderx.jit.is_jit_compiled(meth_nojit))

    def test_py_code(self) -> None:
        def victim() -> None:
            pass

        # pyre-ignore[16]: Pyre doesn't know about __code__.
        victim_code = victim.__code__
        victim_name = victim.__qualname__

        # This is _very_ fragile.  We're trying to compute what the line number of
        # `code_func` is going to be, before we create it.
        cinderx.jit.append_jit_list(
            f"{victim_name}@{victim_code.co_filename}:{victim_code.co_firstlineno}".replace(
                "victim", "code_func"
            ).replace(
                f"{victim_code.co_firstlineno}",
                f"{victim_code.co_firstlineno + 18}",
            )
        )

        def code_func() -> None:
            pass

        def code_func_nojit() -> None:
            pass

        # pyre-ignore[16]: Pyre doesn't know about __code__.
        code_obj = code_func.__code__

        # Cheating a little here, because we don't have a code.co_qualname in 3.10.
        code_name = code_func.__qualname__

        py_code_objs = cinderx.jit.get_jit_list()[1]
        thisfile = os.path.basename(__file__)
        self.assertIn(code_name, py_code_objs)
        self.assertIn(code_obj.co_firstlineno, py_code_objs[code_name][thisfile])
        code_func()
        self.assertTrue(cinderx.jit.is_jit_compiled(code_func))
        code_func_nojit()
        self.assertFalse(cinderx.jit.is_jit_compiled(code_func_nojit))

    def test_change_func_qualname(self) -> None:
        # This should be a skipIf decorator, but that makes pyre unhappy for some
        # unknown reason.
        if cinderx.jit.get_compile_after_n_calls() == 0:
            # Test doesn't support functions being compiled ASAP
            return

        def inner_func() -> int:
            return 24

        cinderx.jit.append_jit_list(
            f"{inner_func.__module__}:{inner_func.__qualname__}_foo"
        )

        self.assertEqual(inner_func(), 24)
        self.assertFalse(cinderx.jit.is_jit_compiled(inner_func))

        inner_func.__qualname__ += "_foo"
        self.assertEqual(inner_func(), 24)
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
            env=subprocess_env(),
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(b"42\n", proc.stdout, proc.stdout)

    @staticmethod
    def precompile_all_test() -> None:
        import cinderx.jit

        def func() -> int:
            return 24

        assert not cinderx.jit.is_jit_compiled(func)
        cinderx.jit.lazy_compile(func)
        assert not cinderx.jit.is_jit_compiled(func)

        assert cinderx.jit.precompile_all(workers=2)
        assert cinderx.jit.is_jit_compiled(func)

        assert func() == 24

    def test_precompile_all(self) -> None:
        # Has to be run under a separate process because precompile_all will mess up the
        # other JIT-related tests.
        p = multiprocessing.Process(target=JitListTest.precompile_all_test)
        p.start()
        p.join()
        self.assertEqual(p.exitcode, 0)

    def test_read_jit_list(self) -> None:
        def victim() -> None:
            pass

        entry = f"{victim.__module__}:{victim.__qualname__}".replace("victim", "func")

        with tempfile.NamedTemporaryFile("w+") as jit_list_file:
            jit_list_file.write(entry)
            jit_list_file.flush()
            cinderx.jit.read_jit_list(jit_list_file.name)

        def func() -> int:
            return 35

        def func_nojit() -> int:
            return 47

        entries = cinderx.jit.get_jit_list()[0]
        self.assertIn(func.__module__, entries)
        self.assertIn(func.__qualname__, entries[func.__module__])
        self.assertNotIn(func_nojit.__qualname__, entries[func.__module__])

        self.assertEqual(func(), 35)
        self.assertTrue(cinderx.jit.is_jit_compiled(func))

        self.assertEqual(func_nojit(), 47)
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
        code = """if 1:
        import cinderx.jit
        print("Hello world!")"""
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
                env=subprocess_env(),
            )

        self.assertNotEqual(proc.returncode, 0, proc)

        self.assertIn("Error while parsing line", proc.stderr)
        self.assertIn("in JIT list file", proc.stderr)


if __name__ == "__main__":
    unittest.main()
