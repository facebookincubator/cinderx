# Copyright (c) Meta Platforms, Inc. and affiliates.

import ast
import json
import sys
import unittest
from glob import glob
from os import path
from types import CodeType

from cinderx.compiler.static.pyrefly_compiler import PyreflyCompiler
from cinderx.compiler.static.pyrefly_info import (
    EMPTY_TYPE_INFO,
    Pyrefly,
    PyreflyTypeInfo,
)

from .common import StaticTestBase


class TestPyrefly(Pyrefly):
    def __init__(self, info: PyreflyTypeInfo):
        self.info = info

    def load_type_info(self, module_name: str) -> PyreflyTypeInfo | None:
        return self.info


EMPTY_PYREFLY = TestPyrefly(EMPTY_TYPE_INFO)


class PyreBinderTests(StaticTestBase):
    def test_simple(self):
        self.compile_one(
            "x = 1 + 2",
            EMPTY_TYPE_INFO,
        )

    def test_force_static(self):
        compiler = PyreflyCompiler(static_opt_out=set(), pyrefly=EMPTY_PYREFLY)
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.bit_length()", "foo.py", "foo", 0
        )
        self.assertTrue(static)
        self.assertFalse(strict)
        self.assertIn("<fixed-modules>", code.co_names)

    def test_opt_in_static(self):
        compiler = PyreflyCompiler(static_opt_in={"foo"}, pyrefly=EMPTY_PYREFLY)
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.bit_length()", "foo.py", "foo", 0
        )
        self.assertTrue(static)
        self.assertFalse(strict)
        self.assertIn("<fixed-modules>", code.co_names)

    def test_non_force_static(self):
        compiler = PyreflyCompiler(static_opt_out={"foo"}, pyrefly=EMPTY_PYREFLY)
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.bit_length()", "foo.py", "foo", 0
        )
        self.assertFalse(static)
        self.assertFalse(strict)
        self.assertNotIn("<fixed-modules>", code.co_names)

    def test_non_opt_in(self):
        compiler = PyreflyCompiler(static_opt_in=set(), pyrefly=EMPTY_PYREFLY)
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.bit_length()", "foo.py", "foo", 0
        )
        self.assertFalse(static)
        self.assertFalse(strict)
        self.assertNotIn("<fixed-modules>", code.co_names)
        self.assertNotIn("<fixed-modules>", code.co_names)

    def test_cases(self):
        for f in glob(path.dirname(__file__) + "/pyreflytests/*.py"):
            if f.endswith(".test.py"):
                continue
            filename = path.splitext(f)[0]
            jsonname = filename + ".json"
            testname = filename + ".test.py"
            basename = path.basename(filename)
            if basename == "__init__":
                continue
            modname = f"cinderx.PythonLib.test_cinderx.test_compiler.test_static.pyreflytests.{basename}"
            with self.subTest(f), open(f) as f, open(jsonname) as jsonf:
                data = json.load(jsonf)
                code = self.compile_one(
                    f.read(),
                    PyreflyTypeInfo(data),
                    modname=modname,
                )
                if path.exists(testname):
                    print(testname)
                    with open(testname) as testf:
                        test_source = testf.read()
                    ns = {}
                    exec(compile(test_source, testname, "exec"), ns)
                    ns["verify"](self, code)

                mod = type(sys)(modname)
                mod.__dict__.update(
                    **{
                        "<fixed-modules>": {
                            "__strict__": __import__("__strict__").__dict__,
                            "typing": __import__("typing").__dict__,
                        },
                        "__name__": "__main__",
                    }
                )
                sys.modules[modname] = mod
                exec(code, mod.__dict__)

    def compile_one(
        self,
        code: str,
        type_info: PyreflyTypeInfo,
        modname: str = "<module>",
        optimize: int = 0,
        ast_optimizer_enabled: bool = True,
        enable_patching: bool = False,
    ) -> CodeType:
        compiler = PyreflyCompiler(pyrefly=TestPyrefly(type_info))
        tree = ast.parse(self.clean_code(code))
        return compiler.compile(
            modname,
            f"{modname}.py",
            tree,
            code,
            optimize,
            enable_patching=enable_patching,
        )


if __name__ == "__main__":
    unittest.main()
