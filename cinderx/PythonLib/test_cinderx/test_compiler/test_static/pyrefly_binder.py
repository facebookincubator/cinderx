# Copyright (c) Meta Platforms, Inc. and affiliates.

import ast
import unittest
from types import CodeType

from cinderx.compiler.static.pyrefly_compiler import PyreflyCompiler
from cinderx.compiler.static.pyrefly_type_binder import PyreflyTypeInfo

from .common import StaticTestBase


EMPTY_TYPE_INFO = PyreflyTypeInfo(
    {
        "type_table": [],
        "locations": [],
    }
)


class PyreBinderTests(StaticTestBase):
    def test_simple(self):
        self.compile_one(
            "x = 1 + 2",
            EMPTY_TYPE_INFO,
        )

    def test_force_static(self):
        compiler = PyreflyCompiler(static_opt_out=set(), type_info=EMPTY_TYPE_INFO)
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.bit_length()", "foo.py", "foo", 0
        )
        self.assertTrue(static)
        self.assertFalse(strict)
        self.assertIn("<fixed-modules>", code.co_names)

    def test_opt_in_static(self):
        compiler = PyreflyCompiler(static_opt_in={"foo"}, type_info=EMPTY_TYPE_INFO)
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.bit_length()", "foo.py", "foo", 0
        )
        self.assertTrue(static)
        self.assertFalse(strict)
        self.assertIn("<fixed-modules>", code.co_names)

    def test_non_force_static(self):
        compiler = PyreflyCompiler(static_opt_out={"foo"}, type_info=EMPTY_TYPE_INFO)
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.bit_length()", "foo.py", "foo", 0
        )
        self.assertFalse(static)
        self.assertFalse(strict)
        self.assertNotIn("<fixed-modules>", code.co_names)

    def test_non_opt_in(self):
        compiler = PyreflyCompiler(static_opt_in=set(), type_info=EMPTY_TYPE_INFO)
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.bit_length()", "foo.py", "foo", 0
        )
        self.assertFalse(static)
        self.assertFalse(strict)
        self.assertNotIn("<fixed-modules>", code.co_names)

    def compile_one(
        self,
        code: str,
        type_info: PyreflyTypeInfo,
        modname: str = "<module>",
        optimize: int = 0,
        ast_optimizer_enabled: bool = True,
        enable_patching: bool = False,
    ) -> CodeType:
        compiler = PyreflyCompiler(type_info=type_info)
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
