# Copyright (c) Meta Platforms, Inc. and affiliates.

import ast
import unittest
from types import CodeType

from cinderx.compiler.pycodegen import CodeGenerator
from cinderx.compiler.static import Compiler, StaticCodeGenBase
from cinderx.compiler.static.pyrefly_compiler import PyreflyCompiler

from .common import StaticTestBase


class PyreBinderTests(StaticTestBase):
    def test_simple(self):
        code = self.compile_one("x = 1 + 2")

    def test_force_static(self):
        compiler = PyreflyCompiler(static_opt_out=set())
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.is_integer()", "foo.py", "foo", 0
        )
        self.assertTrue(static)
        self.assertFalse(strict)
        self.assertInBytecode(self.find_code(code, "f"), "INVOKE_METHOD")

    def test_non_force_static(self):
        compiler = PyreflyCompiler(static_opt_out={"foo"})
        code, strict, static = compiler.load_compiled_module_from_source(
            "def f(x: int): return x.is_integer()", "foo.py", "foo", 0
        )
        self.assertFalse(static)
        self.assertFalse(strict)
        self.assertNotInBytecode(self.find_code(code, "f"), "INVOKE_METHOD")

    def compile_one(
        self,
        code: str,
        modname: str = "<module>",
        optimize: int = 0,
        ast_optimizer_enabled: bool = True,
        enable_patching: bool = False,
    ) -> CodeType:
        compiler = PyreflyCompiler()
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
